#include "audiobackend.h"

#include <QDebug>
#include <QTimer>
#include <QtGlobal>

// Data passed through PulseAudio module-load callback
struct ModuleLoadData {
    PulseAudioBackend *backend;
    QString combinedName;
};

// ═══════════════════════════════════════════════════════════════════════════
// Construction / destruction
// ═══════════════════════════════════════════════════════════════════════════

PulseAudioBackend::PulseAudioBackend(QObject *parent)
    : QObject(parent)
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
    m_mainloop = pa_threaded_mainloop_new();
    if (!m_mainloop) {
        emit errorOccurred(QStringLiteral("Failed to create PulseAudio mainloop"));
        return;
    }

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_mainloop);
    m_context = pa_context_new(api, "AudioRouter");
    if (!m_context) {
        emit errorOccurred(QStringLiteral("Failed to create PulseAudio context"));
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
        return;
    }

    pa_context_set_state_callback(m_context, contextStateCallback, this);

    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0) {
        emit errorOccurred(QStringLiteral("Failed to connect to PulseAudio server"));
        pa_context_unref(m_context);
        m_context = nullptr;
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
        return;
    }

    pa_threaded_mainloop_start(m_mainloop);
}

void PulseAudioBackend::shutdown()
{
    if (m_mainloop)
        pa_threaded_mainloop_stop(m_mainloop);

    // Clean up any combined sinks we created
    if (m_context && m_connected) {
        for (auto it = m_combinedModules.cbegin(); it != m_combinedModules.cend(); ++it)
            pa_context_unload_module(m_context, it.value(), nullptr, nullptr);
        m_combinedModules.clear();
    }

    if (m_context) {
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }

    if (m_mainloop) {
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }

    m_connected = false;
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

int PulseAudioBackend::defaultSinkVolume() const
{
    QMutexLocker lock(&m_mutex);
    return m_defaultSinkVolume;
}

// ═══════════════════════════════════════════════════════════════════════════
// Actions
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

void PulseAudioBackend::setSinkInputVolume(uint32_t sinkInputIndex,
                                           int volumePercent,
                                           uint8_t channels)
{
    if (!m_context || !m_connected) return;

    const int clampedPercent = qBound(0, volumePercent, 150);

    // Use raw PA linear volume: 100% == PA_VOLUME_NORM == 65536.
    // KDE/pavucontrol use the same convention; pa_sw_volume_from_linear applies
    // a cube-root curve that would make our percentages mismatch the system mixer.
    pa_cvolume volume;
    pa_cvolume_set(&volume,
                   qMax<uint8_t>(1, channels),
                   static_cast<pa_volume_t>(clampedPercent / 100.0 * PA_VOLUME_NORM));

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_set_sink_input_volume(
        m_context, sinkInputIndex, &volume, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::createCombinedSink(const QString &name,
                                           const QStringList &sinkNames)
{
    if (!m_context || !m_connected || sinkNames.isEmpty()) return;

    // module-combine-sink arguments
    const QString slaves = sinkNames.join(QLatin1Char(','));
    const QString args   = QStringLiteral("sink_name=%1 slaves=%2").arg(name, slaves);

    auto *data = new ModuleLoadData{this, name};

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_load_module(
        m_context, "module-combine-sink",
        args.toUtf8().constData(),
        moduleLoadCallback, data);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::destroyCombinedSink(const QString &name)
{
    if (!m_context || !m_connected) return;

    uint32_t moduleIdx;
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_combinedModules.find(name);
        if (it == m_combinedModules.end()) return;
        moduleIdx = it.value();
        m_combinedModules.erase(it);
    }

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_unload_module(
        m_context, moduleIdx, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::applyRoute(const QString &appName,
                                   const QStringList &outputSinkNames,
                                   int volumePercent)
{
    if (outputSinkNames.isEmpty()) return;

    // Parse per-stream key: "Firefox\t1" → base="Firefox", ordinal=1
    // Legacy keys without \t match ALL streams of that app (ordinal = -1)
    QString baseAppName = appName;
    int ordinal = -1;
    int tabIdx = appName.indexOf(QLatin1Char('\t'));
    if (tabIdx >= 0) {
        baseAppName = appName.left(tabIdx);
        bool ok = false;
        ordinal = appName.mid(tabIdx + 1).toInt(&ok);
        if (!ok) ordinal = -1;
    }

    // Collect ALL matching sink-inputs for this application
    QVector<QPair<uint32_t, uint8_t>> matches; // (index, channels)
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &si : m_sinkInputs) {
            if (si.appName == baseAppName)
                matches.append({si.index, si.channels});
        }
    }

    // If ordinal specified, only route the Nth matching stream
    if (ordinal >= 0) {
        if (ordinal < matches.size()) {
            auto single = matches[ordinal];
            matches.clear();
            matches.append(single);
        } else {
            // Requested stream ordinal doesn't exist yet — skip silently
            return;
        }
    }

    if (matches.isEmpty()) {
        qWarning() << "AudioRouter: no active stream found for" << baseAppName;
        return;
    }

    if (outputSinkNames.size() == 1) {
        // Single output – find the sink index and move directly
        uint32_t targetSinkIdx = PA_INVALID_INDEX;
        {
            QMutexLocker lock(&m_mutex);
            for (const auto &sink : m_sinks) {
                if (sink.name == outputSinkNames.first()) {
                    targetSinkIdx = sink.index;
                    break;
                }
            }
        }
        if (targetSinkIdx == PA_INVALID_INDEX) {
            qWarning() << "AudioRouter: sink not found:" << outputSinkNames.first();
            return;
        }
        for (const auto &[idx, channels] : matches) {
            moveSinkInput(idx, targetSinkIdx);
            setSinkInputVolume(idx, volumePercent, channels);
        }
    } else {
        // Multiple outputs – filter the list to only sinks that currently exist,
        // then create a combined sink with the available subset.
        QStringList availableSinks;
        {
            QMutexLocker lock(&m_mutex);
            for (const auto &requested : outputSinkNames) {
                for (const auto &sink : m_sinks) {
                    if (sink.name == requested) {
                        availableSinks.append(requested);
                        break;
                    }
                }
            }
        }

        if (availableSinks.isEmpty()) {
            qWarning() << "AudioRouter: none of the target sinks are currently"
                          " available for" << appName << "— skipping";
            return;
        }

        if (availableSinks.size() == 1) {
            // Only one of the targets is online; send directly, no combine needed
            uint32_t targetSinkIdx = PA_INVALID_INDEX;
            {
                QMutexLocker lock(&m_mutex);
                for (const auto &sink : m_sinks) {
                    if (sink.name == availableSinks.first()) {
                        targetSinkIdx = sink.index;
                        break;
                    }
                }
            }
            if (targetSinkIdx == PA_INVALID_INDEX) return;
            for (const auto &[idx, channels] : matches) {
                moveSinkInput(idx, targetSinkIdx);
                setSinkInputVolume(idx, volumePercent, channels);
            }
            return;
        }

        // Two or more available – create a combined sink, then move streams to it
        const QString combinedName =
            QStringLiteral("audiorouter_%1").arg(
                QString(baseAppName).toLower().replace(QLatin1Char(' '), QLatin1Char('_')));

        destroyCombinedSink(combinedName);
        createCombinedSink(combinedName, availableSinks);

        // Combined sink creation is async; wait for PA to register it then move
        const auto capturedMatches = matches;
        QTimer::singleShot(800, this, [this, capturedMatches, combinedName, volumePercent]() {
            moveSinkInputsToCombined(combinedName, capturedMatches, volumePercent, 0);
        });
    }
}

void PulseAudioBackend::moveSinkInputsToCombined(
    const QString &combinedName,
    const QVector<QPair<uint32_t, uint8_t>> &sinkInputs,
    int volumePercent,
    int retryCount)
{
    uint32_t combinedSinkIdx = PA_INVALID_INDEX;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &sink : m_sinks) {
            if (sink.name == combinedName) {
                combinedSinkIdx = sink.index;
                break;
            }
        }
    }

    if (combinedSinkIdx == PA_INVALID_INDEX) {
        if (retryCount < 4) {
            QTimer::singleShot(500, this,
                [this, combinedName, sinkInputs, volumePercent, retryCount]() {
                    moveSinkInputsToCombined(combinedName, sinkInputs,
                                            volumePercent, retryCount + 1);
                });
        } else {
            qWarning() << "AudioRouter: combined sink not available after retries:"
                       << combinedName;
        }
        return;
    }

    for (const auto &[idx, channels] : sinkInputs) {
        moveSinkInput(idx, combinedSinkIdx);
        setSinkInputVolume(idx, volumePercent, channels);
    }
}

void PulseAudioBackend::removeRoute(const QString &appName)
{
    // Strip per-stream ordinal suffix for combine-sink naming
    QString baseAppName = appName;
    int tabIdx = appName.indexOf(QLatin1Char('\t'));
    if (tabIdx >= 0)
        baseAppName = appName.left(tabIdx);

    const QString combinedName =
        QStringLiteral("audiorouter_%1").arg(
            QString(baseAppName).toLower().replace(QLatin1Char(' '), QLatin1Char('_')));
    destroyCombinedSink(combinedName);
}

void PulseAudioBackend::moveInputToSink(uint32_t sinkInputIndex,
                                        const QString &sinkName)
{
    if (!m_context || !m_connected || sinkName.isEmpty()) return;

    uint32_t sinkIdx = PA_INVALID_INDEX;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &s : m_sinks) {
            if (s.name == sinkName) { sinkIdx = s.index; break; }
        }
    }
    if (sinkIdx == PA_INVALID_INDEX) {
        qWarning() << "AudioRouter: moveInputToSink: sink not found:" << sinkName;
        return;
    }
    moveSinkInput(sinkInputIndex, sinkIdx);
}

void PulseAudioBackend::routeStreamToSinks(uint32_t sinkInputIndex,
                                           const QStringList &sinkNames)
{
    if (!m_context || !m_connected || sinkNames.isEmpty()) return;

    // Clean up any previous per-stream combine sink
    const QString combinedName =
        QStringLiteral("audiorouter_stream_%1").arg(sinkInputIndex);
    destroyCombinedSink(combinedName);

    if (sinkNames.size() == 1) {
        moveInputToSink(sinkInputIndex, sinkNames.first());
        return;
    }

    // Filter to sinks that currently exist
    QStringList availableSinks;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &requested : sinkNames) {
            for (const auto &sink : m_sinks) {
                if (sink.name == requested) {
                    availableSinks.append(requested);
                    break;
                }
            }
        }
    }

    if (availableSinks.isEmpty()) return;

    if (availableSinks.size() == 1) {
        moveInputToSink(sinkInputIndex, availableSinks.first());
        return;
    }

    // Find channels for this stream
    uint8_t channels = 2;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &si : m_sinkInputs) {
            if (si.index == sinkInputIndex) {
                channels = si.channels;
                break;
            }
        }
    }

    // Create combine sink and move stream to it
    createCombinedSink(combinedName, availableSinks);

    QVector<QPair<uint32_t, uint8_t>> inputs;
    inputs.append({sinkInputIndex, channels});
    QTimer::singleShot(800, this, [this, inputs, combinedName]() {
        moveSinkInputsToCombined(combinedName, inputs, 100, 0);
    });
}

void PulseAudioBackend::cleanupStreamCombineSinks()
{
    QStringList toRemove;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_combinedModules.begin(); it != m_combinedModules.end(); ++it) {
            if (it.key().startsWith(QStringLiteral("audiorouter_stream_")))
                toRemove.append(it.key());
        }
    }
    for (const auto &name : toRemove)
        destroyCombinedSink(name);
}

void PulseAudioBackend::restoreRoute(const QString &appName)
{
    if (!m_context || !m_connected) return;

    // Parse per-stream key: "Firefox\t1" → base="Firefox", ordinal=1
    QString baseAppName = appName;
    int ordinal = -1;
    int tabIdx = appName.indexOf(QLatin1Char('\t'));
    if (tabIdx >= 0) {
        baseAppName = appName.left(tabIdx);
        bool ok = false;
        ordinal = appName.mid(tabIdx + 1).toInt(&ok);
        if (!ok) ordinal = -1;
    }

    // Find the default sink index. Prefer the stored PA default; fall back to
    // the first non-audiorouter ALSA sink so we never leave streams homeless.
    uint32_t defaultIdx = PA_INVALID_INDEX;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &sink : m_sinks) {
            if (sink.name == m_defaultSinkName) {
                defaultIdx = sink.index;
                break;
            }
        }
        if (defaultIdx == PA_INVALID_INDEX) {
            for (const auto &sink : m_sinks) {
                if (!sink.name.startsWith(QStringLiteral("audiorouter_"))) {
                    defaultIdx = sink.index;
                    break;
                }
            }
        }
    }

    if (defaultIdx == PA_INVALID_INDEX) {
        qWarning() << "AudioRouter: cannot restore" << baseAppName
                   << "— no default sink found";
        return;
    }

    QVector<uint32_t> toMove;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &si : m_sinkInputs) {
            if (si.appName == baseAppName)
                toMove.append(si.index);
        }
    }

    // If ordinal specified, only restore the Nth matching stream
    if (ordinal >= 0) {
        if (ordinal < toMove.size()) {
            uint32_t single = toMove[ordinal];
            toMove.clear();
            toMove.append(single);
        } else {
            return;
        }
    }

    for (uint32_t idx : toMove)
        moveSinkInput(idx, defaultIdx);
}

void PulseAudioBackend::setDefaultSinkVolume(int percent)
{
    if (!m_context || !m_connected) return;

    const int clamped = qBound(0, percent, 150);

    pa_cvolume volume;
    pa_cvolume_set(&volume, 2,
                   static_cast<pa_volume_t>(clamped / 100.0 * PA_VOLUME_NORM));

    QString sinkName;
    { QMutexLocker lock(&m_mutex); sinkName = m_defaultSinkName; }

    if (sinkName.isEmpty()) return;

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_set_sink_volume_by_name(
        m_context, sinkName.toUtf8().constData(),
        &volume, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::toggleSinkMuteByName(const QString &sinkName)
{
    if (!m_context || !m_connected || sinkName.isEmpty()) return;

    // Read current mute state, then flip it
    bool currentlyMuted = false;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &s : m_sinks) {
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

void PulseAudioBackend::setSinkVolumeByName(const QString &sinkName, int percent)
{
    if (!m_context || !m_connected || sinkName.isEmpty()) return;

    const int clamped = qBound(0, percent, 150);

    // Find the channel count for this sink so we set all channels correctly
    uint8_t channels = 2;
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &s : m_sinks) {
            if (s.name == sinkName) { channels = s.channels; break; }
        }
    }

    pa_cvolume volume;
    pa_cvolume_set(&volume, qMax<uint8_t>(1, channels),
                   static_cast<pa_volume_t>(clamped / 100.0 * PA_VOLUME_NORM));

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_set_sink_volume_by_name(
        m_context, sinkName.toUtf8().constData(),
        &volume, successCallback, this);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);
}

// ═══════════════════════════════════════════════════════════════════════════
// Internal refresh (PA lock must already be held by caller)
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::refreshSinksLocked()
{
    m_pendingSinks.clear();
    pa_operation *op =
        pa_context_get_sink_info_list(m_context, sinkInfoCallback, this);
    if (op) pa_operation_unref(op);
}

void PulseAudioBackend::refreshSinkInputsLocked()
{
    m_pendingSinkInputs.clear();
    pa_operation *op =
        pa_context_get_sink_input_info_list(m_context, sinkInputInfoCallback, this);
    if (op) pa_operation_unref(op);
}

// Public-facing (acquires PA lock)
void PulseAudioBackend::refreshSinks()
{
    if (!m_context || !m_connected) return;
    pa_threaded_mainloop_lock(m_mainloop);
    refreshSinksLocked();
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::refreshSinkInputs()
{
    if (!m_context || !m_connected) return;
    pa_threaded_mainloop_lock(m_mainloop);
    refreshSinkInputsLocked();
    pa_threaded_mainloop_unlock(m_mainloop);
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

        // Subscribe to sink, sink-input, and server (default-sink) changes
        pa_context_set_subscribe_callback(c, subscribeCallback, self);
        pa_operation_unref(pa_context_subscribe(
            c,
            static_cast<pa_subscription_mask_t>(
                PA_SUBSCRIPTION_MASK_SINK |
                PA_SUBSCRIPTION_MASK_SINK_INPUT |
                PA_SUBSCRIPTION_MASK_SERVER),
            nullptr, nullptr));

        // Initial population (lock is already held inside callback)
        self->refreshSinksLocked();
        self->refreshSinkInputsLocked();

        // Fetch the initial default-sink name
        {
            pa_operation *op = pa_context_get_server_info(c, serverInfoCallback, self);
            if (op) pa_operation_unref(op);
        }

        QMetaObject::invokeMethod(self, "serverConnected", Qt::QueuedConnection);
        break;

    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        self->m_connected = false;
        QMetaObject::invokeMethod(self, "serverDisconnected", Qt::QueuedConnection);
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
    auto  evtype   = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    // PA lock is already held
    if (facility == PA_SUBSCRIPTION_EVENT_SERVER) {
        // Default sink may have changed – refresh
        pa_operation *op = pa_context_get_server_info(c, serverInfoCallback, self);
        if (op) pa_operation_unref(op);

    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        self->m_pendingSinks.clear();
        pa_operation *op =
            pa_context_get_sink_info_list(c, sinkInfoCallback, self);
        if (op) pa_operation_unref(op);

    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        // Only flag a re-apply when a brand-new stream appears, not on
        // move/volume CHANGE events (which we ourselves trigger).
        if (evtype == PA_SUBSCRIPTION_EVENT_NEW)
            self->m_hasNewSinkInput = true;

        self->m_pendingSinkInputs.clear();
        pa_operation *op =
            pa_context_get_sink_input_info_list(c, sinkInputInfoCallback, self);
        if (op) pa_operation_unref(op);
    }
}

void PulseAudioBackend::sinkInfoCallback(pa_context * /*c*/,
                                         const pa_sink_info *info,
                                         int eol, void *userdata)
{
    auto *self = static_cast<PulseAudioBackend *>(userdata);

    if (eol > 0) {
        int newDefaultVol = -1;
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sinks = self->m_pendingSinks;
            // Find the default sink and pick up its current volume
            for (const auto &s : self->m_sinks) {
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
    if (!info) return;

    PASinkInfo sink;
    sink.index       = info->index;
    sink.name        = QString::fromUtf8(info->name);
    sink.description = QString::fromUtf8(info->description);
    sink.channels    = static_cast<uint8_t>(info->volume.channels);
    sink.isMuted     = (info->mute != 0);
    // Extract average channel volume and convert to 0-150% integer
    sink.volumePercent = static_cast<int>(
        pa_cvolume_avg(&info->volume) * 100.0 / PA_VOLUME_NORM + 0.5);

    // Hide internal combine sinks we created for multi-output routing
    if (sink.name.startsWith(QStringLiteral("audiorouter_")))
        return;

    self->m_pendingSinks.append(sink);
}

void PulseAudioBackend::sinkInputInfoCallback(pa_context * /*c*/,
                                              const pa_sink_input_info *info,
                                              int eol, void *userdata)
{
    auto *self = static_cast<PulseAudioBackend *>(userdata);

    if (eol > 0) {
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sinkInputs = self->m_pendingSinkInputs;
        }
        self->m_pendingSinkInputs.clear();  // prevent next enumeration from appending to stale data
        QMetaObject::invokeMethod(self, "sinkInputsChanged", Qt::QueuedConnection);
        // Only trigger re-apply when a brand-new stream appeared, not on
        // every volume/move CHANGE that we ourselves issue.
        if (self->m_hasNewSinkInput) {
            self->m_hasNewSinkInput = false;
            QMetaObject::invokeMethod(self, "sinkInputAdded", Qt::QueuedConnection);
        }
        return;
    }
    if (!info) return;

    PASinkInputInfo si;
    si.index     = info->index;
    si.sinkIndex = info->sink;
    si.channels  = info->volume.channels;

    const char *appName =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    si.appName = appName ? QString::fromUtf8(appName)
                         : QString::fromUtf8(info->name);

    const char *mediaName =
        pa_proplist_gets(info->proplist, PA_PROP_MEDIA_NAME);
    si.mediaName = mediaName ? QString::fromUtf8(mediaName) : QString();

    const char *iconName =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_ICON_NAME);
    si.iconName = iconName ? QString::fromUtf8(iconName)
                           : QStringLiteral("audio-card");

    // Filter out module-combine-sink virtual sub-streams.
    // Under PipeWire, the driver field may not be "module-combine-sink.c";
    // instead these streams have media.name like "audiorouter_xxx output"
    // and/or no application.name.
    const char *driver = info->driver;
    if (driver && QByteArray(driver).contains("combine-sink"))
        return;
    if (si.mediaName.startsWith(QStringLiteral("audiorouter_")))
        return;
    if (si.appName.startsWith(QStringLiteral("audiorouter_")))
        return;

    self->m_pendingSinkInputs.append(si);
}

void PulseAudioBackend::moduleLoadCallback(pa_context * /*c*/,
                                           uint32_t idx, void *userdata)
{
    auto *data = static_cast<ModuleLoadData *>(userdata);
    auto *self = data->backend;

    if (idx == PA_INVALID_INDEX) {
        QMetaObject::invokeMethod(
            self, "errorOccurred", Qt::QueuedConnection,
            Q_ARG(QString, QStringLiteral("Failed to load combined sink module")));
    } else {
        QMutexLocker lock(&self->m_mutex);
        self->m_combinedModules[data->combinedName] = idx;
    }
    delete data;
}

void PulseAudioBackend::successCallback(pa_context * /*c*/,
                                        int success, void * /*userdata*/)
{
    if (!success)
        qWarning() << "AudioRouter: PulseAudio operation failed";
}

void PulseAudioBackend::serverInfoCallback(pa_context * /*c*/,
                                           const pa_server_info *info,
                                           void *userdata)
{
    if (!info) return;
    auto *self = static_cast<PulseAudioBackend *>(userdata);
    const QString name = QString::fromUtf8(info->default_sink_name);

    int newVol = -1;
    {
        QMutexLocker lock(&self->m_mutex);
        self->m_defaultSinkName = name;
        // If we already have the sink list, extract volume immediately
        for (const auto &s : self->m_sinks) {
            if (s.name == name) {
                newVol = s.volumePercent;
                self->m_defaultSinkVolume = newVol;
                break;
            }
        }
    }
    if (newVol >= 0) {
        QMetaObject::invokeMethod(self, [self, newVol]() {
            emit self->defaultSinkVolumeChanged(newVol);
        }, Qt::QueuedConnection);
    }
}
