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

    // Debounced re-apply: when new audio streams appear, re-route them
    m_reapplyTimer.setSingleShot(true);
    m_reapplyTimer.setInterval(1000);
    connect(&m_reapplyTimer, &QTimer::timeout,
            this, &AudioRouterApplet::applyAllActiveGroups);

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

    for (const auto &route : group->routes)
        m_audioBackend.applyRoute(route.sourceAppName,
                                  route.outputSinkNames,
                                  route.volumePercent);

    group->active = true;
    m_groupModel.save();
}

void AudioRouterApplet::deactivateGroup(int groupIndex)
{
    auto *group = m_groupModel.groupAt(groupIndex);
    if (!group) return;

    for (const auto &route : group->routes)
        m_audioBackend.removeRoute(route.sourceAppName);

    group->active = false;
    m_groupModel.save();
}

void AudioRouterApplet::applyAllActiveGroups()
{
    for (int i = 0; i < m_groupModel.rowCount(); ++i) {
        auto *group = m_groupModel.groupAt(i);
        if (group && group->active)
            applyGroup(i);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Internals
// ═════════════════════════════════════════════════════════════════════════════

void AudioRouterApplet::setupConnections()
{
    connect(&m_groupModel, &GroupListModel::groupChanged,
            this, [this](int row) {
                const auto *group = m_groupModel.groupAt(row);
                if (!group) return;
                if (group->active)
                    applyGroup(row);
                else
                    deactivateGroup(row);
            });

    connect(&m_audioBackend, &PulseAudioBackend::serverConnected,
            this, [this]() {
                QMetaObject::invokeMethod(this, "applyAllActiveGroups",
                                          Qt::QueuedConnection);
            });

    connect(&m_audioBackend, &PulseAudioBackend::sinkInputsChanged,
            this, [this]() {
                m_reapplyTimer.start();
            });

    connect(&m_audioBackend, &PulseAudioBackend::errorOccurred,
            this, [](const QString &msg) {
                qWarning() << "SoundRoot plasmoid:" << msg;
            });
}

K_PLUGIN_CLASS_WITH_JSON(AudioRouterApplet, "../package/metadata.json")

#include "audiorouterapplet.moc"
