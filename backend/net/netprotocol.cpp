#include "netprotocol.h"

#include <QtEndian>

namespace NetProtocol {

namespace {
const char kMagic[4] = { 'S', 'R', 'N', '1' };

QByteArray encodeHeader(const Header &h)
{
    QByteArray out(kHeaderBytes, '\0');
    char *p = out.data();
    memcpy(p, kMagic, 4);
    p[4] = static_cast<char>(h.type);
    p[5] = static_cast<char>(h.flags);
    memcpy(p + 8, h.channelId.constData(),
           static_cast<size_t>(qMin<qsizetype>(h.channelId.size(), NetCrypto::kChannelIdBytes)));
    qToBigEndian<quint64>(h.senderId, p + 16);
    qToBigEndian<quint64>(h.counter, p + 24);
    return out;
}

QByteArray nonceFor(const Header &h)
{
    QByteArray nonce(NetCrypto::kNonceBytes, '\0');
    qToBigEndian<quint64>(h.senderId, nonce.data());
    qToBigEndian<quint64>(h.counter, nonce.data() + 8);
    return nonce;
}
} // namespace

bool decodeHeader(const QByteArray &datagram, Header *header)
{
    if (!header || datagram.size() < kHeaderBytes + NetCrypto::kTagBytes) return false;
    const char *p = datagram.constData();
    if (memcmp(p, kMagic, 4) != 0) return false;
    header->type = static_cast<PacketType>(static_cast<quint8>(p[4]));
    header->flags = static_cast<quint8>(p[5]);
    header->channelId = QByteArray(p + 8, NetCrypto::kChannelIdBytes);
    header->senderId = qFromBigEndian<quint64>(p + 16);
    header->counter = qFromBigEndian<quint64>(p + 24);
    return true;
}

QByteArray sealPacket(const NetCrypto::ChannelKeys &keys, const Header &header,
                      const QByteArray &payload)
{
    const QByteArray head = encodeHeader(header);
    const QByteArray sealed = NetCrypto::seal(keys.aeadKey, nonceFor(header), head, payload);
    if (sealed.isEmpty()) return {};
    return head + sealed;
}

bool openPacket(const NetCrypto::ChannelKeys &keys, const QByteArray &datagram,
                const Header &header, QByteArray *payload)
{
    if (datagram.size() < kHeaderBytes + NetCrypto::kTagBytes) return false;
    return NetCrypto::open(keys.aeadKey, nonceFor(header), datagram.left(kHeaderBytes),
                           datagram.mid(kHeaderBytes), payload);
}

QByteArray encodeAudio(qint64 timestamp, const QByteArray &pcm)
{
    QByteArray out(16, '\0');
    char *p = out.data();
    p[0] = static_cast<char>(kCodecPcm16);
    p[1] = static_cast<char>(kChannels);
    qToBigEndian<quint32>(kSampleRate, p + 4);
    qToBigEndian<qint64>(timestamp, p + 8);
    out += pcm;
    return out;
}

bool decodeAudio(const QByteArray &payload, AudioPayload *audio)
{
    if (!audio || payload.size() < 16) return false;
    const char *p = payload.constData();
    audio->codec = static_cast<quint8>(p[0]);
    audio->channels = static_cast<quint8>(p[1]);
    audio->sampleRate = qFromBigEndian<quint32>(p + 4);
    audio->timestamp = qFromBigEndian<qint64>(p + 8);
    audio->samples = payload.mid(16);
    return true;
}

bool ReplayWindow::accept(quint64 counter)
{
    if (!m_any) {
        m_any = true;
        m_highest = counter;
        m_seen = 1;
        return true;
    }
    if (counter > m_highest) {
        const quint64 shift = counter - m_highest;
        m_seen = shift >= 64 ? 0 : (m_seen << shift);
        m_seen |= 1;
        m_highest = counter;
        return true;
    }
    const quint64 behind = m_highest - counter;
    if (behind >= 64) return false;
    const quint64 bit = quint64(1) << behind;
    if (m_seen & bit) return false;
    m_seen |= bit;
    return true;
}

bool isLocalNetworkAddress(const QHostAddress &address)
{
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);   // also unwraps ::ffff:a.b.c.d
    if (!isV4)
        return address.isLoopback() || address.isLinkLocal() || address.isUniqueLocalUnicast();

    const QHostAddress a(v4);
    static const QPair<QHostAddress, int> ranges[] = {
        { QHostAddress(QStringLiteral("127.0.0.0")), 8 },
        { QHostAddress(QStringLiteral("10.0.0.0")), 8 },
        { QHostAddress(QStringLiteral("172.16.0.0")), 12 },
        { QHostAddress(QStringLiteral("192.168.0.0")), 16 },
        { QHostAddress(QStringLiteral("169.254.0.0")), 16 },
        { QHostAddress(QStringLiteral("100.64.0.0")), 10 },   // CGNAT, used by mesh VPNs
    };
    for (const auto &range : ranges) {
        if (a.isInSubnet(range.first, range.second)) return true;
    }
    return false;
}

} // namespace NetProtocol
