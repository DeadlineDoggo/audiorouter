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
    int      volumePercent = 100; ///< Current output volume (0-150)
    uint8_t  channels = 2;       ///< Channel count for volume operations
    bool     isMuted = false;     ///< Whether the sink is muted
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
    QVector<PASinkInputInfo> availableSinkInputs() const;    int                      defaultSinkVolume()   const;
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

    /// Move all streams from appName back to the default sink.
    void restoreRoute(const QString &appName);

    /// Set the volume of the default output sink (0–150%).
    void setDefaultSinkVolume(int percent);

    /// Set the volume of a specific sink by its PA name (0–150%).
    void setSinkVolumeByName(const QString &sinkName, int percent);

    /// Toggle mute state of a specific sink by name.
    void toggleSinkMuteByName(const QString &sinkName);

    /// Move a specific sink-input (by PA index) to a named sink.
    void moveInputToSink(uint32_t sinkInputIndex, const QString &sinkName);

    /// Route a specific sink-input to one or more sinks.
    /// Creates a combine-sink when multiple sinks are specified.
    void routeStreamToSinks(uint32_t sinkInputIndex, const QStringList &sinkNames);

    /// Destroy all per-stream combine sinks (audiorouter_stream_*).
    void cleanupStreamCombineSinks();

signals:
    void sinksChanged();
    void sinkInputsChanged();
    /// Emitted only when a truly new stream appears (not on move/volume change).
    void sinkInputAdded();
    void serverConnected();
    void serverDisconnected();
    /// Emitted whenever the default sink’s volume changes.
    void defaultSinkVolumeChanged(int volumePercent);
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
    static void serverInfoCallback(pa_context *c,
                                   const pa_server_info *info,
                                   void *userdata);
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
    QMap<QString, QStringList>   m_combinedSlaves;    // name → slave sink names

    // Pending buffers for atomic refresh (only accessed from PA thread)
    QVector<PASinkInfo>         m_pendingSinks;
    QVector<PASinkInputInfo>    m_pendingSinkInputs;

    // Set from PA thread (subscribeCallback) when a NEW sink-input event arrives.
    // Read and cleared in sinkInputInfoCallback(eol). Never needs a mutex since
    // both callers run on the PA mainloop thread.
    bool     m_hasNewSinkInput  = false;
    int      m_lastSinkInputCount = 0;  // post-filter count for detecting real new streams

    // Generation counters – bumped in subscribeCallback each time a new
    // enumeration starts.  The sinkInputInfoCallback / sinkInfoCallback
    // eol handler ignores results whose generation doesn't match.
    uint64_t m_sinkInputGen     = 0;
    uint64_t m_sinkGen          = 0;

    QString  m_defaultSinkName;            // guarded by m_mutex
    int      m_defaultSinkVolume = 100;    // guarded by m_mutex

    bool m_connected = false;
};
