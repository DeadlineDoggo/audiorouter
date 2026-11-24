#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>

#include "audiotypes.h"
#include "routing/streambinder.h"

class GroupListModel;
struct AudioGroup;
struct VirtualSinkDef;

/// What the routing engine needs from the network layer (NetEngine).
class NetworkController {
public:
    struct NodeSpec {
        QString     paName;
        QString     displayName;
        QString     channel;
        QString     passphrase;
        QStringList staticPeers;
        bool operator==(const NodeSpec &other) const = default;
    };

    virtual ~NetworkController() = default;
    /// Join the channel.  Idempotent; a changed spec restarts the node.
    virtual void startNode(const NodeSpec &spec) = 0;
    virtual void stopNode(const QString &paName) = 0;
    /// Whether this device plays the channel's audio (its OUT side is wired).
    virtual void setNodeListening(const QString &paName, bool listening) = 0;
};

/**
 * @brief Owns the routing state and keeps the sound server in line with it.
 *
 * The configuration (rooms, routes, virtual nodes) lives in GroupListModel;
 * this class edits it on behalf of the UI and applies the active room.
 *
 * Applying is a reconcile pass: it compares what the active room wants with
 * what the server reports and issues only the missing steps.  It runs
 * whenever the server's sinks or streams change, so
 *  - streams that start later are routed,
 *  - devices plugged in later are picked up,
 *  - anything the server undid is redone,
 * without timers or one-shot "apply" calls that can miss.
 *
 * Node keys used with the UI:
 *   "input:<index>"    a live application stream
 *   "app:<route key>"  a saved source that is not playing right now
 *   "sink:<name>"      an output device (present or not)
 *   "vsinkin:<pa>"     a virtual node's input side
 *   "vsinkout:<pa>"    a virtual node's output side
 *   "mic:<name>"       a microphone (present or not)
 *   "rec:<index>"      a live recording application
 *   "recapp:<key>"     a saved recording application that is not recording
 *
 * Wires, always written from the source side:
 *   input:/app:  → sink:/vsinkin:   the application plays there
 *   vsinkout:    → sink:/vsinkin:   the node's output is played there
 *   mic:         → vsinkin:         the microphone plays into the node
 *   mic:         → rec:/recapp:     the application records the microphone
 *   vsinkout:    → rec:/recapp:     the application records the node (its
 *                                   virtual microphone); local nodes only
 * A recording application has one wire at most: a new one replaces it.
 */
class RoutingEngine : public QObject {
    Q_OBJECT

public:
    RoutingEngine(AudioBackend *backend, GroupListModel *groups, QObject *parent = nullptr);

    void setNetworkController(NetworkController *net) { m_net = net; }

    // ── Room activation ─────────────────────────────────────────────────
    /// Activate a room; any other active room is deactivated.
    void activateRoom(int row);
    void deactivateRoom(int row);
    /// Deactivate (if needed) and delete a room.
    void removeRoom(int row);
    /// Step to the next (+1) or previous (-1) room after the active one.
    void activateRelative(int step);

    // ── Wires ───────────────────────────────────────────────────────────
    bool canConnect(int row, const QString &a, const QString &b) const;
    bool connectNodes(int row, const QString &a, const QString &b);
    void disconnectNodes(int row, const QString &a, const QString &b);
    /// Remove every wire attached to a node.
    void disconnectNode(int row, const QString &key);
    void clearRoom(int row);

    /// [{from, to}] with from on the source side.
    QVariantList connections(int row);
    /// [{key, appName, mediaName}] saved sources that are not playing.
    QVariantList offlineSources(int row);
    /// [{name, description}] wired outputs that are not present.
    QVariantList offlineSinks(int row);
    /// [{key, description}] wired microphones that are not present.
    QVariantList offlineMics(int row);
    /// [{key, appName, mediaName}] wired recording applications that are
    /// not recording right now.
    QVariantList offlineRecorders(int row);

    // ── Virtual nodes ───────────────────────────────────────────────────
    QString addVirtualSink(int row, const QString &displayName);
    void    removeVirtualSink(int row, const QString &paName);
    /// Rename / change network settings.  Keys: displayName, networkEnabled,
    /// channel, passphrase, staticPeers.
    void    configureVirtualSink(int row, const QString &paName, const QVariantMap &config);
    QVariantList virtualSinks(int row) const;

    /// Run a reconcile pass soon (coalesced).
    void scheduleReconcile();
    /// Run it now.  Public for tests.
    void reconcile();

signals:
    /// Wires, bindings or ghost nodes of some room changed.
    void connectionsChanged();
    void virtualSinksChanged();

private:
    struct Endpoints {
        enum Kind { Invalid, Route, Loopback, MicInput, Capture } kind = Invalid;
        // Route:    input:/app: key        → sink name
        // Loopback: virtual node pa name   → sink name
        // MicInput: microphone source name → virtual node pa name
        // Capture:  source name (a microphone or a node's virtual
        //           microphone)            → rec:/recapp: key
        QString source;
        QString target;
    };
    struct MoveAttempts { QString target; int count = 0; qint64 since = 0; };

    Endpoints   classify(const QString &a, const QString &b) const;
    AudioGroup *group(int row) const;
    AudioGroup *activeGroup() const;
    /// Bindings of a room for the current streams (cached, sticky).
    const StreamBinder::Bindings &bindingsFor(const AudioGroup &g);
    /// Route key behind a source node key; creates the route for an unrouted
    /// live stream when @p create is set.  Empty when there is none.
    QString routeKeyFor(AudioGroup &g, const QString &sourceKey, bool create);
    /// Bindings of a room's recording applications for the current streams.
    const StreamBinder::Bindings &captureBindingsFor(const AudioGroup &g);
    /// The same as routeKeyFor, for a rec:/recapp: key.
    QString captureKeyFor(AudioGroup &g, const QString &recKey, bool create);
    /// Remove wires of recording applications that record @p sourceName.
    static void dropCapturesFrom(AudioGroup &g, const QString &sourceName);
    bool    wouldLoop(const AudioGroup &g, const QString &fromPa, const QString &toSink) const;
    void    roomEdited(int row);
    void    dropUnusedRoutes(AudioGroup &g);

    void onServerDisconnected();
    void notifyIfTopologyChanged();
    void rememberSinkLabels();
    bool tryMove(const PASinkInputInfo &stream, uint32_t sinkIndex, const QString &sinkName);
    /// Counts a move of a stream to @p target; false once the stream has
    /// been moved there too often lately (something keeps moving it back).
    bool allowMove(QHash<uint32_t, MoveAttempts> &attempts, uint32_t stream,
                   const QString &appName, const QString &target);
    void reconcileRecorders(const AudioGroup *room);
    void stopNetworkNode(const QString &paName);

    AudioBackend      *m_backend;
    GroupListModel    *m_groups;
    NetworkController *m_net = nullptr;

    QTimer m_reconcileTimer;   // coalesces change bursts
    QTimer m_watchdogTimer;    // slow safety pass while a room is active

    QHash<QString, StreamBinder::Bindings> m_bindings;         // room id → bindings
    QHash<QString, StreamBinder::Bindings> m_captureBindings;  // room id → recorder bindings
    QString m_topologySignature;   // streams + sinks the UI was last told about

    // What we created on the server
    QSet<QString>               m_virtualSinks;     // null-sinks
    QHash<QString, NetworkController::NodeSpec> m_netNodes;   // started network nodes
    QSet<QString>               m_combines;         // combine-sink names
    QSet<uint32_t>              m_managedStreams;   // streams we routed
    QSet<uint32_t>              m_managedRecorders; // recording applications we moved

    // Guard against fighting the server over a stream it keeps moving back.
    // Playback and recording streams are numbered separately.
    QHash<uint32_t, MoveAttempts> m_moveAttempts;
    QHash<uint32_t, MoveAttempts> m_recorderMoveAttempts;
    QElapsedTimer m_clock;
};
