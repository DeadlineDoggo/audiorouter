#pragma once

#include <Plasma/Applet>
#include <KGlobalAccel>
#include <QTimer>

#include "audiobackend.h"
#include "models/grouplistmodel.h"
#include "models/routelistmodel.h"
#include "models/sinkinputmodel.h"
#include "models/sinkmodel.h"

/**
 * @brief C++ backend for the Audio Router plasmoid.
 *
 * Extends Plasma::Applet so Plasma loads it automatically when the widget
 * is added to a panel. QML accesses this object as `plasmoid` and can
 * reach the models/actions through properties defined here.
 *
 * Usage in QML:
 *   Plasmoid.nativeInterface.groupModel
 *   Plasmoid.nativeInterface.applyGroup(index)
 */
class AudioRouterApplet : public Plasma::Applet {
    Q_OBJECT

    // Models exposed to QML
    Q_PROPERTY(GroupListModel*  groupModel      READ groupModel      CONSTANT)
    Q_PROPERTY(RouteListModel*  routeModel      READ routeModel      CONSTANT)
    Q_PROPERTY(SinkModel*       sinkModel       READ sinkModel       CONSTANT)
    Q_PROPERTY(SinkInputModel*  sinkInputModel  READ sinkInputModel  CONSTANT)
    Q_PROPERTY(int masterVolume READ masterVolume NOTIFY masterVolumeChanged)
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
    RouteListModel  *routeModel()     { return &m_routeModel; }
    SinkModel       *sinkModel()      { return &m_sinkModel; }
    SinkInputModel  *sinkInputModel() { return &m_sinkInputModel; }
    int              masterVolume()   const { return m_audioBackend.defaultSinkVolume(); }
    QKeySequence     nextRoomShortcut() const;
    void             setNextRoomShortcut(const QKeySequence &seq);
    QKeySequence     prevRoomShortcut() const;
    void             setPrevRoomShortcut(const QKeySequence &seq);

    // ── QML-invokable actions ───────────────────────────────────────────
    Q_INVOKABLE void applyGroup(int groupIndex);
    Q_INVOKABLE void deactivateGroup(int groupIndex);
    Q_INVOKABLE void applyAllActiveGroups();
    Q_INVOKABLE void setMasterVolume(int percent);
    Q_INVOKABLE void setSinkVolumeByName(const QString &sinkName, int percent);
    Q_INVOKABLE void toggleSinkMute(const QString &sinkName);
    /// Direct per-stream move: PA sinkInputIndex → named sink.
    Q_INVOKABLE void moveInput(int inputIndex, const QString &sinkName);
    /// Route a specific stream to one or more sinks (combine-sink for multi).
    Q_INVOKABLE void routeStreamToSinks(int inputIndex, const QStringList &sinkNames);
signals:
    void nextRoomActivated();
    void prevRoomActivated();
    void masterVolumeChanged(int volumePercent);
    void nextRoomShortcutChanged();
    void prevRoomShortcutChanged();
    /// Emitted when a new stream appears while a room is active.
    /// QML should rebuild connections from model and re-apply routing.
    void reapplyRequested();

private:
    void setupConnections();

    PulseAudioBackend  m_audioBackend;
    GroupListModel     m_groupModel;
    RouteListModel     m_routeModel;
    SinkModel          m_sinkModel;
    SinkInputModel     m_sinkInputModel;
    QTimer             m_reapplyTimer;
    QAction           *m_nextAction = nullptr;
    QAction           *m_prevAction = nullptr;
};
