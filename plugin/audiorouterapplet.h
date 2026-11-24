#pragma once

#include <Plasma/Applet>
#include <KGlobalAccel>
#include <QVariantList>

#include "audiobackend.h"
#include "models/grouplistmodel.h"
#include "models/sinkinputmodel.h"
#include "models/sinkmodel.h"
#include "models/sourcemodel.h"
#include "models/sourceoutputmodel.h"
#include "net/netengine.h"
#include "routing/routingengine.h"

/**
 * @brief The SoundRoot plasmoid's C++ side.
 *
 * Plasma creates this when the widget is added to a panel; QML reaches it
 * through the `Plasmoid` attached object.  It owns the sound-server backend,
 * the routing engine and the network engine, and is a thin, QML-facing layer
 * over them: all state and all routing decisions live in C++, so they work
 * whether or not the popup has ever been opened.
 */
class AudioRouterApplet : public Plasma::Applet {
    Q_OBJECT

    // Models exposed to QML
    Q_PROPERTY(GroupListModel*  groupModel      READ groupModel      CONSTANT)
    Q_PROPERTY(SinkModel*       sinkModel       READ sinkModel       CONSTANT)
    Q_PROPERTY(SinkInputModel*  sinkInputModel  READ sinkInputModel  CONSTANT)
    Q_PROPERTY(SourceModel*       sourceModel       READ sourceModel       CONSTANT)
    Q_PROPERTY(SourceOutputModel* sourceOutputModel READ sourceOutputModel CONSTANT)
    Q_PROPERTY(int masterVolume READ masterVolume NOTIFY masterVolumeChanged)
    /// False while the sound server is unreachable.
    Q_PROPERTY(bool serverConnected READ serverConnected NOTIFY serverConnectedChanged)
    // Per-action shortcuts, read/write directly via KGlobalAccel — same pattern as Plasmoid.globalShortcut
    Q_PROPERTY(QKeySequence nextRoomShortcut READ nextRoomShortcut WRITE setNextRoomShortcut NOTIFY nextRoomShortcutChanged)
    Q_PROPERTY(QKeySequence prevRoomShortcut READ prevRoomShortcut WRITE setPrevRoomShortcut NOTIFY prevRoomShortcutChanged)

public:
    explicit AudioRouterApplet(QObject *parent, const KPluginMetaData &data,
                               const QVariantList &args);
    ~AudioRouterApplet() override;
    void init() override;

    // ── Property accessors ──────────────────────────────────────────────
    GroupListModel  *groupModel()     { return &m_groupModel; }
    SinkModel       *sinkModel()      { return &m_sinkModel; }
    SinkInputModel  *sinkInputModel() { return &m_sinkInputModel; }
    SourceModel       *sourceModel()       { return &m_sourceModel; }
    SourceOutputModel *sourceOutputModel() { return &m_sourceOutputModel; }
    int              masterVolume()   const { return m_audioBackend.defaultSinkVolume(); }
    bool             serverConnected() const { return m_audioBackend.isConnected(); }
    QKeySequence     nextRoomShortcut() const;
    void             setNextRoomShortcut(const QKeySequence &seq);
    QKeySequence     prevRoomShortcut() const;
    void             setPrevRoomShortcut(const QKeySequence &seq);

    // ── Rooms ───────────────────────────────────────────────────────────
    /// Make a room the active one (the previously active room is switched off).
    Q_INVOKABLE void activateRoom(int row);
    Q_INVOKABLE void deactivateRoom(int row);
    Q_INVOKABLE void removeRoom(int row);
    Q_INVOKABLE void activateNextRoom();
    Q_INVOKABLE void activatePrevRoom();
    /// Row of the active room, or -1.
    Q_INVOKABLE int  activeRoom() const;

    // ── Wires (see RoutingEngine for the node key format) ───────────────
    Q_INVOKABLE bool canConnect(int row, const QString &a, const QString &b) const;
    Q_INVOKABLE bool connectNodes(int row, const QString &a, const QString &b);
    Q_INVOKABLE void disconnectNodes(int row, const QString &a, const QString &b);
    Q_INVOKABLE void disconnectNode(int row, const QString &key);
    Q_INVOKABLE void clearRoom(int row);
    Q_INVOKABLE QVariantList roomConnections(int row);
    Q_INVOKABLE QVariantList offlineSources(int row);
    Q_INVOKABLE QVariantList offlineSinks(int row);
    Q_INVOKABLE QVariantList offlineMics(int row);
    Q_INVOKABLE QVariantList offlineRecorders(int row);

    // ── Volumes ─────────────────────────────────────────────────────────
    Q_INVOKABLE void setMasterVolume(int percent);
    Q_INVOKABLE void setSinkVolumeByName(const QString &sinkName, int percent);
    Q_INVOKABLE void toggleSinkMute(const QString &sinkName);
    Q_INVOKABLE void setSinkInputVolume(int inputIndex, int percent);

    // ── Virtual and network nodes ───────────────────────────────────────
    /// Add a virtual node to a room. Returns the generated PA name.
    Q_INVOKABLE QString addVirtualSink(int row, const QString &displayName);
    Q_INVOKABLE void removeVirtualSink(int row, const QString &paName);
    /// [{displayName, paName, loopbackTargets, networkEnabled, channel,
    ///   passphrase, staticPeers}]
    Q_INVOKABLE QVariantList groupVirtualSinks(int row) const;
    /// Change name and network settings of a node; see RoutingEngine.
    Q_INVOKABLE void configureVirtualSink(int row, const QString &paName,
                                          const QVariantMap &config);
    /// {state, error, port, peers: [{name, address, listening}]} of a
    /// running network node; empty when it is not running.
    Q_INVOKABLE QVariantMap networkStatus(const QString &paName) const;
    Q_INVOKABLE QString generatePassphrase() const;

signals:
    void masterVolumeChanged(int volumePercent);
    void serverConnectedChanged();
    void nextRoomShortcutChanged();
    void prevRoomShortcutChanged();
    /// The wires, ghost nodes or stream bindings of some room changed.
    void connectionsChanged();
    void virtualSinksChanged();
    void networkStatusChanged(const QString &paName);

private:
    NetEngine          m_netEngine;      // first: outlives everything that uses it
    PulseAudioBackend  m_audioBackend;
    GroupListModel     m_groupModel;
    SinkModel          m_sinkModel;
    SinkInputModel     m_sinkInputModel;
    SourceModel        m_sourceModel;
    SourceOutputModel  m_sourceOutputModel;
    RoutingEngine      m_engine;
    QAction           *m_nextAction = nullptr;
    QAction           *m_prevAction = nullptr;
};
