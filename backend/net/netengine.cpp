#include "netengine.h"

#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QSysInfo>
#include <QTimer>
#include <QUdpSocket>
#include <QVarLengthArray>

#include <atomic>
#include <cstring>

#include "net/jitterbuffer.h"

using namespace NetProtocol;

namespace {
constexpr int    kBeaconIntervalMs = 1000;
constexpr qint64 kPeerTimeoutMs    = 5000;    // no packet for this long → peer left
constexpr qint64 kHelloIntervalMs  = 300;     // at most one challenge per peer in this time
constexpr int    kMaxPeers         = 16;      // per node
constexpr int    kSilenceHangover  = 40;      // silent packets still sent before pausing (200 ms)
constexpr int    kPacketBytes      = kFramesPerPacket * kBytesPerFrame;
constexpr int    kMaxCaptureQueue  = kSampleRate * kBytesPerFrame;   // 1 s

bool allZero(const QByteArray &pcm)
{
    const char *p = pcm.constData();
    for (qsizetype i = 0; i < pcm.size(); ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}
} // namespace

// State of a node that the audio thread, the worker thread and the UI thread
// share.  Each part has its own lock; none is held while doing I/O.
struct NetNodeShared {
    QMutex            captureMutex;
    QByteArray        captureQueue;          // PCM waiting to be sent
    std::atomic<bool> drainQueued { false };

    QMutex                                 rxMutex;
    QVector<std::shared_ptr<JitterBuffer>> buffers;   // one per sender we hear

    QMutex      statusMutex;
    QVariantMap status;

    std::atomic<bool> listening { false };
};

// ═══════════════════════════════════════════════════════════════════════════
// Worker: everything that touches the network, on its own thread
// ═══════════════════════════════════════════════════════════════════════════

class NetWorker : public QObject {
    Q_OBJECT

public:
    NetWorker(NetEngine *engine, const NetEngine::Options &options)
        : m_engine(engine), m_options(options)
    {
        if (m_options.deviceName.isEmpty())
            m_options.deviceName = QSysInfo::machineHostName();
    }

    std::atomic<quint16> boundPort { 0 };

public slots:
    void start();
    void stop();
    void startNode(const NetworkController::NodeSpec &spec);
    void stopNode(const QString &paName);
    void drainCapture(const QString &paName);

signals:
    void statusChanged(const QString &paName);

private:
    struct Peer {
        quint64      id = 0;
        QHostAddress address;
        quint16      port = 0;
        QString      name;
        bool         listening = false;
        bool         confirmed = false;   // answered our challenge from address:port
        qint64       lastSeen = 0;
        quint64      helloNonce = 0;
        qint64       lastHello = 0;
        ReplayWindow replay;
        std::shared_ptr<JitterBuffer> jitter;
    };

    struct Node {
        NetworkController::NodeSpec    spec;
        std::shared_ptr<NetNodeShared> shared;
        NetCrypto::ChannelKeys         keys;
        quint64  senderId = 0;
        quint64  counter = 0;
        qint64   txTimestamp = 0;
        int      silentPackets = 0;
        QByteArray pending;                       // captured PCM not yet a full packet
        QHash<quint64, Peer> peers;
        QVector<QPair<QHostAddress, quint16>> staticPeers;
        QString  error;
    };

    void readSocket(QUdpSocket *socket);
    void handleDatagram(const QByteArray &data, const QHostAddress &from, quint16 port);
    void tick();
    void joinMulticast();

    QByteArray makePacket(Node &node, PacketType type, const QByteArray &payload);
    void send(const QByteArray &packet, const QHostAddress &to, quint16 port);
    void sendBeacons(Node &node);
    void sendHello(Node &node, Peer &peer, const QHostAddress &to, quint16 port);
    void removePeer(Node &node, quint64 id);
    void publishStatus(Node &node);
    void addStaticPeer(const QString &paName, const QString &entry);

    NetEngine          *m_engine;
    NetEngine::Options  m_options;
    QUdpSocket         *m_data = nullptr;
    QUdpSocket         *m_discovery = nullptr;
    QTimer             *m_timer = nullptr;
    int                 m_ticks = 0;
    QHash<QString, Node> m_nodes;   // by PA name
};

void NetWorker::start()
{
    NetCrypto::init();

    m_data = new QUdpSocket(this);
    // LAN traffic never goes through a proxy.  Without this Qt consults the
    // system proxy settings for every socket, which can mean downloading a
    // PAC file - slow, and pointless for local UDP.
    m_data->setProxy(QNetworkProxy::NoProxy);
    // The well-known port lets peers be reached by address alone.  If it is
    // taken (a second instance on this machine), any port works: peers learn
    // the real one from our packets.
    if (!m_data->bind(QHostAddress::AnyIPv4, m_options.dataPort)
        && !m_data->bind(QHostAddress::AnyIPv4, 0)) {
        qWarning() << "SoundRoot network: cannot bind a UDP socket:" << m_data->errorString();
    }
    boundPort = m_data->localPort();
    m_data->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);        // never leaves the LAN
    m_data->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);   // instances on one machine
    connect(m_data, &QUdpSocket::readyRead, this, [this]() { readSocket(m_data); });

    if (m_options.multicast) {
        m_discovery = new QUdpSocket(this);
        m_discovery->setProxy(QNetworkProxy::NoProxy);
        if (m_discovery->bind(QHostAddress::AnyIPv4, m_options.discoveryPort,
                              QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            connect(m_discovery, &QUdpSocket::readyRead, this,
                    [this]() { readSocket(m_discovery); });
            joinMulticast();
        } else {
            qWarning() << "SoundRoot network: cannot listen for beacons:"
                       << m_discovery->errorString();
        }
    }

    m_timer = new QTimer(this);
    m_timer->setInterval(kBeaconIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &NetWorker::tick);
    m_timer->start();
}

void NetWorker::stop()
{
    const QStringList names = m_nodes.keys();
    for (const auto &name : names)
        stopNode(name);
    if (m_timer) m_timer->stop();
    delete m_data;      m_data = nullptr;
    delete m_discovery; m_discovery = nullptr;
}

void NetWorker::joinMulticast()
{
    if (!m_discovery) return;
    // Joining twice fails harmlessly, so this doubles as "pick up interfaces
    // that appeared since" (Wi-Fi reconnect, resume from sleep).
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto &iface : interfaces) {
        const auto flags = iface.flags();
        if ((flags & QNetworkInterface::IsUp) && (flags & QNetworkInterface::CanMulticast))
            m_discovery->joinMulticastGroup(multicastGroup(), iface);
    }
}

// ── Nodes ────────────────────────────────────────────────────────────────

void NetWorker::startNode(const NetworkController::NodeSpec &spec)
{
    if (m_nodes.contains(spec.paName)) {
        if (m_nodes[spec.paName].spec == spec) return;
        stopNode(spec.paName);   // settings changed: leave and rejoin
    }

    Node node;
    node.spec = spec;
    node.shared = m_engine->sharedNode(spec.paName);
    if (!node.shared) return;   // stopped again before we got here
    node.senderId = NetCrypto::randomU64();
    node.keys = NetCrypto::deriveKeys(spec.channel, spec.passphrase);
    if (!node.keys.isValid())
        node.error = QStringLiteral("Channel name and passphrase are required");
    else if (!m_data || m_data->state() != QAbstractSocket::BoundState)
        node.error = QStringLiteral("Could not open a network port");

    m_nodes.insert(spec.paName, node);
    Node &stored = m_nodes[spec.paName];
    publishStatus(stored);
    if (!stored.error.isEmpty()) return;

    for (const auto &entry : spec.staticPeers)
        addStaticPeer(spec.paName, entry);
    sendBeacons(stored);
}

void NetWorker::addStaticPeer(const QString &paName, const QString &entry)
{
    // "host", "host:port" or an IPv4 address
    QString host = entry.trimmed();
    quint16 port = kDefaultDataPort;
    const auto colon = host.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        const uint p = host.mid(colon + 1).toUInt(&ok);
        if (ok && p > 0 && p < 65536) {
            port = static_cast<quint16>(p);
            host = host.left(colon);
        }
    }
    if (host.isEmpty()) return;

    auto add = [this, paName, port](const QHostAddress &address) {
        auto it = m_nodes.find(paName);
        if (it == m_nodes.end() || address.protocol() != QAbstractSocket::IPv4Protocol) return;
        if (!isLocalNetworkAddress(address)) {
            qWarning() << "SoundRoot network: ignoring peer outside the local network:" << address;
            return;
        }
        it->staticPeers.append({ address, port });
        sendBeacons(it.value());
    };

    const QHostAddress literal(host);
    if (!literal.isNull()) {
        add(literal);
        return;
    }
    QHostInfo::lookupHost(host, this, [add](const QHostInfo &info) {
        const auto addresses = info.addresses();
        for (const auto &a : addresses) {
            if (a.protocol() == QAbstractSocket::IPv4Protocol) { add(a); break; }
        }
    });
}

void NetWorker::stopNode(const QString &paName)
{
    auto it = m_nodes.find(paName);
    if (it == m_nodes.end()) return;
    Node &node = it.value();

    if (node.keys.isValid()) {
        const QByteArray bye = makePacket(node, Bye, {});
        for (const auto &peer : std::as_const(node.peers)) {
            if (peer.confirmed) send(bye, peer.address, peer.port);
        }
    }
    {
        QMutexLocker lock(&node.shared->rxMutex);
        node.shared->buffers.clear();
    }
    {
        QMutexLocker lock(&node.shared->statusMutex);
        node.shared->status.clear();
    }
    {
        QMutexLocker lock(&node.shared->captureMutex);
        node.shared->captureQueue.clear();
    }
    node.shared->drainQueued = false;
    node.shared->listening = false;
    m_nodes.erase(it);
    emit statusChanged(paName);
}

void NetWorker::publishStatus(Node &node)
{
    QVariantList peers;
    for (const auto &peer : std::as_const(node.peers)) {
        if (!peer.confirmed) continue;
        QVariantMap m;
        m[QStringLiteral("name")] = peer.name.isEmpty() ? peer.address.toString() : peer.name;
        m[QStringLiteral("address")] = peer.address.toString();
        m[QStringLiteral("listening")] = peer.listening;
        peers.append(m);
    }
    QVariantMap status;
    status[QStringLiteral("state")] = node.error.isEmpty() ? QStringLiteral("online")
                                                           : QStringLiteral("error");
    status[QStringLiteral("error")] = node.error;
    status[QStringLiteral("port")] = boundPort.load();
    status[QStringLiteral("peers")] = peers;

    {
        QMutexLocker lock(&node.shared->statusMutex);
        if (node.shared->status == status) return;
        node.shared->status = status;
    }
    emit statusChanged(node.spec.paName);
}

// ── Sending ──────────────────────────────────────────────────────────────

QByteArray NetWorker::makePacket(Node &node, PacketType type, const QByteArray &payload)
{
    Header h;
    h.type = type;
    h.channelId = node.keys.channelId;
    h.senderId = node.senderId;
    h.counter = ++node.counter;   // never reused: it is half of the nonce
    return sealPacket(node.keys, h, payload);
}

void NetWorker::send(const QByteArray &packet, const QHostAddress &to, quint16 port)
{
    if (m_data && !packet.isEmpty())
        m_data->writeDatagram(packet, to, port);
}

void NetWorker::sendBeacons(Node &node)
{
    if (!node.keys.isValid() || !node.error.isEmpty()) return;

    QJsonObject info;
    info[QStringLiteral("v")] = 1;
    info[QStringLiteral("name")] = m_options.deviceName;
    info[QStringLiteral("node")] = node.spec.displayName;
    info[QStringLiteral("listen")] = node.shared->listening.load();
    const QByteArray beacon =
        makePacket(node, Beacon, QJsonDocument(info).toJson(QJsonDocument::Compact));

    if (m_options.multicast) {
        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const auto &iface : interfaces) {
            const auto flags = iface.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::CanMulticast))
                continue;
            m_data->setMulticastInterface(iface);
            send(beacon, multicastGroup(), m_options.discoveryPort);
        }
    }
    for (const auto &target : std::as_const(node.staticPeers))
        send(beacon, target.first, target.second);
    // Also straight to the peers we know: keeps the channel alive on
    // networks that drop multicast after a while.
    for (const auto &peer : std::as_const(node.peers)) {
        if (peer.confirmed) send(beacon, peer.address, peer.port);
    }
}

void NetWorker::sendHello(Node &node, Peer &peer, const QHostAddress &to, quint16 port)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (peer.helloNonce != 0 && now - peer.lastHello < kHelloIntervalMs) return;
    peer.helloNonce = NetCrypto::randomU64() | 1;   // never 0
    peer.lastHello = now;
    QByteArray payload(8, '\0');
    memcpy(payload.data(), &peer.helloNonce, 8);
    send(makePacket(node, Hello, payload), to, port);
}

void NetWorker::drainCapture(const QString &paName)
{
    auto it = m_nodes.find(paName);
    if (it == m_nodes.end()) {
        // Audio that was captured while the node was being stopped
        if (const auto shared = m_engine->sharedNode(paName)) {
            QMutexLocker lock(&shared->captureMutex);
            shared->captureQueue.clear();
            shared->drainQueued = false;
        }
        return;
    }
    Node &node = it.value();

    node.shared->drainQueued = false;
    {
        QMutexLocker lock(&node.shared->captureMutex);
        node.pending += node.shared->captureQueue;
        node.shared->captureQueue.clear();
    }
    if (!node.keys.isValid() || !node.error.isEmpty()) {
        node.pending.clear();
        return;
    }

    qsizetype offset = 0;
    for (; node.pending.size() - offset >= kPacketBytes; offset += kPacketBytes) {
        const QByteArray pcm = node.pending.mid(offset, kPacketBytes);
        // The timestamp advances whether or not the packet is sent, so the
        // receivers keep the gaps as silence.
        const qint64 timestamp = node.txTimestamp;
        node.txTimestamp += kFramesPerPacket;

        // Stop sending during silence (after a short tail) instead of
        // streaming 1.5 Mbit/s of zeros to every peer.
        if (allZero(pcm)) {
            if (node.silentPackets > kSilenceHangover) continue;
            ++node.silentPackets;
        } else {
            node.silentPackets = 0;
        }

        QByteArray packet;
        for (const auto &peer : std::as_const(node.peers)) {
            if (!peer.confirmed || !peer.listening) continue;
            if (packet.isEmpty())
                packet = makePacket(node, Audio, encodeAudio(timestamp, pcm));
            send(packet, peer.address, peer.port);
        }
    }
    node.pending.remove(0, offset);
}

// ── Receiving ────────────────────────────────────────────────────────────

void NetWorker::readSocket(QUdpSocket *socket)
{
    while (socket && socket->hasPendingDatagrams()) {
        const QNetworkDatagram dgram = socket->receiveDatagram(2048);
        if (dgram.isValid())
            handleDatagram(dgram.data(), dgram.senderAddress(),
                           static_cast<quint16>(dgram.senderPort()));
    }
}

void NetWorker::handleDatagram(const QByteArray &data, const QHostAddress &from, quint16 port)
{
    Header header;
    if (!decodeHeader(data, &header)) return;
    if (!isLocalNetworkAddress(from)) return;   // LAN only

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // One channel can be joined by several of our nodes
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        Node &node = it.value();
        if (!node.keys.isValid() || node.keys.channelId != header.channelId) continue;
        if (header.senderId == node.senderId) continue;   // our own packet

        QByteArray payload;
        if (!openPacket(node.keys, data, header, &payload)) continue;   // wrong key or forged

        auto peerIt = node.peers.find(header.senderId);
        if (peerIt == node.peers.end()) {
            if (node.peers.size() >= kMaxPeers) continue;
            Peer fresh;
            fresh.id = header.senderId;
            fresh.address = from;
            fresh.port = port;
            fresh.lastSeen = now;
            peerIt = node.peers.insert(header.senderId, fresh);
        }
        Peer &peer = peerIt.value();
        if (!peer.replay.accept(header.counter)) continue;   // replayed

        const bool fromKnownEndpoint = peer.confirmed && peer.address == from && peer.port == port;

        switch (header.type) {
        case Hello:
            // Prove we are live here by echoing the challenge
            send(makePacket(node, HelloAck, payload), from, port);
            if (!fromKnownEndpoint)
                sendHello(node, peer, from, port);
            break;

        case HelloAck: {
            quint64 echoed = 0;
            if (payload.size() == 8) memcpy(&echoed, payload.constData(), 8);
            if (peer.helloNonce == 0 || echoed != peer.helloNonce) break;
            peer.helloNonce = 0;
            peer.confirmed = true;
            peer.address = from;   // also how a peer that changed address is followed
            peer.port = port;
            peer.lastSeen = now;
            if (!peer.jitter) {
                peer.jitter = std::make_shared<JitterBuffer>(kChannels, kSampleRate);
                QMutexLocker lock(&node.shared->rxMutex);
                node.shared->buffers.append(peer.jitter);
            }
            sendBeacons(node);   // let it know about us right away
            publishStatus(node);
            break;
        }

        case Beacon: {
            const QJsonObject info = QJsonDocument::fromJson(payload).object();
            peer.name = info.value(QStringLiteral("name")).toString().left(64);
            peer.listening = info.value(QStringLiteral("listen")).toBool();
            if (fromKnownEndpoint) {
                peer.lastSeen = now;
                publishStatus(node);
            } else {
                // New peer, or a known one at a new address: challenge it
                sendHello(node, peer, from, port);
            }
            break;
        }

        case Audio: {
            // Audio is only taken from peers that answered a challenge: a
            // recording of an old session cannot be played back at us.
            if (!fromKnownEndpoint) {
                sendHello(node, peer, from, port);
                break;
            }
            peer.lastSeen = now;
            AudioPayload audio;
            if (!decodeAudio(payload, &audio)) break;
            if (audio.codec != kCodecPcm16 || audio.channels != kChannels
                || audio.sampleRate != static_cast<quint32>(kSampleRate))
                break;   // a format we do not know (yet)
            const int frames = static_cast<int>(audio.samples.size() / kBytesPerFrame);
            if (frames > 0 && peer.jitter)
                peer.jitter->insert(audio.timestamp,
                                    reinterpret_cast<const int16_t *>(audio.samples.constData()),
                                    frames);
            break;
        }

        case Bye:
            if (fromKnownEndpoint) {
                removePeer(node, peer.id);
                publishStatus(node);
            }
            break;
        }
    }
}

void NetWorker::removePeer(Node &node, quint64 id)
{
    auto it = node.peers.find(id);
    if (it == node.peers.end()) return;
    if (it->jitter) {
        QMutexLocker lock(&node.shared->rxMutex);
        node.shared->buffers.removeAll(it->jitter);
    }
    node.peers.erase(it);
}

void NetWorker::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (++m_ticks % 10 == 0)
        joinMulticast();

    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        Node &node = it.value();
        QVector<quint64> gone;
        for (const auto &peer : std::as_const(node.peers)) {
            if (now - peer.lastSeen > kPeerTimeoutMs) gone.append(peer.id);
        }
        for (quint64 id : std::as_const(gone))
            removePeer(node, id);
        sendBeacons(node);
        publishStatus(node);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// NetEngine: thread-safe front
// ═══════════════════════════════════════════════════════════════════════════

NetEngine::NetEngine() : NetEngine(Options()) {}

NetEngine::NetEngine(const Options &options, QObject *parent)
    : QObject(parent)
{
    m_worker = new NetWorker(this, options);
    m_worker->moveToThread(&m_thread);
    connect(m_worker, &NetWorker::statusChanged, this, &NetEngine::statusChanged);
    m_thread.setObjectName(QStringLiteral("soundroot-net"));
    m_thread.start();
    QMetaObject::invokeMethod(m_worker, &NetWorker::start, Qt::QueuedConnection);
}

NetEngine::~NetEngine()
{
    // Say goodbye to the peers and close the sockets on their own thread
    QMetaObject::invokeMethod(m_worker, &NetWorker::stop, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
}

std::shared_ptr<NetNodeShared> NetEngine::sharedNode(const QString &paName) const
{
    QMutexLocker lock(&m_mutex);
    return m_nodes.value(paName);
}

void NetEngine::startNode(const NodeSpec &spec)
{
    // The shared part of a node is created here, synchronously, so calls
    // that follow at once (setNodeListening, audio) already find it.  It is
    // kept for the engine's lifetime and merely emptied on stop: that makes
    // a stop followed directly by a start safe without blocking the caller.
    std::shared_ptr<NetNodeShared> shared;
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_nodes.find(spec.paName);
        if (it == m_nodes.end())
            it = m_nodes.insert(spec.paName, std::make_shared<NetNodeShared>());
        shared = it.value();
    }
    {
        QMutexLocker lock(&shared->statusMutex);
        if (shared->status.isEmpty())
            shared->status[QStringLiteral("state")] = QStringLiteral("starting");
    }
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, spec]() { worker->startNode(spec); },
                              Qt::QueuedConnection);
    emit statusChanged(spec.paName);
}

void NetEngine::stopNode(const QString &paName)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, paName]() { worker->stopNode(paName); },
                              Qt::QueuedConnection);
}

void NetEngine::setNodeListening(const QString &paName, bool listening)
{
    if (const auto node = sharedNode(paName))
        node->listening = listening;   // goes out with the next beacon
}

void NetEngine::capturedAudio(const QString &paName, const void *data, size_t bytes)
{
    const auto node = sharedNode(paName);
    if (!node || !data || bytes == 0) return;
    {
        QMutexLocker lock(&node->captureMutex);
        // If the worker falls behind, drop the oldest audio rather than
        // letting the delay grow without bound.
        if (node->captureQueue.size() + static_cast<qsizetype>(bytes) > kMaxCaptureQueue)
            node->captureQueue.clear();
        node->captureQueue.append(static_cast<const char *>(data), static_cast<qsizetype>(bytes));
    }
    if (!node->drainQueued.exchange(true)) {
        QMetaObject::invokeMethod(m_worker, [worker = m_worker, paName]() {
            worker->drainCapture(paName);
        }, Qt::QueuedConnection);
    }
}

void NetEngine::playbackAudio(const QString &paName, void *data, size_t bytes)
{
    std::memset(data, 0, bytes);
    const auto node = sharedNode(paName);
    if (!node) return;

    QVector<std::shared_ptr<JitterBuffer>> buffers;
    {
        QMutexLocker lock(&node->rxMutex);
        buffers = node->buffers;
    }
    if (buffers.isEmpty()) return;

    const int frames = static_cast<int>(bytes / kBytesPerFrame);
    const int samples = frames * kChannels;
    auto *out = static_cast<int16_t *>(data);

    if (buffers.size() == 1) {
        buffers.first()->read(out, frames);
        return;
    }

    // Several senders: mix them
    QVarLengthArray<int16_t, 4096> one(samples);
    QVarLengthArray<int32_t, 4096> sum(samples);
    std::fill(sum.begin(), sum.end(), 0);
    for (const auto &buffer : std::as_const(buffers)) {
        buffer->read(one.data(), frames);
        for (int i = 0; i < samples; ++i)
            sum[i] += one[i];
    }
    for (int i = 0; i < samples; ++i)
        out[i] = static_cast<int16_t>(qBound<int32_t>(-32768, sum[i], 32767));
}

QVariantMap NetEngine::nodeStatus(const QString &paName) const
{
    const auto node = sharedNode(paName);
    if (!node) return {};
    QMutexLocker lock(&node->statusMutex);
    return node->status;
}

quint16 NetEngine::dataPort() const
{
    return m_worker->boundPort.load();
}

#include "netengine.moc"
