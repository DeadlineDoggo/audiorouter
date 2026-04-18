#include "audiorouterapplet.h"

#include <QDebug>
#include <QAction>
#include <QKeySequence>
#include <KLocalizedString>
#include <KGlobalAccel>
#include <KActionCollection>

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
}

AudioRouterApplet::~AudioRouterApplet()
{
    m_audioBackend.shutdown();
}

void AudioRouterApplet::init()
{
    Plasma::Applet::init();

    // Register room-switching global shortcuts INSIDE init() so this runs
    // AFTER Plasma has already called setGlobalShortcut() for the applet's
    // "activate widget" shortcut.  If we register here instead of in the
    // constructor, our KGlobalAccel component save won't clobber the
    // "activate widget N" entry that Plasma just wrote to kglobalshortcutsrc.
    auto *coll = new KActionCollection(this, QStringLiteral("org.kde.plasma.soundroot"));
    coll->setComponentDisplayName(i18n("SoundRoot"));

    m_nextAction = coll->addAction(QStringLiteral("nextRoom"));
    m_nextAction->setObjectName(QStringLiteral("nextRoom"));
    m_nextAction->setText(i18n("Activate Next Audio Room"));
    KGlobalAccel::self()->setDefaultShortcut(m_nextAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Right")) });
    KGlobalAccel::self()->setShortcut(m_nextAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Right")) });
    connect(m_nextAction, &QAction::triggered, this, &AudioRouterApplet::nextRoomActivated);

    m_prevAction = coll->addAction(QStringLiteral("prevRoom"));
    m_prevAction->setObjectName(QStringLiteral("prevRoom"));
    m_prevAction->setText(i18n("Activate Previous Audio Room"));
    KGlobalAccel::self()->setDefaultShortcut(m_prevAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Left")) });
    KGlobalAccel::self()->setShortcut(m_prevAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Left")) });
    connect(m_prevAction, &QAction::triggered, this, &AudioRouterApplet::prevRoomActivated);
}

// ═════════════════════════════════════════════════════════════════════════════
// Shortcut properties (read/write via KGlobalAccel — mirrors globalShortcut)
// ═════════════════════════════════════════════════════════════════════════════

QKeySequence AudioRouterApplet::nextRoomShortcut() const
{
    if (!m_nextAction) return QKeySequence();
    const auto seqs = KGlobalAccel::self()->shortcut(m_nextAction);
    return seqs.isEmpty() ? QKeySequence() : seqs.first();
}

void AudioRouterApplet::setNextRoomShortcut(const QKeySequence &seq)
{
    if (!m_nextAction) return;
    KGlobalAccel::self()->setShortcut(m_nextAction, { seq }, KGlobalAccel::NoAutoloading);
    emit nextRoomShortcutChanged();
}

QKeySequence AudioRouterApplet::prevRoomShortcut() const
{
    if (!m_prevAction) return QKeySequence();
    const auto seqs = KGlobalAccel::self()->shortcut(m_prevAction);
    return seqs.isEmpty() ? QKeySequence() : seqs.first();
}

void AudioRouterApplet::setPrevRoomShortcut(const QKeySequence &seq)
{
    if (!m_prevAction) return;
    KGlobalAccel::self()->setShortcut(m_prevAction, { seq }, KGlobalAccel::NoAutoloading);
    emit prevRoomShortcutChanged();
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
