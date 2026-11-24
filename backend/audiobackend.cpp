#include "audiobackend.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>

// Data passed through the PulseAudio module-load callback
struct ModuleLoadData {
    PulseAudioBackend *backend;
    QString combinedName;
    QStringList slaves;   // slaves this particular load was started with
};

// What a virtual-node module load is for
enum VirtualModuleKind {
    NullSinkModule,   // key: paName
    LoopbackModule,   // key: "paName|sinkName"  (OUT side)
    MicModule,        // key: paName             (the node's microphone)
    InputModule,      // key: "paName|sourceName" (a microphone into IN)
};

// Data passed through virtual-sink / loopback module-load callbacks
struct VirtualModuleLoadData {
    PulseAudioBackend *backend;
    QString key;
    int kind;
};

namespace {
/// Key under which a load in flight is remembered; one set for all kinds,
/// so the kinds whose keys could look alike are told apart.
QString pendingKey(int kind, const QString &key)
{
    switch (kind) {
    case MicModule:   return QStringLiteral("mic:") + key;
    case InputModule: return QStringLiteral("in:") + key;
    default:          return key;
    }
}

/// Text for a module argument that is wrapped as  x='y="…"'
QString quotable(QString text)
{
    text.remove(QLatin1Char('"')).remove(QLatin1Char('\'')).remove(QLatin1Char('\\'));
    return text;
}
} // namespace

// Data passed through sink / sink-input list callbacks: the generation the
// enumeration was started with, so results of a superseded one are dropped.
struct EnumRequest {
    PulseAudioBackend *backend;
    uint64_t gen;
};

// A network node's record or playback stream
struct PulseAudioBackend::NetStream {
    PulseAudioBackend *backend = nullptr;
    QString    paName;
    bool       capture = false;
    pa_stream *stream = nullptr;
    QByteArray device;   // source (capture) or initial sink (playback)
};

namespace {
// Network audio format: signed 16-bit little-endian, 48 kHz, stereo
constexpr uint32_t kNetRate = 48000;
constexpr uint8_t  kNetChannels = 2;
constexpr size_t   kNetFrameBytes = kNetChannels * 2;
constexpr uint32_t kNetBytesPerMs = kNetRate * kNetFrameBytes / 1000;

pa_volume_t volumeFromPercent(int percent)
{
    // Raw PA linear volume: 100% == PA_VOLUME_NORM.  KDE and pavucontrol use
    // the same convention.
    return static_cast<pa_volume_t>(qBound(0, percent, 150) / 100.0 * PA_VOLUME_NORM);
}
} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Construction / destruction
// ═══════════════════════════════════════════════════════════════════════════

PulseAudioBackend::PulseAudioBackend(QObject *parent)
    : AudioBackend(parent)
{
}

PulseAudioBackend::~PulseAudioBackend()
{
    shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::initialize()
{
    if (m_mainloop || m_shuttingDown) return;

    QString failure;
    m_mainloop = pa_threaded_mainloop_new();
    if (!m_mainloop) {
        failure = QStringLiteral("Failed to create PulseAudio mainloop");
    } else {
        pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_mainloop);
        m_context = pa_context_new(api, "SoundRoot");
        if (!m_context)
            failure = QStringLiteral("Failed to create PulseAudio context");
    }

    if (failure.isEmpty()) {
        pa_context_set_state_callback(m_context, contextStateCallback, this);
        // NOFAIL asks libpulse to wait for a server that is not there yet.
        // It can only do that with a session bus to watch, so a refusal here
        // is not final either: we simply try again (see scheduleReconnect).
        if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0)
            failure = QStringLiteral("The sound server is not reachable");
    }

    if (!failure.isEmpty()) {
        if (m_context) {
            pa_context_set_state_callback(m_context, nullptr, nullptr);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        if (m_mainloop) {
            pa_threaded_mainloop_free(m_mainloop);
            m_mainloop = nullptr;
        }
        if (!m_reportedUnreachable) {   // once, not on every retry
            m_reportedUnreachable = true;
            emit errorOccurred(failure + QStringLiteral(" - retrying"));
        }
        scheduleReconnect();
        return;
    }

    pa_threaded_mainloop_start(m_mainloop);
}

void PulseAudioBackend::scheduleReconnect()
{
    if (m_shuttingDown) return;
    QTimer::singleShot(m_reconnectDelayMs.load(), this, &PulseAudioBackend::initialize);
    // Back off a little while the server stays away
    m_reconnectDelayMs = qMin(m_reconnectDelayMs.load() * 2, kMaxReconnectDelayMs);
}

void PulseAudioBackend::teardown()
{
    if (m_mainloop)
        pa_threaded_mainloop_stop(m_mainloop);   // no callback runs after this

    auto freeStreams = [](QHash<QString, NetStream *> &streams) {
        for (NetStream *ns : std::as_const(streams)) {
            pa_stream_set_state_callback(ns->stream, nullptr, nullptr);
            pa_stream_set_read_callback(ns->stream, nullptr, nullptr);
            pa_stream_set_write_callback(ns->stream, nullptr, nullptr);
            pa_stream_disconnect(ns->stream);
            pa_stream_unref(ns->stream);
            delete ns;
        }
        streams.clear();
    };
    freeStreams(m_captureStreams);
    freeStreams(m_playbackStreams);

    if (m_context) {
        pa_context_set_state_callback(m_context, nullptr, nullptr);
        pa_context_set_subscribe_callback(m_context, nullptr, nullptr);
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    if (m_mainloop) {
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }

    // Callbacks that will never run now still own their data
    for (const auto &deleter : std::as_const(m_userdata))
        deleter();
    m_userdata.clear();

    // Nothing we loaded exists any more
    m_connected = false;
    m_pendingUnloads = 0;
    m_virtualSinkModules.clear();
    m_loopbackModules.clear();
    m_wantedVirtualSinks.clear();
    m_wantedLoopbacks.clear();
    m_wantedMics.clear();
    m_micModules.clear();
    m_wantedInputs.clear();
    m_inputModules.clear();
    m_pendingVirtualLoads.clear();
    m_streamFailures.clear();
    m_pendingSinks.clear();
    m_pendingSinkInputs.clear();
    m_pendingSources.clear();
    m_pendingSourceOutputs.clear();

    QMutexLocker lock(&m_mutex);
    m_combinedModules.clear();
    m_combinedSlaves.clear();
    m_pendingCombined.clear();
    m_sinks.clear();
    m_sinkInputs.clear();
    m_sources.clear();
    m_sourceOutputs.clear();
    m_defaultSinkName.clear();
    m_defaultSourceName.clear();
}

void PulseAudioBackend::shutdown()
{
    m_shuttingDown = true;

    // Unload our modules while the mainloop is still running so the
    // operations actually execute.  Under native PulseAudio (unlike PipeWire)
    // modules are NOT auto-removed when a client disconnects.
    if (m_mainloop && m_context && m_connected) {
        pa_threaded_mainloop_lock(m_mainloop);
        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_combinedModules.cbegin(); it != m_combinedModules.cend(); ++it)
                unloadModuleLocked(it.value());
            m_combinedModules.clear();
            m_combinedSlaves.clear();   // loads still in flight are unloaded on completion
        }
        m_wantedLoopbacks.clear();
        m_wantedVirtualSinks.clear();
        m_wantedMics.clear();
        m_wantedInputs.clear();
        for (auto it = m_loopbackModules.cbegin(); it != m_loopbackModules.cend(); ++it)
            unloadModuleLocked(it.value());
        m_loopbackModules.clear();
        for (auto it = m_inputModules.cbegin(); it != m_inputModules.cend(); ++it)
            unloadModuleLocked(it.value());
        m_inputModules.clear();
        // A recording application on a node's microphone falls back to the
        // default source when the microphone goes.
        for (auto it = m_micModules.cbegin(); it != m_micModules.cend(); ++it)
            unloadModuleLocked(it.value());
        m_micModules.clear();
        for (auto it = m_virtualSinkModules.cbegin(); it != m_virtualSinkModules.cend(); ++it)
            unloadModuleLocked(it.value());
        m_virtualSinkModules.clear();

        // Queued operations only reach the server if we keep the connection
        // open until they are done: wait for the unloads, and for loads still
        // in flight (their callbacks unload them).  Bounded so a stuck server
        // cannot hang the shell on exit.
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < kShutdownWaitMs && m_connected) {
            bool busy = m_pendingUnloads > 0 || !m_pendingVirtualLoads.isEmpty();
            if (!busy) {
                QMutexLocker lock(&m_mutex);
                busy = !m_pendingCombined.isEmpty();
            }
            if (!busy) break;
            pa_threaded_mainloop_unlock(m_mainloop);
            QThread::msleep(5);
            pa_threaded_mainloop_lock(m_mainloop);
        }
        if (m_pendingUnloads > 0)
            qWarning() << "SoundRoot: shutdown timed out with" << m_pendingUnloads
                       << "module unload(s) unconfirmed";
        pa_threaded_mainloop_unlock(m_mainloop);
    }

    teardown();
}

void PulseAudioBackend::handleServerLost(quintptr deadContext)
{
    // Runs on the main thread.  Ignore a notification about a context that
    // was replaced already.
    if (m_shuttingDown || !m_context || reinterpret_cast<quintptr>(m_context) != deadContext)
        return;

    if (!m_reportedUnreachable) {
        m_reportedUnreachable = true;
        qWarning() << "SoundRoot: lost the connection to the sound server - reconnecting";
    }
    const bool wasConnected = m_everConnected;
    m_everConnected = false;
    teardown();
    if (!wasConnected) {
        // It never got as far as connecting: nothing to tell anyone, retry
        scheduleReconnect();
        return;
    }
    emit serverDisconnected();
    emit sinksChanged();
    emit sinkInputsChanged();
    emit sourcesChanged();
    emit sourceOutputsChanged();

    // A context cannot be reused after it failed: start over with a new one,
    // again and again until the server is back.
    scheduleReconnect();
}

void PulseAudioBackend::trackUserdata(void *data, std::function<void()> deleter)
{
    m_userdata.insert(data, std::move(deleter));
}

void PulseAudioBackend::untrackUserdata(void *data)
{
    m_userdata.remove(data);
}

// ═══════════════════════════════════════════════════════════════════════════
// Thread-safe queries
// ═══════════════════════════════════════════════════════════════════════════

QVector<PASinkInfo> PulseAudioBackend::availableSinks() const
{
    QMutexLocker lock(&m_mutex);
    return m_sinks;
}

QVector<PASinkInputInfo> PulseAudioBackend::availableSinkInputs() const
{
    QMutexLocker lock(&m_mutex);
    return m_sinkInputs;
}

QString PulseAudioBackend::defaultSinkName() const
{
    QMutexLocker lock(&m_mutex);
    return m_defaultSinkName;
}

QVector<PASourceInfo> PulseAudioBackend::availableSources() const
{
    QMutexLocker lock(&m_mutex);
    return m_sources;
}

QVector<PASourceOutputInfo> PulseAudioBackend::availableSourceOutputs() const
{
    QMutexLocker lock(&m_mutex);
    return m_sourceOutputs;
}

QString PulseAudioBackend::defaultSourceName() const
{
    QMutexLocker lock(&m_mutex);
    return m_defaultSourceName;
}

int PulseAudioBackend::defaultSinkVolume() const
{
    QMutexLocker lock(&m_mutex);
    return m_defaultSinkVolume;
}

// ═══════════════════════════════════════════════════════════════════════════
// Stream moves and volumes
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::moveSinkInput(uint32_t sinkInputIndex, uint32_t sinkIndex)
{
    if (!m_context || !m_connected) return;

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_move_sink_input_by_index(
        m_context, sinkInputIndex, sinkIndex, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::moveSourceOutput(uint32_t sourceOutputIndex, uint32_t sourceIndex)
{
    if (!m_context || !m_connected) return;

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_move_source_output_by_index(
        m_context, sourceOutputIndex, sourceIndex, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::setSinkInputVolume(uint32_t sinkInputIndex, int volumePercent)
{
    if (!m_context || !m_connected) return;

    uint8_t channels = 2;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &si : std::as_const(m_sinkInputs)) {
            if (si.index == sinkInputIndex) { channels = si.channels; break; }
        }
    }

    pa_cvolume volume;
    pa_cvolume_set(&volume, qMax<uint8_t>(1, channels), volumeFromPercent(volumePercent));

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_set_sink_input_volume(
        m_context, sinkInputIndex, &volume, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::setSinkVolume(const QString &sinkName, int percent)
{
    if (!m_context || !m_connected || sinkName.isEmpty()) return;

    // Set every channel of the sink, not just the first two
    uint8_t channels = 2;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &s : std::as_const(m_sinks)) {
            if (s.name == sinkName) { channels = s.channels; break; }
        }
    }

    pa_cvolume volume;
    pa_cvolume_set(&volume, qMax<uint8_t>(1, channels), volumeFromPercent(percent));

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_set_sink_volume_by_name(
        m_context, sinkName.toUtf8().constData(),
        &volume, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::setDefaultSinkVolume(int percent)
{
    setSinkVolume(defaultSinkName(), percent);
}

void PulseAudioBackend::setSinkVolumeByName(const QString &sinkName, int percent)
{
    setSinkVolume(sinkName, percent);
}

void PulseAudioBackend::toggleSinkMuteByName(const QString &sinkName)
{
    if (!m_context || !m_connected || sinkName.isEmpty()) return;

    // Read current mute state, then flip it
    bool currentlyMuted = false;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &s : std::as_const(m_sinks)) {
            if (s.name == sinkName) { currentlyMuted = s.isMuted; break; }
        }
    }

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_set_sink_mute_by_name(
        m_context, sinkName.toUtf8().constData(),
        currentlyMuted ? 0 : 1, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

// ═══════════════════════════════════════════════════════════════════════════
// Combine sinks
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::createCombinedSink(const QString &name,
                                           const QStringList &sinkNames)
{
    if (!m_context || !m_connected || sinkNames.isEmpty()) return;

    // Lock order: PA mainloop lock first, then m_mutex (same as the callbacks).
    pa_threaded_mainloop_lock(m_mainloop);

    bool startLoad = false;
    uint32_t staleModule = PA_INVALID_INDEX;
    {
        QMutexLocker lock(&m_mutex);
        const QStringList previous = m_combinedSlaves.value(name);
        m_combinedSlaves[name] = sinkNames;   // desired state

        auto loaded = m_combinedModules.find(name);
        if (loaded != m_combinedModules.end()) {
            if (previous == sinkNames) {
                // Already loaded with exactly these slaves
                pa_threaded_mainloop_unlock(m_mainloop);
                return;
            }
            // Loaded with different slaves: replace it
            staleModule = loaded.value();
            m_combinedModules.erase(loaded);
        }

        // A load for this name may still be in flight.  Never start a second
        // one (the name would clash); moduleLoadCallback reconciles the result
        // with the desired slaves once it completes.
        if (!m_pendingCombined.contains(name)) {
            m_pendingCombined.insert(name, sinkNames);
            startLoad = true;
        }
    }

    if (staleModule != PA_INVALID_INDEX)
        unloadModuleLocked(staleModule);
    if (startLoad)
        loadCombinedLocked(name, sinkNames);

    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::loadCombinedLocked(const QString &name,
                                           const QStringList &sinkNames)
{
    // module-combine-sink arguments
    const QString args = QStringLiteral("sink_name=%1 slaves=%2")
                             .arg(name, sinkNames.join(QLatin1Char(',')));

    auto *data = new ModuleLoadData{this, name, sinkNames};
    pa_operation *op = pa_context_load_module(
        m_context, "module-combine-sink",
        args.toUtf8().constData(),
        moduleLoadCallback, data);
    if (op) {
        trackUserdata(data, [data]() { delete data; });
        pa_operation_unref(op);
    } else {
        // Nothing in flight after all: let the next create retry
        delete data;
        QMutexLocker lock(&m_mutex);
        m_pendingCombined.remove(name);
        m_combinedSlaves.remove(name);
    }
}

void PulseAudioBackend::destroyCombinedSink(const QString &name)
{
    if (!m_context || !m_connected) return;

    pa_threaded_mainloop_lock(m_mainloop);
    uint32_t moduleIdx = PA_INVALID_INDEX;
    {
        QMutexLocker lock(&m_mutex);
        // No longer wanted.  A load still in flight is unloaded by
        // moduleLoadCallback when it completes.
        m_combinedSlaves.remove(name);
        auto it = m_combinedModules.find(name);
        if (it != m_combinedModules.end()) {
            moduleIdx = it.value();
            m_combinedModules.erase(it);
        }
    }
    if (moduleIdx != PA_INVALID_INDEX)
        unloadModuleLocked(moduleIdx);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::moduleLoadCallback(pa_context * /*c*/,
                                           uint32_t idx, void *userdata)
{
    // Runs on the PA thread with the mainloop lock held.
    auto *data = static_cast<ModuleLoadData *>(userdata);
    auto *self = data->backend;
    const QString name = data->combinedName;
    const QStringList slaves = data->slaves;
    self->untrackUserdata(data);
    delete data;

    const bool failed = (idx == PA_INVALID_INDEX);
    bool unloadIt = false;
    bool reload = false;
    QStringList reloadSlaves;
    {
        QMutexLocker lock(&self->m_mutex);
        self->m_pendingCombined.remove(name);

        const bool wanted = self->m_combinedSlaves.contains(name);
        const bool matches = wanted && self->m_combinedSlaves.value(name) == slaves;

        if (failed) {
            // Forget the desired state so a later create call can retry
            if (matches) self->m_combinedSlaves.remove(name);
        } else if (matches) {
            self->m_combinedModules[name] = idx;
        } else {
            // Destroyed, or wanted with other slaves, while the load was in flight
            unloadIt = true;
        }

        // Wanted with different slaves than the load that just finished?
        if (wanted && !matches) {
            reload = true;
            reloadSlaves = self->m_combinedSlaves.value(name);
            self->m_pendingCombined.insert(name, reloadSlaves);
        }
    }

    if (failed) {
        qWarning() << "SoundRoot: failed to load combine-sink" << name;
        QMetaObject::invokeMethod(
            self, "errorOccurred", Qt::QueuedConnection,
            Q_ARG(QString, QStringLiteral("Failed to load combined sink module")));
    }
    if (unloadIt)
        self->unloadModuleLocked(idx);
    if (reload)
        self->loadCombinedLocked(name, reloadSlaves);
}

// ═══════════════════════════════════════════════════════════════════════════
// Virtual null-sink / loopback management
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::createVirtualSink(const QString &paName,
                                          const QString &displayName)
{
    if (!m_context || !m_connected) return;

    // The description is wrapped as sink_properties='device.description="…"',
    // so strip the quote characters that would break that nesting.
    const QString safe = quotable(displayName.isEmpty() ? paName : displayName);
    const QString args =
        QStringLiteral("sink_name=%1 sink_properties='device.description=\"%2\"'")
            .arg(paName, safe);

    pa_threaded_mainloop_lock(m_mainloop);
    m_wantedVirtualSinks.insert(paName);
    m_wantedMics.insert(paName, QStringLiteral("SoundRoot: ") + safe);
    if (m_virtualSinkModules.contains(paName)) {
        loadVirtualMicLocked(paName);   // e.g. it failed to load before
    } else if (!m_pendingVirtualLoads.contains(paName)) {
        loadVirtualModuleLocked("module-null-sink", args, paName, NullSinkModule);
    }
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::destroyVirtualSink(const QString &paName)
{
    if (!m_context || !m_connected) return;

    pa_threaded_mainloop_lock(m_mainloop);

    // Forget the desired state first, so loads still in flight get unloaded
    // by virtualModuleLoadCallback when they complete.
    const QString prefix = paName + QLatin1Char('|');
    m_wantedVirtualSinks.remove(paName);
    m_wantedMics.remove(paName);
    for (auto *wanted : { &m_wantedLoopbacks, &m_wantedInputs }) {
        for (auto it = wanted->begin(); it != wanted->end();) {
            if (it->startsWith(prefix)) it = wanted->erase(it);
            else ++it;
        }
    }

    // Destroy everything attached to the null-sink first
    for (auto *loaded : { &m_loopbackModules, &m_inputModules }) {
        for (auto it = loaded->begin(); it != loaded->end();) {
            if (it.key().startsWith(prefix)) {
                unloadModuleLocked(it.value());
                it = loaded->erase(it);
            } else {
                ++it;
            }
        }
    }
    auto mic = m_micModules.find(paName);
    if (mic != m_micModules.end()) {
        unloadModuleLocked(mic.value());
        m_micModules.erase(mic);
    }

    // Destroy the null-sink module
    auto it = m_virtualSinkModules.find(paName);
    if (it != m_virtualSinkModules.end()) {
        unloadModuleLocked(it.value());
        m_virtualSinkModules.erase(it);
    }

    pa_threaded_mainloop_unlock(m_mainloop);
}

namespace {
/// Bring the wanted keys of one node (all starting with prefix) in line with
/// @p desired; unloads what is no longer wanted.  Returns nothing to load:
/// the caller loads what is missing.
template <typename Unload>
void syncWanted(QSet<QString> &wanted, QMap<QString, uint32_t> &loaded,
                const QString &prefix, const QSet<QString> &desired, Unload unload)
{
    for (auto it = wanted.begin(); it != wanted.end();) {
        if (it->startsWith(prefix) && !desired.contains(*it)) {
            auto module = loaded.find(*it);
            if (module != loaded.end()) {
                unload(module.value());
                loaded.erase(module);
            }
            it = wanted.erase(it);
        } else {
            ++it;
        }
    }
    wanted.unite(desired);
}
} // namespace

void PulseAudioBackend::setVirtualSinkLoopbacks(const QString &paName,
                                                const QStringList &targetSinks)
{
    if (!m_context || !m_connected) return;

    pa_threaded_mainloop_lock(m_mainloop);

    if (!m_wantedVirtualSinks.contains(paName)) {
        pa_threaded_mainloop_unlock(m_mainloop);
        return;
    }

    const QString prefix = paName + QLatin1Char('|');
    QSet<QString> desired;
    for (const auto &sinkName : targetSinks) {
        if (!sinkName.isEmpty())
            desired.insert(prefix + sinkName);
    }
    syncWanted(m_wantedLoopbacks, m_loopbackModules, prefix, desired,
               [this](uint32_t idx) { unloadModuleLocked(idx); });

    // If the null-sink is still loading, its load callback creates these.
    if (m_virtualSinkModules.contains(paName))
        loadMissingLoopbacksLocked(paName);

    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::setVirtualSinkInputs(const QString &paName,
                                             const QStringList &sourceNames)
{
    if (!m_context || !m_connected) return;

    pa_threaded_mainloop_lock(m_mainloop);

    if (!m_wantedVirtualSinks.contains(paName)) {
        pa_threaded_mainloop_unlock(m_mainloop);
        return;
    }

    const QString prefix = paName + QLatin1Char('|');
    QSet<QString> desired;
    for (const auto &source : sourceNames) {
        if (!source.isEmpty())
            desired.insert(prefix + source);
    }
    syncWanted(m_wantedInputs, m_inputModules, prefix, desired,
               [this](uint32_t idx) { unloadModuleLocked(idx); });

    if (m_virtualSinkModules.contains(paName))
        loadMissingInputsLocked(paName);

    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::unloadModuleLocked(uint32_t moduleIndex)
{
    ++m_pendingUnloads;
    pa_operation *op = pa_context_unload_module(
        m_context, moduleIndex, unloadModuleCallback, this);
    if (op) pa_operation_unref(op);
    else    --m_pendingUnloads;
}

void PulseAudioBackend::unloadModuleCallback(pa_context * /*c*/, int success,
                                             void *userdata)
{
    // Runs on the PA thread with the mainloop lock held.
    auto *self = static_cast<PulseAudioBackend *>(userdata);
    if (self->m_pendingUnloads > 0)
        --self->m_pendingUnloads;
    if (!success)
        qWarning() << "SoundRoot: failed to unload module";
}

bool PulseAudioBackend::loadVirtualModuleLocked(const char *module, const QString &args,
                                                const QString &key, int kind)
{
    auto *data = new VirtualModuleLoadData{this, key, kind};
    pa_operation *op = pa_context_load_module(
        m_context, module, args.toUtf8().constData(), virtualModuleLoadCallback, data);
    if (!op) {
        delete data;
        return false;
    }
    trackUserdata(data, [data]() { delete data; });
    m_pendingVirtualLoads.insert(pendingKey(kind, key));
    pa_operation_unref(op);
    return true;
}

void PulseAudioBackend::loadMissingLoopbacksLocked(const QString &paName)
{
    const QString prefix = paName + QLatin1Char('|');
    const QString monitorSource = paName + QStringLiteral(".monitor");

    for (const auto &key : std::as_const(m_wantedLoopbacks)) {
        if (!key.startsWith(prefix) || m_loopbackModules.contains(key) ||
            m_pendingVirtualLoads.contains(pendingKey(LoopbackModule, key)))
            continue;

        const QString sinkName = key.mid(prefix.size());
        // Tag the sink-input so we can filter it from the app list.
        // *_dont_move: if the target goes away the loopback must fail rather
        // than quietly follow the default device (or, on the source side,
        // start recording the microphone).
        const QString args = QStringLiteral(
            "source=%1 sink=%2 latency_msec=50 source_dont_move=true sink_dont_move=true "
            "sink_input_properties=media.name=soundroot_loopback_%3")
                .arg(monitorSource, sinkName, paName);
        loadVirtualModuleLocked("module-loopback", args, key, LoopbackModule);
    }
}

void PulseAudioBackend::loadVirtualMicLocked(const QString &paName)
{
    const auto wanted = m_wantedMics.constFind(paName);
    if (wanted == m_wantedMics.constEnd() || m_micModules.contains(paName)
        || m_pendingVirtualLoads.contains(pendingKey(MicModule, paName)))
        return;

    // A remap of the monitor looks like an ordinary microphone.  Applications
    // built on Chromium (Teams, Discord, ...) do not offer monitors at all.
    const QString args = QStringLiteral(
        "master=%1.monitor source_name=%2 source_properties='device.description=\"%3\"'")
            .arg(paName, virtualMicName(paName), wanted.value());
    loadVirtualModuleLocked("module-remap-source", args, paName, MicModule);
}

void PulseAudioBackend::loadMissingInputsLocked(const QString &paName)
{
    const QString prefix = paName + QLatin1Char('|');

    for (const auto &key : std::as_const(m_wantedInputs)) {
        if (!key.startsWith(prefix) || m_inputModules.contains(key) ||
            m_pendingVirtualLoads.contains(pendingKey(InputModule, key)))
            continue;

        const QString sourceName = key.mid(prefix.size());
        // A voice in a call must not lag: a short latency.  *_dont_move as
        // above: an unplugged microphone ends the loopback instead of
        // recording another one.
        const QString args = QStringLiteral(
            "source=%1 sink=%2 latency_msec=20 source_dont_move=true sink_dont_move=true "
            "sink_input_properties=media.name=soundroot_loopback_in_%2 "
            "source_output_properties=media.name=soundroot_loopback_in_%2")
                .arg(sourceName, paName);
        loadVirtualModuleLocked("module-loopback", args, key, InputModule);
    }
}

void PulseAudioBackend::virtualModuleLoadCallback(pa_context * /*c*/,
                                                   uint32_t idx, void *userdata)
{
    // Runs on the PA thread with the mainloop lock held.
    auto *data = static_cast<VirtualModuleLoadData *>(userdata);
    auto *self = data->backend;
    const QString key = data->key;
    const int kind = data->kind;
    self->untrackUserdata(data);
    delete data;

    self->m_pendingVirtualLoads.remove(pendingKey(kind, key));

    bool wanted = false;
    switch (kind) {
    case NullSinkModule: wanted = self->m_wantedVirtualSinks.contains(key); break;
    case LoopbackModule: wanted = self->m_wantedLoopbacks.contains(key); break;
    case MicModule:      wanted = self->m_wantedMics.contains(key); break;
    case InputModule:    wanted = self->m_wantedInputs.contains(key); break;
    }

    if (idx == PA_INVALID_INDEX) {
        static const char *const names[] = { "virtual sink", "loopback", "microphone", "input loopback" };
        qWarning() << "SoundRoot: failed to load" << names[kind] << "module for" << key;
        // Forget it so a later create/set call can retry
        switch (kind) {
        case NullSinkModule: self->m_wantedVirtualSinks.remove(key); break;
        case LoopbackModule: self->m_wantedLoopbacks.remove(key); break;
        case MicModule:      break;   // retried by the next createVirtualSink
        case InputModule:    self->m_wantedInputs.remove(key); break;
        }
        return;
    }
    if (!wanted) {
        // Destroyed or replaced while the load was in flight
        self->unloadModuleLocked(idx);
        return;
    }
    switch (kind) {
    case NullSinkModule:
        self->m_virtualSinkModules.insert(key, idx);
        self->loadVirtualMicLocked(key);
        self->loadMissingLoopbacksLocked(key);
        self->loadMissingInputsLocked(key);
        break;
    case LoopbackModule: self->m_loopbackModules.insert(key, idx); break;
    case MicModule:      self->m_micModules.insert(key, idx); break;
    case InputModule:    self->m_inputModules.insert(key, idx); break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Network node streams
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::openNetworkCapture(const QString &paName)
{
    if (!m_context || !m_connected || !m_tap) return;

    pa_threaded_mainloop_lock(m_mainloop);
    const QString failKey = QStringLiteral("c|") + paName;
    const bool recentlyFailed = m_streamFailures.contains(failKey)
        && QDateTime::currentMSecsSinceEpoch() - m_streamFailures.value(failKey) < kStreamRetryMs;
    if (m_captureStreams.contains(paName) || recentlyFailed) {
        pa_threaded_mainloop_unlock(m_mainloop);
        return;
    }

    const pa_sample_spec spec { PA_SAMPLE_S16LE, kNetRate, kNetChannels };
    pa_proplist *props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "SoundRoot");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ICON_NAME, "network-wireless");

    auto *ns = new NetStream;
    ns->backend = this;
    ns->paName = paName;
    ns->capture = true;
    ns->device = (paName + QStringLiteral(".monitor")).toUtf8();
    ns->stream = pa_stream_new_with_proplist(
        m_context, (netCapturePrefix() + paName).toUtf8().constData(), &spec, nullptr, props);
    pa_proplist_free(props);

    bool ok = ns->stream != nullptr;
    if (ok) {
        pa_stream_set_state_callback(ns->stream, streamStateCallback, ns);
        pa_stream_set_read_callback(ns->stream, streamReadCallback, ns);

        pa_buffer_attr attr;
        attr.maxlength = static_cast<uint32_t>(-1);
        attr.tlength = static_cast<uint32_t>(-1);
        attr.prebuf = static_cast<uint32_t>(-1);
        attr.minreq = static_cast<uint32_t>(-1);
        attr.fragsize = 10 * kNetBytesPerMs;   // deliver audio every 10 ms

        // DONT_MOVE is essential: if the node's null-sink disappears this
        // stream must end.  Without it the server would re-attach it to the
        // default source - the microphone - and that would go to the network.
        const auto flags = static_cast<pa_stream_flags_t>(
            PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
        ok = pa_stream_connect_record(ns->stream, ns->device.constData(), &attr, flags) == 0;
    }
    if (ok) {
        m_captureStreams.insert(paName, ns);
    } else {
        qWarning() << "SoundRoot: cannot record network node" << paName;
        if (ns->stream) pa_stream_unref(ns->stream);
        delete ns;
        m_streamFailures.insert(failKey, QDateTime::currentMSecsSinceEpoch());
    }
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::openNetworkPlayback(const QString &paName, const QString &sinkName)
{
    if (!m_context || !m_connected || !m_tap || sinkName.isEmpty()) return;

    pa_threaded_mainloop_lock(m_mainloop);
    const QString failKey = QStringLiteral("p|") + paName;
    const bool recentlyFailed = m_streamFailures.contains(failKey)
        && QDateTime::currentMSecsSinceEpoch() - m_streamFailures.value(failKey) < kStreamRetryMs;
    if (m_playbackStreams.contains(paName) || recentlyFailed) {
        pa_threaded_mainloop_unlock(m_mainloop);
        return;
    }

    const pa_sample_spec spec { PA_SAMPLE_S16LE, kNetRate, kNetChannels };
    pa_proplist *props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "SoundRoot");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ICON_NAME, "network-wireless");

    auto *ns = new NetStream;
    ns->backend = this;
    ns->paName = paName;
    ns->capture = false;
    ns->device = sinkName.toUtf8();
    // The media name is how sinkInputInfoCallback recognises this stream
    ns->stream = pa_stream_new_with_proplist(
        m_context, (netPlaybackPrefix() + paName).toUtf8().constData(), &spec, nullptr, props);
    pa_proplist_free(props);

    bool ok = ns->stream != nullptr;
    if (ok) {
        pa_stream_set_state_callback(ns->stream, streamStateCallback, ns);
        pa_stream_set_write_callback(ns->stream, streamWriteCallback, ns);

        pa_buffer_attr attr;
        attr.maxlength = static_cast<uint32_t>(-1);
        attr.tlength = 30 * kNetBytesPerMs;   // keep the server-side buffer short
        attr.prebuf = 0;                      // we always have data (silence if nothing else)
        attr.minreq = static_cast<uint32_t>(-1);
        attr.fragsize = static_cast<uint32_t>(-1);

        ok = pa_stream_connect_playback(ns->stream, ns->device.constData(), &attr,
                                        PA_STREAM_ADJUST_LATENCY, nullptr, nullptr) == 0;
    }
    if (ok) {
        m_playbackStreams.insert(paName, ns);
    } else {
        qWarning() << "SoundRoot: cannot play network node" << paName;
        if (ns->stream) pa_stream_unref(ns->stream);
        delete ns;
        m_streamFailures.insert(failKey, QDateTime::currentMSecsSinceEpoch());
    }
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::closeStreamLocked(QHash<QString, NetStream *> &streams,
                                          const QString &paName)
{
    NetStream *ns = streams.take(paName);
    if (!ns) return;
    pa_stream_set_state_callback(ns->stream, nullptr, nullptr);
    pa_stream_set_read_callback(ns->stream, nullptr, nullptr);
    pa_stream_set_write_callback(ns->stream, nullptr, nullptr);
    pa_stream_disconnect(ns->stream);
    pa_stream_unref(ns->stream);
    delete ns;
}

void PulseAudioBackend::closeNetworkCapture(const QString &paName)
{
    if (!m_mainloop) return;
    pa_threaded_mainloop_lock(m_mainloop);
    closeStreamLocked(m_captureStreams, paName);
    m_streamFailures.remove(QStringLiteral("c|") + paName);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::closeNetworkPlayback(const QString &paName)
{
    if (!m_mainloop) return;
    pa_threaded_mainloop_lock(m_mainloop);
    closeStreamLocked(m_playbackStreams, paName);
    m_streamFailures.remove(QStringLiteral("p|") + paName);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::streamStateCallback(pa_stream *s, void *userdata)
{
    auto *ns = static_cast<NetStream *>(userdata);
    const pa_stream_state_t state = pa_stream_get_state(s);
    if (state != PA_STREAM_FAILED && state != PA_STREAM_TERMINATED) return;
    // Clean up on the main thread; the stream cannot be freed from inside
    // its own callback.
    QMetaObject::invokeMethod(ns->backend, "handleStreamEnded", Qt::QueuedConnection,
                              Q_ARG(quintptr, reinterpret_cast<quintptr>(s)));
}

void PulseAudioBackend::handleStreamEnded(quintptr stream)
{
    if (!m_mainloop) return;
    pa_threaded_mainloop_lock(m_mainloop);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto drop = [&](QHash<QString, NetStream *> &streams, const QString &tag) {
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            if (reinterpret_cast<quintptr>(it.value()->stream) != stream) continue;
            const QString paName = it.key();
            qWarning() << "SoundRoot: network node stream ended:" << paName;
            closeStreamLocked(streams, paName);
            m_streamFailures.insert(tag + paName, now);   // do not reopen in a tight loop
            return true;
        }
        return false;
    };
    const bool dropped = drop(m_captureStreams, QStringLiteral("c|"))
                         || drop(m_playbackStreams, QStringLiteral("p|"));
    pa_threaded_mainloop_unlock(m_mainloop);
    if (dropped)
        emit sinkInputsChanged();   // lets the engine notice and reopen it
}

void PulseAudioBackend::streamReadCallback(pa_stream *s, size_t /*nbytes*/, void *userdata)
{
    auto *ns = static_cast<NetStream *>(userdata);
    AudioTap *tap = ns->backend->m_tap;

    // Belt and braces for DONT_MOVE: never forward audio from any source
    // other than the node's own monitor.
    const char *device = pa_stream_get_device_name(s);
    const bool rightSource = device && ns->device == device;

    const void *data = nullptr;
    size_t bytes = 0;
    while (pa_stream_peek(s, &data, &bytes) == 0 && bytes > 0) {
        if (data && tap && rightSource)   // data == nullptr is a hole: nothing was played
            tap->capturedAudio(ns->paName, data, bytes);
        pa_stream_drop(s);
    }
}

void PulseAudioBackend::streamWriteCallback(pa_stream *s, size_t nbytes, void *userdata)
{
    auto *ns = static_cast<NetStream *>(userdata);
    AudioTap *tap = ns->backend->m_tap;

    while (nbytes >= kNetFrameBytes) {
        void *buffer = nullptr;
        size_t bytes = nbytes;
        if (pa_stream_begin_write(s, &buffer, &bytes) < 0 || !buffer) return;
        bytes -= bytes % kNetFrameBytes;
        if (bytes == 0) {
            pa_stream_cancel_write(s);
            return;
        }
        if (tap) tap->playbackAudio(ns->paName, buffer, bytes);
        else     memset(buffer, 0, bytes);
        if (pa_stream_write(s, buffer, bytes, nullptr, 0, PA_SEEK_RELATIVE) < 0) return;
        nbytes -= bytes;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Enumeration (PA lock must already be held by caller)
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::refreshSinksLocked()
{
    m_pendingSinks.clear();
    auto *req = new EnumRequest{this, ++m_sinkGen};
    pa_operation *op =
        pa_context_get_sink_info_list(m_context, sinkInfoCallback, req);
    if (op) {
        trackUserdata(req, [req]() { delete req; });
        pa_operation_unref(op);
    } else {
        delete req;
    }
}

void PulseAudioBackend::refreshSinkInputsLocked()
{
    m_pendingSinkInputs.clear();
    auto *req = new EnumRequest{this, ++m_sinkInputGen};
    pa_operation *op =
        pa_context_get_sink_input_info_list(m_context, sinkInputInfoCallback, req);
    if (op) {
        trackUserdata(req, [req]() { delete req; });
        pa_operation_unref(op);
    } else {
        delete req;
    }
}

void PulseAudioBackend::refreshSourcesLocked()
{
    m_pendingSources.clear();
    auto *req = new EnumRequest{this, ++m_sourceGen};
    pa_operation *op =
        pa_context_get_source_info_list(m_context, sourceInfoCallback, req);
    if (op) {
        trackUserdata(req, [req]() { delete req; });
        pa_operation_unref(op);
    } else {
        delete req;
    }
}

void PulseAudioBackend::refreshSourceOutputsLocked()
{
    m_pendingSourceOutputs.clear();
    auto *req = new EnumRequest{this, ++m_sourceOutputGen};
    pa_operation *op =
        pa_context_get_source_output_info_list(m_context, sourceOutputInfoCallback, req);
    if (op) {
        trackUserdata(req, [req]() { delete req; });
        pa_operation_unref(op);
    } else {
        delete req;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PulseAudio static callbacks (called with PA mainloop lock held)
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::contextStateCallback(pa_context *c, void *userdata)
{
    auto *self = static_cast<PulseAudioBackend *>(userdata);

    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        self->m_connected = true;
        self->m_everConnected = true;
        self->m_reportedUnreachable = false;
        self->m_reconnectDelayMs = kReconnectDelayMs;

        // Subscribe to both directions and to server (default device) changes
        pa_context_set_subscribe_callback(c, subscribeCallback, self);
        if (pa_operation *op = pa_context_subscribe(
                c,
                static_cast<pa_subscription_mask_t>(
                    PA_SUBSCRIPTION_MASK_SINK |
                    PA_SUBSCRIPTION_MASK_SINK_INPUT |
                    PA_SUBSCRIPTION_MASK_SOURCE |
                    PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT |
                    PA_SUBSCRIPTION_MASK_SERVER),
                nullptr, nullptr))
            pa_operation_unref(op);

        // Initial population (lock is already held inside callback)
        self->refreshSinksLocked();
        self->refreshSinkInputsLocked();
        self->refreshSourcesLocked();
        self->refreshSourceOutputsLocked();

        // Fetch the initial default-sink name
        if (pa_operation *op = pa_context_get_server_info(c, serverInfoCallback, self))
            pa_operation_unref(op);

        QMetaObject::invokeMethod(self, "serverConnected", Qt::QueuedConnection);
        break;

    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        self->m_connected = false;
        // Reconnect from the main thread (the context cannot be torn down
        // from inside its own callback)
        QMetaObject::invokeMethod(self, "handleServerLost", Qt::QueuedConnection,
                                  Q_ARG(quintptr, reinterpret_cast<quintptr>(c)));
        break;

    default:
        break;
    }
}

void PulseAudioBackend::subscribeCallback(pa_context *c,
                                          pa_subscription_event_type_t t,
                                          uint32_t /*idx*/, void *userdata)
{
    auto *self     = static_cast<PulseAudioBackend *>(userdata);
    auto  facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

    // PA lock is already held
    if (facility == PA_SUBSCRIPTION_EVENT_SERVER) {
        // Default sink may have changed – refresh
        pa_operation *op = pa_context_get_server_info(c, serverInfoCallback, self);
        if (op) pa_operation_unref(op);

    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        self->refreshSinksLocked();   // invalidates any in-flight enumeration

    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        self->refreshSinkInputsLocked();   // invalidates any in-flight enumeration

    } else if (facility == PA_SUBSCRIPTION_EVENT_SOURCE) {
        self->refreshSourcesLocked();

    } else if (facility == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT) {
        self->refreshSourceOutputsLocked();
    }
}

void PulseAudioBackend::sinkInfoCallback(pa_context * /*c*/,
                                         const pa_sink_info *info,
                                         int eol, void *userdata)
{
    auto *req  = static_cast<EnumRequest *>(userdata);
    auto *self = req->backend;
    // A newer enumeration was started after this one — discard its results.
    const bool superseded = (req->gen != self->m_sinkGen);

    if (eol != 0) {
        self->untrackUserdata(req);
        delete req;
        if (eol < 0 || superseded) return;
        int newDefaultVol = -1;
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sinks = self->m_pendingSinks;
            // Find the default sink and pick up its current volume
            for (const auto &s : std::as_const(self->m_sinks)) {
                if (s.name == self->m_defaultSinkName) {
                    newDefaultVol = s.volumePercent;
                    break;
                }
            }
            if (newDefaultVol >= 0)
                self->m_defaultSinkVolume = newDefaultVol;
        }
        self->m_pendingSinks.clear();  // prevent next enumeration from appending to stale data
        QMetaObject::invokeMethod(self, "sinksChanged", Qt::QueuedConnection);
        if (newDefaultVol >= 0) {
            QMetaObject::invokeMethod(self, [self, newDefaultVol]() {
                emit self->defaultSinkVolumeChanged(newDefaultVol);
            }, Qt::QueuedConnection);
        }
        return;
    }
    if (!info || superseded) return;

    PASinkInfo sink;
    sink.index       = info->index;
    sink.name        = QString::fromUtf8(info->name);
    sink.description = QString::fromUtf8(info->description);
    sink.channels    = static_cast<uint8_t>(info->volume.channels);
    sink.isMuted     = (info->mute != 0);
    // Extract average channel volume and convert to 0-150% integer
    sink.volumePercent = static_cast<int>(
        pa_cvolume_avg(&info->volume) * 100.0 / PA_VOLUME_NORM + 0.5);

    self->m_pendingSinks.append(sink);
}

void PulseAudioBackend::sinkInputInfoCallback(pa_context * /*c*/,
                                              const pa_sink_input_info *info,
                                              int eol, void *userdata)
{
    auto *req  = static_cast<EnumRequest *>(userdata);
    auto *self = req->backend;
    // A newer enumeration was started after this one — discard its results.
    const bool superseded = (req->gen != self->m_sinkInputGen);

    if (eol != 0) {
        self->untrackUserdata(req);
        delete req;
        if (eol < 0 || superseded) return;
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sinkInputs = self->m_pendingSinkInputs;
        }
        self->m_pendingSinkInputs.clear();  // prevent next enumeration from appending to stale data
        QMetaObject::invokeMethod(self, "sinkInputsChanged", Qt::QueuedConnection);
        return;
    }
    if (!info || superseded) return;

    PASinkInputInfo si;
    si.index         = info->index;
    si.sinkIndex     = info->sink;
    si.channels      = info->volume.channels;
    si.volumePercent = static_cast<int>(
        pa_cvolume_avg(&info->volume) * 100.0 / PA_VOLUME_NORM + 0.5);

    const char *appName =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    si.appName = appName ? QString::fromUtf8(appName)
                         : QString::fromUtf8(info->name);

    const char *mediaName =
        pa_proplist_gets(info->proplist, PA_PROP_MEDIA_NAME);
    si.mediaName = mediaName ? QString::fromUtf8(mediaName) : QString::fromUtf8(info->name);

    const char *iconName =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_ICON_NAME);
    si.iconName = iconName ? QString::fromUtf8(iconName)
                           : QStringLiteral("audio-card");

    // The playback stream of one of our network nodes: routed like an
    // application, but not listed as one.
    if (si.mediaName.startsWith(netPlaybackPrefix())) {
        si.netNode = si.mediaName.mid(netPlaybackPrefix().size());
        self->m_pendingSinkInputs.append(si);
        return;
    }

    // Filter out module-combine-sink virtual sub-streams.
    // Under PipeWire, the driver field may not be "module-combine-sink.c";
    // instead these streams have media.name like "audiorouter_xxx output"
    // and/or no application.name.
    const char *driver = info->driver;
    if (driver && QByteArray(driver).contains("combine-sink"))
        return;
    if (si.mediaName.startsWith(combineSinkPrefix()))
        return;
    if (si.appName.startsWith(combineSinkPrefix()))
        return;
    // Filter out loopback streams created for virtual sinks
    if (si.mediaName.startsWith(QStringLiteral("soundroot_loopback_")))
        return;
    // Filter out module-generated streams (no client attached: combine-sinks,
    // loopbacks, etc.) that slipped through the name-based filters above.
    if (info->client == PA_INVALID_INDEX)
        return;

    self->m_pendingSinkInputs.append(si);
}

void PulseAudioBackend::sourceInfoCallback(pa_context * /*c*/,
                                           const pa_source_info *info,
                                           int eol, void *userdata)
{
    auto *req  = static_cast<EnumRequest *>(userdata);
    auto *self = req->backend;
    const bool superseded = (req->gen != self->m_sourceGen);

    if (eol != 0) {
        self->untrackUserdata(req);
        delete req;
        if (eol < 0 || superseded) return;
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sources = self->m_pendingSources;
        }
        self->m_pendingSources.clear();
        QMetaObject::invokeMethod(self, "sourcesChanged", Qt::QueuedConnection);
        return;
    }
    if (!info || superseded) return;

    // A monitor carries what an output plays; it is not a microphone.  The
    // node microphones (remaps of a node's monitor) are kept: the engine
    // moves recording applications onto them.
    if (info->monitor_of_sink != PA_INVALID_INDEX) return;

    PASourceInfo source;
    source.index       = info->index;
    source.name        = QString::fromUtf8(info->name);
    source.description = QString::fromUtf8(info->description);
    self->m_pendingSources.append(source);
}

void PulseAudioBackend::sourceOutputInfoCallback(pa_context * /*c*/,
                                                 const pa_source_output_info *info,
                                                 int eol, void *userdata)
{
    auto *req  = static_cast<EnumRequest *>(userdata);
    auto *self = req->backend;
    const bool superseded = (req->gen != self->m_sourceOutputGen);

    if (eol != 0) {
        self->untrackUserdata(req);
        delete req;
        if (eol < 0 || superseded) return;
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sourceOutputs = self->m_pendingSourceOutputs;
        }
        self->m_pendingSourceOutputs.clear();
        QMetaObject::invokeMethod(self, "sourceOutputsChanged", Qt::QueuedConnection);
        return;
    }
    if (!info || superseded) return;

    // Only applications that record.  Left out:
    //  - streams of modules: our node microphones and loopbacks, and the
    //    same kind of thing loaded by anyone else;
    //  - peak meters (volume applets, pavucontrol).  PipeWire marks them
    //    with stream.monitor / resample.peaks; PulseAudio reports the
    //    resampler "peaks";
    //  - our own network capture.
    if (info->owner_module != PA_INVALID_INDEX || info->client == PA_INVALID_INDEX)
        return;
    auto prop = [info](const char *key) {
        const char *v = pa_proplist_gets(info->proplist, key);
        return v ? QByteArray(v) : QByteArray();
    };
    if (prop("stream.monitor") == "true" || prop("resample.peaks") == "true"
        || (info->resample_method && qstrcmp(info->resample_method, "peaks") == 0))
        return;

    PASourceOutputInfo so;
    so.index       = info->index;
    so.sourceIndex = info->source;
    const QByteArray app = prop(PA_PROP_APPLICATION_NAME);
    so.appName   = app.isEmpty() ? QString::fromUtf8(info->name) : QString::fromUtf8(app);
    const QByteArray media = prop(PA_PROP_MEDIA_NAME);
    so.mediaName = media.isEmpty() ? QString::fromUtf8(info->name) : QString::fromUtf8(media);
    const QByteArray icon = prop(PA_PROP_APPLICATION_ICON_NAME);
    so.iconName  = icon.isEmpty() ? QStringLiteral("audio-input-microphone") : QString::fromUtf8(icon);

    if (so.mediaName.startsWith(netCapturePrefix())) return;

    self->m_pendingSourceOutputs.append(so);
}

void PulseAudioBackend::successCallback(pa_context * /*c*/,
                                        int success, void * /*userdata*/)
{
    if (!success)
        qWarning() << "SoundRoot: PulseAudio operation failed";
}

void PulseAudioBackend::serverInfoCallback(pa_context * /*c*/,
                                           const pa_server_info *info,
                                           void *userdata)
{
    if (!info) return;
    auto *self = static_cast<PulseAudioBackend *>(userdata);
    const QString name = QString::fromUtf8(info->default_sink_name);
    const QString sourceName = QString::fromUtf8(info->default_source_name);

    int newVol = -1;
    bool changed = false;
    bool sourceChanged = false;
    {
        QMutexLocker lock(&self->m_mutex);
        changed = self->m_defaultSinkName != name;
        self->m_defaultSinkName = name;
        sourceChanged = self->m_defaultSourceName != sourceName;
        self->m_defaultSourceName = sourceName;
        // If we already have the sink list, extract volume immediately
        for (const auto &s : std::as_const(self->m_sinks)) {
            if (s.name == name) {
                newVol = s.volumePercent;
                self->m_defaultSinkVolume = newVol;
                break;
            }
        }
    }
    if (changed)   // which device is the default is part of the sink list for the UI
        QMetaObject::invokeMethod(self, "sinksChanged", Qt::QueuedConnection);
    if (sourceChanged)
        QMetaObject::invokeMethod(self, "sourcesChanged", Qt::QueuedConnection);
    if (newVol >= 0) {
        QMetaObject::invokeMethod(self, [self, newVol]() {
            emit self->defaultSinkVolumeChanged(newVol);
        }, Qt::QueuedConnection);
    }
}
