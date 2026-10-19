#include "audiorouterapplet.h"

#include <QDebug>
#include <QAction>
#include <QKeySequence>
#include <QUuid>
#include <KLocalizedString>
#include <KGlobalAccel>
#include <KActionCollection>

#include "../backend/data/audiogroup.h"

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

    // Create virtual sinks and their loopbacks.  Both calls are idempotent
    // (re-applying an active room creates nothing twice), and the backend
    // defers the loopbacks until the null-sink has finished loading.  Streams
    // routed into a virtual sink are re-applied once it appears (see
    // virtualSinkAppeared in setupConnections).
    for (const auto &vs : group->virtualSinks) {
        m_audioBackend.createVirtualSink(vs.paName, vs.displayName);
        m_audioBackend.setVirtualSinkLoopbacks(vs.paName, vs.loopbackTargets);
    }

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

    // Destroy virtual sinks (this also tears down their loopbacks)
    for (const auto &vs : group->virtualSinks)
        m_audioBackend.destroyVirtualSink(vs.paName);

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
    // Keep the stream's current volume when it is moved onto a combine-sink
    int volume = 100;
    const auto inputs = m_audioBackend.availableSinkInputs();
    for (const auto &si : inputs) {
        if (static_cast<int>(si.index) == inputIndex) {
            volume = si.volumePercent;
            break;
        }
    }
    m_audioBackend.routeStreamToSinks(static_cast<uint32_t>(inputIndex), sinkNames, volume);
}

void AudioRouterApplet::setSinkInputVolume(int inputIndex, int percent)
{
    const auto inputs = m_audioBackend.availableSinkInputs();
    uint8_t channels = 2;
    for (const auto &si : inputs) {
        if (static_cast<int>(si.index) == inputIndex) {
            channels = si.channels;
            break;
        }
    }
    m_audioBackend.setSinkInputVolume(static_cast<uint32_t>(inputIndex), percent, channels);
}

// ═════════════════════════════════════════════════════════════════════════════
// Virtual sink management
// ═════════════════════════════════════════════════════════════════════════════

QString AudioRouterApplet::addVirtualSink(int groupIndex, const QString &displayName)
{
    auto *group = m_groupModel.groupAt(groupIndex);
    if (!group) return {};

    // Generate a short unique PA-safe name
    const QString paName = QStringLiteral("soundroot_virt_")
        + QUuid::createUuid().toString(QUuid::Id128).left(8);

    VirtualSinkDef def;
    def.displayName = displayName;
    def.paName = paName;
    group->virtualSinks.append(def);
    m_groupModel.save();

    // If this room is currently active, create the null-sink immediately
    if (group->active)
        m_audioBackend.createVirtualSink(paName, displayName);

    return paName;
}

void AudioRouterApplet::removeVirtualSink(int groupIndex, const QString &paName)
{
    auto *group = m_groupModel.groupAt(groupIndex);
    if (!group) return;

    // Destroy the PA module if the room is active
    if (group->active)
        m_audioBackend.destroyVirtualSink(paName);

    group->virtualSinks.erase(
        std::remove_if(group->virtualSinks.begin(), group->virtualSinks.end(),
                       [&paName](const VirtualSinkDef &v) { return v.paName == paName; }),
        group->virtualSinks.end());

    m_groupModel.save();
}

QVariantList AudioRouterApplet::groupVirtualSinks(int groupIndex) const
{
    QVariantList result;
    const auto *group = m_groupModel.groupAt(groupIndex);
    if (!group) return result;

    for (const auto &vs : group->virtualSinks) {
        QVariantMap m;
        m[QStringLiteral("displayName")] = vs.displayName;
        m[QStringLiteral("paName")] = vs.paName;
        m[QStringLiteral("loopbackTargets")] = QVariant::fromValue(vs.loopbackTargets);
        result.append(m);
    }
    return result;
}

void AudioRouterApplet::setVirtualSinkLoopbackTargets(int groupIndex,
                                                      const QString &paName,
                                                      const QStringList &targets)
{
    auto *group = m_groupModel.groupAt(groupIndex);
    if (!group) return;

    // QML calls this for every virtual sink on each connection change, so
    // only persist when the targets actually changed.
    for (auto &vs : group->virtualSinks) {
        if (vs.paName == paName) {
            if (vs.loopbackTargets != targets) {
                vs.loopbackTargets = targets;
                m_groupModel.save();
            }
            break;
        }
    }

    // If room is active, apply loopbacks now
    if (group->active)
        m_audioBackend.setVirtualSinkLoopbacks(paName, targets);
}

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

    // A virtual null-sink finished loading: streams routed into it could not
    // be moved while it did not exist yet, so re-apply the active room.
    connect(&m_audioBackend, &PulseAudioBackend::virtualSinkAppeared,
            this, [this]() {
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
