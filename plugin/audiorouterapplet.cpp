#include "audiorouterapplet.h"

#include <QDebug>
#include <QAction>
#include <QKeySequence>
#include <KLocalizedString>
#include <KActionCollection>
#include <KGlobalAccel>

AudioRouterApplet::AudioRouterApplet(QObject *parent,
                                     const KPluginMetaData &data,
                                     const QVariantList &args)
    : Plasma::Applet(parent, data, args)
    , m_routeModel(&m_groupModel)
    , m_sinkModel(&m_audioBackend)
    , m_sinkInputModel(&m_audioBackend)
{
    setupConnections();
    m_groupModel.load();
    m_audioBackend.initialize();

    // Debounced re-apply signal: when new audio streams appear, tell QML
    // to rebuild ordinal mapping and re-route (avoids stale ordinal swaps).
    m_reapplyTimer.setSingleShot(true);
    m_reapplyTimer.setInterval(1000);
    connect(&m_reapplyTimer, &QTimer::timeout,
            this, &AudioRouterApplet::reapplyRequested);

    // Create an action collection specifically for this applet and register
    // user-configurable actions there so they appear under the applet in
    // System Settings → Shortcuts.
    auto *coll = new KActionCollection(this, QStringLiteral("org.kde.plasma.soundroot"));
    coll->setComponentDisplayName(i18n("SoundRoot"));
    QAction *nextAction = coll->addAction(QStringLiteral("nextRoom"));
    nextAction->setObjectName(QStringLiteral("nextRoom"));
    nextAction->setText(i18n("Activate Next Audio Room"));
    // Provide a sensible default so the Shortcuts UI shows the action; users can rebind it.
    const QList<QKeySequence> nextDefaults = { QKeySequence("Ctrl+Alt+Right") };
    KGlobalAccel::self()->setDefaultShortcut(nextAction, nextDefaults);
    KGlobalAccel::self()->setShortcut(nextAction, nextDefaults);
    connect(nextAction, &QAction::triggered, this, &AudioRouterApplet::nextRoomActivated);

    QAction *prevAction = coll->addAction(QStringLiteral("prevRoom"));
    prevAction->setObjectName(QStringLiteral("prevRoom"));
    prevAction->setText(i18n("Activate Previous Audio Room"));
    const QList<QKeySequence> prevDefaults = { QKeySequence("Ctrl+Alt+Left") };
    KGlobalAccel::self()->setDefaultShortcut(prevAction, prevDefaults);
    KGlobalAccel::self()->setShortcut(prevAction, prevDefaults);
    connect(prevAction, &QAction::triggered, this, &AudioRouterApplet::prevRoomActivated);
}

AudioRouterApplet::~AudioRouterApplet()
{
    m_audioBackend.shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
// QML-invokable actions (same logic as the old Application class)
// ═════════════════════════════════════════════════════════════════════════════

void AudioRouterApplet::applyGroup(int groupIndex)
{
    auto *group = m_groupModel.groupAt(groupIndex);
    if (!group) return;

    // Enforce single-room: deactivate every OTHER active group first
    for (int i = 0; i < m_groupModel.rowCount(); ++i) {
        if (i != groupIndex && m_groupModel.isGroupActive(i))
            deactivateGroup(i);
    }

    for (const auto &route : group->routes)
        m_audioBackend.applyRoute(route.sourceAppName,
                                  route.outputSinkNames,
                                  route.volumePercent);

    m_groupModel.setGroupActive(groupIndex, true);
}

void AudioRouterApplet::deactivateGroup(int groupIndex)
{
    auto *group = m_groupModel.groupAt(groupIndex);
    if (!group) return;

    for (const auto &route : group->routes)
        m_audioBackend.restoreRoute(route.sourceAppName);

    for (const auto &route : group->routes)
        m_audioBackend.removeRoute(route.sourceAppName);

    m_audioBackend.cleanupStreamCombineSinks();

    m_groupModel.setGroupActive(groupIndex, false);
}

void AudioRouterApplet::applyAllActiveGroups()
{
    for (int i = 0; i < m_groupModel.rowCount(); ++i) {
        auto *group = m_groupModel.groupAt(i);
        if (group && group->active)
            applyGroup(i);
    }
}

void AudioRouterApplet::setMasterVolume(int percent)
{
    m_audioBackend.setDefaultSinkVolume(percent);
}

void AudioRouterApplet::setSinkVolumeByName(const QString &sinkName, int percent)
{
    m_audioBackend.setSinkVolumeByName(sinkName, percent);
}

void AudioRouterApplet::toggleSinkMute(const QString &sinkName)
{
    m_audioBackend.toggleSinkMuteByName(sinkName);
}

void AudioRouterApplet::moveInput(int inputIndex, const QString &sinkName)
{
    m_audioBackend.moveInputToSink(static_cast<uint32_t>(inputIndex), sinkName);
}

void AudioRouterApplet::routeStreamToSinks(int inputIndex, const QStringList &sinkNames)
{
    m_audioBackend.routeStreamToSinks(static_cast<uint32_t>(inputIndex), sinkNames);
}

// ═════════════════════════════════════════════════════════════════════════════
// Internals
// ═════════════════════════════════════════════════════════════════════════════

void AudioRouterApplet::setupConnections()
{
    connect(&m_audioBackend, &PulseAudioBackend::serverConnected,
            this, [this]() {
                // Delay long enough for the initial sink/stream list to be
                // populated via the async PA callbacks before we try to apply.
                QTimer::singleShot(600, this, &AudioRouterApplet::applyAllActiveGroups);
            });

    // Only re-apply when a genuinely NEW stream appears, not on every
    // move/volume CHANGE that we ourselves issue – that would create an
    // infinite re-apply loop.
    connect(&m_audioBackend, &PulseAudioBackend::sinkInputAdded,
            this, [this]() {
                // Only signal QML if a room is actually active
                for (int i = 0; i < m_groupModel.rowCount(); ++i) {
                    if (m_groupModel.isGroupActive(i)) {
                        m_reapplyTimer.start();
                        return;
                    }
                }
            });

    connect(&m_audioBackend, &PulseAudioBackend::defaultSinkVolumeChanged,
            this, &AudioRouterApplet::masterVolumeChanged);

    connect(&m_audioBackend, &PulseAudioBackend::errorOccurred,
            this, [](const QString &msg) {
                qWarning() << "SoundRoot plasmoid:" << msg;
            });
}

K_PLUGIN_CLASS_WITH_JSON(AudioRouterApplet, "../package/metadata.json")

#include "audiorouterapplet.moc"
