#pragma once

#include <QHash>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <QVector>

#include <atomic>
#include <functional>

#include <pulse/pulseaudio.h>

#include "audiotypes.h"

/**
 * @brief Async wrapper around the PulseAudio threaded-mainloop API.
 *
 * Provides live lists of sinks (output devices), sink-inputs (applications
 * that play), sources (microphones) and source-outputs (applications that
 * record), and the primitives the routing engine builds on: moving a stream,
 * combine-sinks, virtual null-sinks with their microphone and loopbacks, and
 * the record / playback streams of network nodes.
 *
 * Works on PipeWire through its PulseAudio compatibility layer.
 *
 * If the sound server goes away (restart, crash, update) the backend drops
 * everything it knew, reconnects, and emits serverConnected() again so the
 * engine can rebuild its state.
 *
 * Locking: the PulseAudio mainloop lock is always taken BEFORE m_mutex,
 * never the other way round (the PA callbacks run with the mainloop lock
 * held and then take m_mutex).
 */
class PulseAudioBackend : public AudioBackend {
    Q_OBJECT

public:
    explicit PulseAudioBackend(QObject *parent = nullptr);
    ~PulseAudioBackend() override;

    /// Where network node audio goes to / comes from.  Set before initialize().
    void setAudioTap(AudioTap *tap) { m_tap = tap; }

    void initialize();
    void shutdown();

    // ── AudioBackend ────────────────────────────────────────────────────
    bool isConnected() const override { return m_connected; }
    QVector<PASinkInfo>      availableSinks() const override;
    QVector<PASinkInputInfo> availableSinkInputs() const override;
    QString                  defaultSinkName() const override;
    QVector<PASourceInfo>       availableSources() const override;
    QVector<PASourceOutputInfo> availableSourceOutputs() const override;
    QString                     defaultSourceName() const override;

    void moveSinkInput(uint32_t sinkInputIndex, uint32_t sinkIndex) override;
    void moveSourceOutput(uint32_t sourceOutputIndex, uint32_t sourceIndex) override;

    void createCombinedSink(const QString &name, const QStringList &sinkNames) override;
    void destroyCombinedSink(const QString &name) override;

    void createVirtualSink(const QString &paName, const QString &displayName) override;
    void destroyVirtualSink(const QString &paName) override;
    void setVirtualSinkLoopbacks(const QString &paName, const QStringList &targetSinks) override;
    void setVirtualSinkInputs(const QString &paName, const QStringList &sourceNames) override;

    void openNetworkCapture(const QString &paName) override;
    void closeNetworkCapture(const QString &paName) override;
    void openNetworkPlayback(const QString &paName, const QString &sinkName) override;
    void closeNetworkPlayback(const QString &paName) override;

    // ── Volume controls ─────────────────────────────────────────────────
    int  defaultSinkVolume() const;
    void setSinkInputVolume(uint32_t sinkInputIndex, int volumePercent);
    /// Set the volume of the default output sink (0–150%).
    void setDefaultSinkVolume(int percent);
    /// Set the volume of a specific sink by its PA name (0–150%).
    void setSinkVolumeByName(const QString &sinkName, int percent);
    void toggleSinkMuteByName(const QString &sinkName);

signals:
    /// Emitted whenever the default sink’s volume changes.
    void defaultSinkVolumeChanged(int volumePercent);

private:
    struct NetStream;

    // ── Internal helpers (PA lock held) ─────────────────────────────────
    void refreshSinksLocked();
    void refreshSinkInputsLocked();
    void refreshSourcesLocked();
    void refreshSourceOutputsLocked();
    void unloadModuleLocked(uint32_t moduleIndex);
    /// Load every wanted loopback of paName that is neither loaded nor loading.
    void loadMissingLoopbacksLocked(const QString &paName);
    /// Load the node's microphone and every wanted input loopback of paName
    /// that is neither loaded nor loading.
    void loadVirtualMicLocked(const QString &paName);
    void loadMissingInputsLocked(const QString &paName);
    /// Start a module load whose result virtualModuleLoadCallback files
    /// under @p key; false if it could not be started.
    bool loadVirtualModuleLocked(const char *module, const QString &args,
                                 const QString &key, int kind);
    /// Start a module-combine-sink load.  The caller has already recorded it
    /// in m_pendingCombined.
    void loadCombinedLocked(const QString &name, const QStringList &sinkNames);
    void closeStreamLocked(QHash<QString, NetStream *> &streams, const QString &paName);
    /// Remember heap data handed to a PA callback, so it can be freed if the
    /// connection dies before the callback runs.
    void trackUserdata(void *data, std::function<void()> deleter);
    void untrackUserdata(void *data);

    void setSinkVolume(const QString &sinkName, int percent);
    /// Stop the mainloop and free every PA object.  Not under the PA lock.
    void teardown();
    /// Try to connect again after a (growing) pause.
    void scheduleReconnect();
    Q_INVOKABLE void handleServerLost(quintptr deadContext);
    Q_INVOKABLE void handleStreamEnded(quintptr stream);

    // ── PulseAudio static callbacks (PA thread, mainloop lock held) ──────
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
    static void sourceInfoCallback(pa_context *c,
                                   const pa_source_info *info,
                                   int eol, void *userdata);
    static void sourceOutputInfoCallback(pa_context *c,
                                         const pa_source_output_info *info,
                                         int eol, void *userdata);
    static void moduleLoadCallback(pa_context *c, uint32_t idx, void *userdata);
    static void virtualModuleLoadCallback(pa_context *c, uint32_t idx, void *userdata);
    static void successCallback(pa_context *c, int success, void *userdata);
    static void unloadModuleCallback(pa_context *c, int success, void *userdata);
    static void streamStateCallback(pa_stream *s, void *userdata);
    static void streamReadCallback(pa_stream *s, size_t nbytes, void *userdata);
    static void streamWriteCallback(pa_stream *s, size_t nbytes, void *userdata);

    // ── Members ─────────────────────────────────────────────────────────
    pa_threaded_mainloop *m_mainloop = nullptr;
    pa_context           *m_context  = nullptr;
    AudioTap             *m_tap      = nullptr;

    mutable QMutex              m_mutex;
    QVector<PASinkInfo>         m_sinks;
    QVector<PASinkInputInfo>    m_sinkInputs;
    QVector<PASourceInfo>       m_sources;
    QVector<PASourceOutputInfo> m_sourceOutputs;
    QMap<QString, uint32_t>     m_combinedModules;   // name → module index
    QMap<QString, QStringList>  m_combinedSlaves;    // name → DESIRED slave sink names (absent = not wanted)
    QMap<QString, QStringList>  m_pendingCombined;   // name → slaves of the load in flight (at most one per name)
    QString  m_defaultSinkName;            // guarded by m_mutex
    QString  m_defaultSourceName;          // guarded by m_mutex
    int      m_defaultSinkVolume = 100;    // guarded by m_mutex

    // Everything below is only touched with the PA mainloop lock held
    QMap<QString, uint32_t>     m_virtualSinkModules; // paName → module index
    QMap<QString, uint32_t>     m_loopbackModules;   // "paName|sinkName" → module index
    QSet<QString>               m_wantedVirtualSinks; // desired paNames
    QSet<QString>               m_wantedLoopbacks;    // desired "paName|sinkName"
    QMap<QString, QString>      m_wantedMics;         // paName → description of its microphone
    QMap<QString, uint32_t>     m_micModules;         // paName → module index
    QSet<QString>               m_wantedInputs;       // desired "paName|sourceName"
    QMap<QString, uint32_t>     m_inputModules;       // "paName|sourceName" → module index
    QSet<QString>               m_pendingVirtualLoads; // keys of either kind with a load in flight
    int                         m_pendingUnloads = 0; // unload ops issued, not yet confirmed
    QHash<QString, NetStream *> m_captureStreams;     // paName → record stream
    QHash<QString, NetStream *> m_playbackStreams;    // paName → playback stream
    QHash<QString, qint64>      m_streamFailures;     // "c|paName" / "p|paName" → time of last failure
    QHash<void *, std::function<void()>> m_userdata;  // callback data not yet delivered

    // Pending buffers for atomic refresh
    QVector<PASinkInfo>         m_pendingSinks;
    QVector<PASinkInputInfo>    m_pendingSinkInputs;
    QVector<PASourceInfo>       m_pendingSources;
    QVector<PASourceOutputInfo> m_pendingSourceOutputs;

    // Generation counters – bumped each time a new enumeration starts.  Each
    // request carries its generation in its userdata; results from an older,
    // superseded request are discarded.
    uint64_t m_sinkInputGen     = 0;
    uint64_t m_sinkGen          = 0;
    uint64_t m_sourceGen        = 0;
    uint64_t m_sourceOutputGen  = 0;

    static constexpr int kShutdownWaitMs = 1500; // max wait for module cleanup on exit
    static constexpr int kReconnectDelayMs = 1000;
    static constexpr int kMaxReconnectDelayMs = 5000;
    static constexpr int kStreamRetryMs = 2000;  // pause after a network stream failed

    std::atomic<bool> m_connected { false };
    std::atomic<bool> m_everConnected { false };       // the current context reached READY
    std::atomic<bool> m_reportedUnreachable { false }; // warned about the outage already
    std::atomic<int>  m_reconnectDelayMs { kReconnectDelayMs };
    bool m_shuttingDown = false;
};
