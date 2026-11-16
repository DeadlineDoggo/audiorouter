#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVariantMap>

#include <memory>

#include "audiotypes.h"
#include "net/netprotocol.h"
#include "routing/routingengine.h"

class NetWorker;
struct NetNodeShared;

/**
 * @brief Network virtual audio nodes: shared, encrypted audio channels
 *        between SoundRoot instances on a local network.
 *
 * Every node joins one channel.  What is played into the node here is sent
 * to all other devices on the channel; what they send is mixed and played
 * here.  Devices find each other with multicast beacons (or a list of
 * addresses) and talk directly to each other over UDP.
 *
 * All sockets and timers live on a worker thread, so neither the UI thread
 * nor the audio thread ever waits on the network:
 *
 *   audio thread ──capturedAudio()──► queue ──► worker ──► UDP
 *   UDP ──► worker ──► jitter buffer per sender ──playbackAudio()──► audio thread
 *
 * The public methods may be called from any thread.
 */
class NetEngine : public QObject, public NetworkController, public AudioTap {
    Q_OBJECT

public:
    struct Options {
        quint16 dataPort      = NetProtocol::kDefaultDataPort;   ///< 0 = any free port
        quint16 discoveryPort = NetProtocol::kDiscoveryPort;
        bool    multicast     = true;    ///< announce and listen for beacons
        QString deviceName;              ///< shown to peers; host name when empty
    };

    NetEngine();
    explicit NetEngine(const Options &options, QObject *parent = nullptr);
    ~NetEngine() override;

    // ── NetworkController ───────────────────────────────────────────────
    void startNode(const NodeSpec &spec) override;
    void stopNode(const QString &paName) override;
    void setNodeListening(const QString &paName, bool listening) override;

    // ── AudioTap (audio thread) ─────────────────────────────────────────
    void capturedAudio(const QString &paName, const void *data, size_t bytes) override;
    void playbackAudio(const QString &paName, void *data, size_t bytes) override;

    /// {state: "starting"|"online"|"error", error, port,
    ///  peers: [{name, address, listening}]} - empty when the node is not running.
    QVariantMap nodeStatus(const QString &paName) const;
    /// UDP port in use; 0 until the worker has bound its socket.
    quint16 dataPort() const;

signals:
    void statusChanged(const QString &paName);

private:
    friend class NetWorker;
    std::shared_ptr<NetNodeShared> sharedNode(const QString &paName) const;

    QThread    m_thread;
    NetWorker *m_worker = nullptr;

    mutable QMutex m_mutex;   // guards m_nodes
    QHash<QString, std::shared_ptr<NetNodeShared>> m_nodes;
};
