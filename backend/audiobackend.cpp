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
    const double linear = static_cast<double>(clampedPercent) / 100.0;

    pa_cvolume volume;
    pa_cvolume_set(&volume,
                   qMax<uint8_t>(1, channels),
                   pa_sw_volume_from_linear(linear));

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

    // Collect ALL matching sink-inputs for this application
    QVector<QPair<uint32_t, uint8_t>> matches; // (index, channels)
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &si : m_sinkInputs) {
            if (si.appName == appName)
                matches.append({si.index, si.channels});
        }
    }

    if (matches.isEmpty()) {
        qWarning() << "AudioRouter: no active stream found for" << appName;
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
        // Multiple outputs – create a combined sink, then move streams to it
        const QString combinedName =
            QStringLiteral("audiorouter_%1").arg(
                QString(appName).toLower().replace(QLatin1Char(' '), QLatin1Char('_')));

        destroyCombinedSink(combinedName);
        createCombinedSink(combinedName, outputSinkNames);

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
    const QString combinedName =
        QStringLiteral("audiorouter_%1").arg(
            QString(appName).toLower().replace(QLatin1Char(' '), QLatin1Char('_')));
    destroyCombinedSink(combinedName);
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

        // Subscribe to sink & sink-input changes
        pa_context_set_subscribe_callback(c, subscribeCallback, self);
        pa_operation_unref(pa_context_subscribe(
            c,
            static_cast<pa_subscription_mask_t>(
                PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT),
            nullptr, nullptr));

        // Initial population (lock is already held inside callback)
        self->refreshSinksLocked();
        self->refreshSinkInputsLocked();

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

    // PA lock is already held
    if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        self->m_pendingSinks.clear();
        pa_operation *op =
            pa_context_get_sink_info_list(c, sinkInfoCallback, self);
        if (op) pa_operation_unref(op);

    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
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
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_sinks = self->m_pendingSinks;
        }
        QMetaObject::invokeMethod(self, "sinksChanged", Qt::QueuedConnection);
        return;
    }
    if (!info) return;

    PASinkInfo sink;
    sink.index       = info->index;
    sink.name        = QString::fromUtf8(info->name);
    sink.description = QString::fromUtf8(info->description);

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
        QMetaObject::invokeMethod(self, "sinkInputsChanged", Qt::QueuedConnection);
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
