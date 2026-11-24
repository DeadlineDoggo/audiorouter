#include "audiorouterapplet.h"

#include <QAction>
#include <QKeySequence>
#include <KLocalizedString>
#include <KGlobalAccel>
#include <KActionCollection>

#include "net/netcrypto.h"

AudioRouterApplet::AudioRouterApplet(QObject *parent,
                                     const KPluginMetaData &data,
                                     const QVariantList &args)
    : Plasma::Applet(parent, data, args)
    , m_sinkModel(&m_audioBackend)
    , m_sinkInputModel(&m_audioBackend)
    , m_sourceModel(&m_audioBackend)
    , m_sourceOutputModel(&m_audioBackend)
    , m_engine(&m_audioBackend, &m_groupModel)
{
    m_engine.setNetworkController(&m_netEngine);
    m_audioBackend.setAudioTap(&m_netEngine);

    connect(&m_audioBackend, &PulseAudioBackend::defaultSinkVolumeChanged,
            this, &AudioRouterApplet::masterVolumeChanged);
    connect(&m_audioBackend, &AudioBackend::serverConnected,
            this, &AudioRouterApplet::serverConnectedChanged);
    connect(&m_audioBackend, &AudioBackend::serverDisconnected,
            this, &AudioRouterApplet::serverConnectedChanged);
    connect(&m_audioBackend, &AudioBackend::errorOccurred,
            this, [](const QString &msg) { qWarning() << "SoundRoot:" << msg; });

    connect(&m_engine, &RoutingEngine::connectionsChanged,
            this, &AudioRouterApplet::connectionsChanged);
    connect(&m_engine, &RoutingEngine::virtualSinksChanged,
            this, &AudioRouterApplet::virtualSinksChanged);
    connect(&m_netEngine, &NetEngine::statusChanged,
            this, &AudioRouterApplet::networkStatusChanged);

    // The engine applies the active room as soon as the server reports its
    // devices and streams, and keeps it applied from then on.
    m_groupModel.load();
    m_audioBackend.initialize();
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

    // The shortcuts act here, in C++: they work right after login, before
    // the popup (and with it any QML handler) has ever been created.
    m_nextAction = coll->addAction(QStringLiteral("nextRoom"));
    m_nextAction->setObjectName(QStringLiteral("nextRoom"));
    m_nextAction->setText(i18n("Activate Next Audio Room"));
    KGlobalAccel::self()->setDefaultShortcut(m_nextAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Right")) });
    KGlobalAccel::self()->setShortcut(m_nextAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Right")) });
    connect(m_nextAction, &QAction::triggered, this, &AudioRouterApplet::activateNextRoom);

    m_prevAction = coll->addAction(QStringLiteral("prevRoom"));
    m_prevAction->setObjectName(QStringLiteral("prevRoom"));
    m_prevAction->setText(i18n("Activate Previous Audio Room"));
    KGlobalAccel::self()->setDefaultShortcut(m_prevAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Left")) });
    KGlobalAccel::self()->setShortcut(m_prevAction, { QKeySequence(QStringLiteral("Ctrl+Alt+Left")) });
    connect(m_prevAction, &QAction::triggered, this, &AudioRouterApplet::activatePrevRoom);
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
// Rooms
// ═════════════════════════════════════════════════════════════════════════════

void AudioRouterApplet::activateRoom(int row)   { m_engine.activateRoom(row); }
void AudioRouterApplet::deactivateRoom(int row) { m_engine.deactivateRoom(row); }
void AudioRouterApplet::removeRoom(int row)     { m_engine.removeRoom(row); }
void AudioRouterApplet::activateNextRoom()      { m_engine.activateRelative(+1); }
void AudioRouterApplet::activatePrevRoom()      { m_engine.activateRelative(-1); }
int  AudioRouterApplet::activeRoom() const      { return m_groupModel.activeRow(); }

// ═════════════════════════════════════════════════════════════════════════════
// Wires
// ═════════════════════════════════════════════════════════════════════════════

bool AudioRouterApplet::canConnect(int row, const QString &a, const QString &b) const
{
    return m_engine.canConnect(row, a, b);
}

bool AudioRouterApplet::connectNodes(int row, const QString &a, const QString &b)
{
    return m_engine.connectNodes(row, a, b);
}

void AudioRouterApplet::disconnectNodes(int row, const QString &a, const QString &b)
{
    m_engine.disconnectNodes(row, a, b);
}

void AudioRouterApplet::disconnectNode(int row, const QString &key)
{
    m_engine.disconnectNode(row, key);
}

void AudioRouterApplet::clearRoom(int row)                  { m_engine.clearRoom(row); }
QVariantList AudioRouterApplet::roomConnections(int row)    { return m_engine.connections(row); }
QVariantList AudioRouterApplet::offlineSources(int row)     { return m_engine.offlineSources(row); }
QVariantList AudioRouterApplet::offlineSinks(int row)       { return m_engine.offlineSinks(row); }
QVariantList AudioRouterApplet::offlineMics(int row)        { return m_engine.offlineMics(row); }
QVariantList AudioRouterApplet::offlineRecorders(int row)   { return m_engine.offlineRecorders(row); }

// ═════════════════════════════════════════════════════════════════════════════
// Volumes
// ═════════════════════════════════════════════════════════════════════════════

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

void AudioRouterApplet::setSinkInputVolume(int inputIndex, int percent)
{
    m_audioBackend.setSinkInputVolume(static_cast<uint32_t>(inputIndex), percent);
}

// ═════════════════════════════════════════════════════════════════════════════
// Virtual and network nodes
// ═════════════════════════════════════════════════════════════════════════════

QString AudioRouterApplet::addVirtualSink(int row, const QString &displayName)
{
    return m_engine.addVirtualSink(row, displayName);
}

void AudioRouterApplet::removeVirtualSink(int row, const QString &paName)
{
    m_engine.removeVirtualSink(row, paName);
}

QVariantList AudioRouterApplet::groupVirtualSinks(int row) const
{
    return m_engine.virtualSinks(row);
}

void AudioRouterApplet::configureVirtualSink(int row, const QString &paName,
                                             const QVariantMap &config)
{
    m_engine.configureVirtualSink(row, paName, config);
}

QVariantMap AudioRouterApplet::networkStatus(const QString &paName) const
{
    return m_netEngine.nodeStatus(paName);
}

QString AudioRouterApplet::generatePassphrase() const
{
    return NetCrypto::generatePassphrase();
}

K_PLUGIN_CLASS_WITH_JSON(AudioRouterApplet, "../package/metadata.json")

#include "audiorouterapplet.moc"
