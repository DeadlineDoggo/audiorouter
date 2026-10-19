#include "audiobackend.h"

#include <QDebug>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>

// Data passed through PulseAudio module-load callback
struct ModuleLoadData {
    PulseAudioBackend *backend;
    QString combinedName;
};

// Data passed through virtual-sink / loopback module-load callbacks
struct VirtualModuleLoadData {
    PulseAudioBackend *backend;
    QString key;       // paName for null-sinks; "paName|sinkName" for loopbacks
    bool isLoopback;
};

// Data passed through sink / sink-input list callbacks: the generation the
// enumeration was started with, so results of a superseded one are dropped.
struct EnumRequest {
    PulseAudioBackend *backend;
    uint64_t gen;
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
    // Unload combined sink modules while the mainloop is still running so the
    // operations actually execute.  Under native PulseAudio (unlike PipeWire)
    // modules are NOT auto-removed when a client disconnects.
    if (m_mainloop && m_context && m_connected) {
        pa_threaded_mainloop_lock(m_mainloop);
        for (auto it = m_combinedModules.cbegin(); it != m_combinedModules.cend(); ++it)
            pa_context_unload_module(m_context, it.value(), nullptr, nullptr);
        m_combinedModules.clear();
        m_combinedSlaves.clear();
        pa_threaded_mainloop_unlock(m_mainloop);
        destroyAllVirtualSinks();
    }

    if (m_mainloop)
        pa_threaded_mainloop_stop(m_mainloop);

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
        if (it == m_combinedModules.end()) {
            // Module entry absent (e.g. load failed), but still clean up slaves
            m_combinedSlaves.remove(name);
            return;
        }
        moduleIdx = it.value();
        m_combinedModules.erase(it);
        m_combinedSlaves.remove(name);
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
                                           const QStringList &sinkNames,
                                           int volumePercent)
{
    if (!m_context || !m_connected || sinkNames.isEmpty()) return;

    const QString combinedName =
        QStringLiteral("audiorouter_stream_%1").arg(sinkInputIndex);

    if (sinkNames.size() == 1) {
        // Single sink — destroy any existing combine for this stream
        destroyCombinedSink(combinedName);
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
        destroyCombinedSink(combinedName);
        moveInputToSink(sinkInputIndex, availableSinks.first());
        return;
    }

    // Check if a combine-sink with the same slaves already exists
    {
        QMutexLocker lock(&m_mutex);
        if (m_combinedModules.contains(combinedName)) {
            auto existingSlaves = m_combinedSlaves.value(combinedName);
            if (existingSlaves == availableSinks) {
                // Combine-sink is correct — just ensure stream is on it
                for (const auto &sink : m_sinks) {
                    if (sink.name == combinedName) {
                        // Check if stream is already on this sink
                        for (const auto &si : m_sinkInputs) {
                            if (si.index == sinkInputIndex && si.sinkIndex == sink.index)
                                return;  // Already correctly routed
                        }
                        // Stream exists but on wrong sink — move it
                        moveSinkInput(sinkInputIndex, sink.index);
                        return;
                    }
                }
                // Combine-sink module exists but sink not yet in m_sinks —
                // wait for PA to register it (will be picked up by next reapply)
                return;
            }
        }
    }

    // Different slaves needed — destroy old and create new
    destroyCombinedSink(combinedName);

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
    {
        QMutexLocker lock(&m_mutex);
        m_combinedSlaves[combinedName] = availableSinks;
    }
    createCombinedSink(combinedName, availableSinks);

    QVector<QPair<uint32_t, uint8_t>> inputs;
    inputs.append({sinkInputIndex, channels});
    QTimer::singleShot(800, this, [this, inputs, combinedName, volumePercent]() {
        moveSinkInputsToCombined(combinedName, inputs, volumePercent, 0);
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

// ═══════════════════════════════════════════════════════════════════════════
// Virtual null-sink / loopback management
// ═══════════════════════════════════════════════════════════════════════════

void PulseAudioBackend::createVirtualSink(const QString &paName,
                                          const QString &displayName)
{
    if (!m_context || !m_connected) return;

    // The description is wrapped as sink_properties='device.description="…"',
    // so strip the quote characters that would break that nesting.
    QString safe = displayName.isEmpty() ? paName : displayName;
    safe.remove(QLatin1Char('"')).remove(QLatin1Char('\'')).remove(QLatin1Char('\\'));
    const QString args =
        QStringLiteral("sink_name=%1 sink_properties='device.description=\"%2\"'")
            .arg(paName, safe);

    pa_threaded_mainloop_lock(m_mainloop);
    m_wantedVirtualSinks.insert(paName);
    if (!m_virtualSinkModules.contains(paName) && !m_pendingVirtualLoads.contains(paName)) {
        auto *data = new VirtualModuleLoadData{this, paName, false};
        pa_operation *op = pa_context_load_module(
            m_context, "module-null-sink",
            args.toUtf8().constData(),
            virtualModuleLoadCallback, data);
        if (op) {
            m_pendingVirtualLoads.insert(paName);
            pa_operation_unref(op);
        } else {
            delete data;
        }
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
    for (auto it = m_wantedLoopbacks.begin(); it != m_wantedLoopbacks.end();) {
        if (it->startsWith(prefix)) it = m_wantedLoopbacks.erase(it);
        else ++it;
    }

    // Destroy all loopbacks for this virtual sink first
    for (auto it = m_loopbackModules.begin(); it != m_loopbackModules.end();) {
        if (it.key().startsWith(prefix)) {
            unloadModuleLocked(it.value());
            it = m_loopbackModules.erase(it);
        } else {
            ++it;
        }
    }

    // Destroy the null-sink module
    auto it = m_virtualSinkModules.find(paName);
    if (it != m_virtualSinkModules.end()) {
        unloadModuleLocked(it.value());
        m_virtualSinkModules.erase(it);
    }

    pa_threaded_mainloop_unlock(m_mainloop);
}

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

    // Drop loopbacks that are no longer wanted
    for (auto it = m_wantedLoopbacks.begin(); it != m_wantedLoopbacks.end();) {
        if (it->startsWith(prefix) && !desired.contains(*it)) {
            auto loaded = m_loopbackModules.find(*it);
            if (loaded != m_loopbackModules.end()) {
                unloadModuleLocked(loaded.value());
                m_loopbackModules.erase(loaded);
            }
            it = m_wantedLoopbacks.erase(it);
        } else {
            ++it;
        }
    }

    m_wantedLoopbacks.unite(desired);

    // If the null-sink is still loading, its load callback creates these.
    if (m_virtualSinkModules.contains(paName))
        loadMissingLoopbacksLocked(paName);

    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::destroyAllVirtualSinks()
{
    if (!m_context || !m_connected) return;

    pa_threaded_mainloop_lock(m_mainloop);
    m_wantedLoopbacks.clear();
    m_wantedVirtualSinks.clear();
    for (auto it = m_loopbackModules.cbegin(); it != m_loopbackModules.cend(); ++it)
        unloadModuleLocked(it.value());
    m_loopbackModules.clear();
    for (auto it = m_virtualSinkModules.cbegin(); it != m_virtualSinkModules.cend(); ++it)
        unloadModuleLocked(it.value());
    m_virtualSinkModules.clear();
    pa_threaded_mainloop_unlock(m_mainloop);
}

void PulseAudioBackend::unloadModuleLocked(uint32_t moduleIndex)
{
    pa_operation *op = pa_context_unload_module(m_context, moduleIndex, nullptr, nullptr);
    if (op) pa_operation_unref(op);
}

void PulseAudioBackend::loadMissingLoopbacksLocked(const QString &paName)
{
    const QString prefix = paName + QLatin1Char('|');
    const QString monitorSource = paName + QStringLiteral(".monitor");

    for (const auto &key : std::as_const(m_wantedLoopbacks)) {
        if (!key.startsWith(prefix) || m_loopbackModules.contains(key) ||
            m_pendingVirtualLoads.contains(key))
            continue;

        const QString sinkName = key.mid(prefix.size());
        // Tag the sink-input so we can filter it from the app list
        const QString args = QStringLiteral(
            "source=%1 sink=%2 latency_msec=50 "
            "sink_input_properties=media.name=soundroot_loopback_%3")
                .arg(monitorSource, sinkName, paName);

        auto *data = new VirtualModuleLoadData{this, key, true};
        pa_operation *op = pa_context_load_module(
            m_context, "module-loopback",
            args.toUtf8().constData(),
            virtualModuleLoadCallback, data);
        if (op) {
            m_pendingVirtualLoads.insert(key);
            pa_operation_unref(op);
        } else {
            delete data;
        }
    }
}

void PulseAudioBackend::virtualModuleLoadCallback(pa_context * /*c*/,
                                                   uint32_t idx, void *userdata)
{
    // Runs on the PA thread with the mainloop lock held.
    auto *data = static_cast<VirtualModuleLoadData *>(userdata);
    auto *self = data->backend;
    const QString key = data->key;
    const bool isLoopback = data->isLoopback;
    delete data;

    self->m_pendingVirtualLoads.remove(key);
    const bool wanted = isLoopback ? self->m_wantedLoopbacks.contains(key)
                                   : self->m_wantedVirtualSinks.contains(key);

    if (idx == PA_INVALID_INDEX) {
        qWarning() << "AudioRouter: Failed to load"
                   << (isLoopback ? "loopback" : "virtual sink")
                   << "module for" << key;
        // Forget it so a later create/set call can retry
        if (isLoopback) self->m_wantedLoopbacks.remove(key);
        else            self->m_wantedVirtualSinks.remove(key);
    } else if (!wanted) {
        // Destroyed or replaced while the load was in flight
        self->unloadModuleLocked(idx);
    } else if (isLoopback) {
        self->m_loopbackModules.insert(key, idx);
    } else {
        self->m_virtualSinkModules.insert(key, idx);
        self->loadMissingLoopbacksLocked(key);
    }
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

    QString sinkName;
    uint8_t channels = 2;
    {
        QMutexLocker lock(&m_mutex);
        sinkName = m_defaultSinkName;
        for (const auto &s : m_sinks) {
            if (s.name == sinkName) { channels = s.channels; break; }
        }
    }

    if (sinkName.isEmpty()) return;

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
    auto *req = new EnumRequest{this, ++m_sinkGen};
    pa_operation *op =
        pa_context_get_sink_info_list(m_context, sinkInfoCallback, req);
    if (op) pa_operation_unref(op);
    else    delete req;
}

void PulseAudioBackend::refreshSinkInputsLocked()
{
    m_pendingSinkInputs.clear();
    auto *req = new EnumRequest{this, ++m_sinkInputGen};
    pa_operation *op =
        pa_context_get_sink_input_info_list(m_context, sinkInputInfoCallback, req);
    if (op) pa_operation_unref(op);
    else    delete req;
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
        self->refreshSinksLocked();   // invalidates any in-flight enumeration

    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        // Only flag a re-apply when a brand-new stream appears, not on
        // move/volume CHANGE events (which we ourselves trigger).
        if (evtype == PA_SUBSCRIPTION_EVENT_NEW)
            self->m_hasNewSinkInput = true;

        self->refreshSinkInputsLocked();   // invalidates any in-flight enumeration
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
        delete req;
        if (eol < 0 || superseded) return;
        int newDefaultVol = -1;
        bool virtualAppeared = false;
        {
            QMutexLocker lock(&self->m_mutex);
            // Did one of our virtual null-sinks just show up?
            for (const auto &s : std::as_const(self->m_pendingSinks)) {
                if (!s.name.startsWith(QStringLiteral("soundroot_virt_")))
                    continue;
                const bool known = std::any_of(self->m_sinks.cbegin(), self->m_sinks.cend(),
                    [&s](const PASinkInfo &old) { return old.name == s.name; });
                if (!known) { virtualAppeared = true; break; }
            }
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
        if (virtualAppeared)
            QMetaObject::invokeMethod(self, "virtualSinkAppeared", Qt::QueuedConnection);
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
        delete req;
        if (eol < 0 || superseded) return;
        int newCount;
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sinkInputs = self->m_pendingSinkInputs;
            newCount = self->m_sinkInputs.size();
        }
        self->m_pendingSinkInputs.clear();  // prevent next enumeration from appending to stale data
        QMetaObject::invokeMethod(self, "sinkInputsChanged", Qt::QueuedConnection);
        // Only trigger re-apply when the post-filter stream count actually
        // grew.  m_hasNewSinkInput alone isn't enough because combine-sink
        // virtual sub-streams fire PA_SUBSCRIPTION_EVENT_NEW but are then
        // filtered out — they should not trigger a reapply loop.
        if (self->m_hasNewSinkInput && newCount > self->m_lastSinkInputCount) {
            self->m_hasNewSinkInput = false;
            self->m_lastSinkInputCount = newCount;
            QMetaObject::invokeMethod(self, "sinkInputAdded", Qt::QueuedConnection);
        } else {
            self->m_hasNewSinkInput = false;
            self->m_lastSinkInputCount = newCount;
        }
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
    // Filter out loopback streams created for virtual sinks
    if (si.mediaName.startsWith(QStringLiteral("soundroot_loopback_")))
        return;
    // Filter out module-generated streams (no client attached: combine-sinks,
    // loopbacks, etc.) that slipped through the name-based filters above.
    if (info->client == PA_INVALID_INDEX)
        return;

    self->m_pendingSinkInputs.append(si);
}

void PulseAudioBackend::moduleLoadCallback(pa_context * /*c*/,
                                           uint32_t idx, void *userdata)
{
    auto *data = static_cast<ModuleLoadData *>(userdata);
    auto *self = data->backend;

    if (idx == PA_INVALID_INDEX) {
        // Load failed — clean up the slave record that was written optimistically
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_combinedSlaves.remove(data->combinedName);
        }
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
