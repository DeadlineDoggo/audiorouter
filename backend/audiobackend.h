#pragma once

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QVector>

#include <pulse/pulseaudio.h>

// ── Lightweight structs exposed to the rest of the app ──────────────────────

struct PASinkInfo {
    uint32_t index = PA_INVALID_INDEX;
    QString  name;          ///< PulseAudio internal name
    QString  description;   ///< Human-readable description
};

struct PASinkInputInfo {
    uint32_t index     = PA_INVALID_INDEX;
    QString  appName;       ///< Application name (PA_PROP_APPLICATION_NAME)
    QString  mediaName;     ///< Media being played
    uint32_t sinkIndex = PA_INVALID_INDEX; ///< Currently routed sink
    QString  iconName;      ///< Freedesktop icon name of the app
    uint8_t  channels = 2;  ///< Stream channel count for volume operations
};

// ── PulseAudio backend ─────────────────────────────────────────────────────

/**
 * @brief Async wrapper around the PulseAudio threaded-mainloop API.
 *
 * Provides live lists of sinks (output devices) and sink-inputs (application
 * audio streams), and methods to move streams between sinks or create
 * combined sinks for multi-output routing.
 *
 * Works transparently on PipeWire installations that expose the PulseAudio
 * compatibility layer.
 */
class PulseAudioBackend : public QObject {
    Q_OBJECT

public:
    explicit PulseAudioBackend(QObject *parent = nullptr);
    ~PulseAudioBackend() override;

    void initialize();
    void shutdown();

    bool isConnected() const { return m_connected; }

    // ── Queries (thread-safe, return copies) ────────────────────────────
    QVector<PASinkInfo>      availableSinks()      const;
    QVector<PASinkInputInfo> availableSinkInputs() const;

    // ── Actions ─────────────────────────────────────────────────────────
    void moveSinkInput(uint32_t sinkInputIndex, uint32_t sinkIndex);
    void setSinkInputVolume(uint32_t sinkInputIndex,
                            int volumePercent,
                            uint8_t channels = 2);

    /// Load a module-combine-sink that merges several sinks into one.
    void createCombinedSink(const QString &name, const QStringList &sinkNames);
    void destroyCombinedSink(const QString &name);

    /// High-level: route an application (by name) to one or more outputs.
    void applyRoute(const QString &appName,
                    const QStringList &outputSinkNames,
                    int volumePercent = 100);
    void removeRoute(const QString &appName);

signals:
    void sinksChanged();
    void sinkInputsChanged();
    void serverConnected();
    void serverDisconnected();
    void errorOccurred(const QString &message);

private:
    // ── Internal refresh helpers (must be called with PA lock held) ──────
    void refreshSinksLocked();
    void refreshSinkInputsLocked();

    // ── Public-facing refresh (acquires PA lock) ────────────────────────
    void refreshSinks();
    void refreshSinkInputs();

    /// Move collected sink-inputs to a combined sink, with retry logic.
    void moveSinkInputsToCombined(const QString &combinedName,
                                  const QVector<QPair<uint32_t, uint8_t>> &sinkInputs,
                                  int volumePercent,
                                  int retryCount);

    // ── PulseAudio static callbacks ─────────────────────────────────────
    static void contextStateCallback(pa_context *c, void *userdata);
    static void subscribeCallback(pa_context *c,
                                  pa_subscription_event_type_t t,
                                  uint32_t idx, void *userdata);
    static void sinkInfoCallback(pa_context *c,
                                 const pa_sink_info *info,
                                 int eol, void *userdata);
    static void sinkInputInfoCallback(pa_context *c,
                                      const pa_sink_input_info *info,
                                      int eol, void *userdata);
    static void moduleLoadCallback(pa_context *c, uint32_t idx, void *userdata);
    static void successCallback(pa_context *c, int success, void *userdata);

    // ── Members ─────────────────────────────────────────────────────────
    pa_threaded_mainloop *m_mainloop = nullptr;
    pa_context           *m_context  = nullptr;

    mutable QMutex              m_mutex;
    QVector<PASinkInfo>         m_sinks;
    QVector<PASinkInputInfo>    m_sinkInputs;
    QMap<QString, uint32_t>     m_combinedModules;   // name → module index

    // Pending buffers for atomic refresh (only accessed from PA thread)
    QVector<PASinkInfo>         m_pendingSinks;
    QVector<PASinkInputInfo>    m_pendingSinkInputs;

    bool m_connected = false;
};
