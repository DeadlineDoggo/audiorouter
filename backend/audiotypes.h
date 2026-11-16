#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

// Types shared by the audio backend, the routing engine and the models.
// Nothing in here depends on libpulse, so the routing logic can be built and
// tested without a sound server.

/// Same value as PA_INVALID_INDEX.
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

/// Prefix of the null-sinks SoundRoot creates for virtual / network nodes.
inline QString virtualSinkPrefix() { return QStringLiteral("soundroot_virt_"); }
/// Prefix of the combine-sinks SoundRoot creates for multi-output routes.
inline QString combineSinkPrefix() { return QStringLiteral("audiorouter_"); }
/// media.name prefix of the playback stream that plays a network node's
/// received audio; followed by the node's PA name.
inline QString netPlaybackPrefix() { return QStringLiteral("soundroot_netrx_"); }
/// media.name prefix of the record stream that captures a network node.
inline QString netCapturePrefix() { return QStringLiteral("soundroot_netcap_"); }

/// True for sinks SoundRoot itself created (never shown as output devices).
inline bool isInternalSinkName(const QString &name)
{
    return name.startsWith(virtualSinkPrefix()) || name.startsWith(combineSinkPrefix());
}

struct PASinkInfo {
    uint32_t index = kInvalidIndex;
    QString  name;          ///< PulseAudio internal name
    QString  description;   ///< Human-readable description
    int      volumePercent = 100; ///< Current output volume (0-150)
    uint8_t  channels = 2;       ///< Channel count for volume operations
    bool     isMuted = false;     ///< Whether the sink is muted
};

struct PASinkInputInfo {
    uint32_t index     = kInvalidIndex;
    QString  appName;       ///< Application name (PA_PROP_APPLICATION_NAME)
    QString  mediaName;     ///< Media being played
    uint32_t sinkIndex = kInvalidIndex; ///< Currently routed sink
    QString  iconName;      ///< Freedesktop icon name of the app
    uint8_t  channels = 2;  ///< Stream channel count for volume operations
    int      volumePercent = 100; ///< Current stream volume (0-150%)
    /// Non-empty for the playback stream of one of our network nodes: the
    /// node's PA name.  Such streams are routed by the engine but never shown
    /// in the application list.
    QString  netNode;
};

/**
 * @brief What the routing engine needs from a sound server.
 *
 * PulseAudioBackend is the real implementation; the tests use a fake.  Every
 * "create/open/set" call is idempotent: it records the desired state and only
 * does the work needed to reach it, so the engine can call them again on
 * every reconcile pass.
 */
class AudioBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual bool isConnected() const = 0;

    // ── Queries (return copies) ─────────────────────────────────────────
    /// All sinks, including SoundRoot's own virtual and combine sinks.
    virtual QVector<PASinkInfo>      availableSinks() const = 0;
    /// All routable streams, including network-node playback streams.
    virtual QVector<PASinkInputInfo> availableSinkInputs() const = 0;
    virtual QString                  defaultSinkName() const = 0;

    // ── Stream moves ────────────────────────────────────────────────────
    virtual void moveSinkInput(uint32_t sinkInputIndex, uint32_t sinkIndex) = 0;

    // ── Combine sinks (one stream → several outputs) ────────────────────
    virtual void createCombinedSink(const QString &name, const QStringList &sinkNames) = 0;
    virtual void destroyCombinedSink(const QString &name) = 0;

    // ── Virtual null-sinks and their loopbacks ──────────────────────────
    virtual void createVirtualSink(const QString &paName, const QString &displayName) = 0;
    virtual void destroyVirtualSink(const QString &paName) = 0;
    virtual void setVirtualSinkLoopbacks(const QString &paName, const QStringList &targetSinks) = 0;

    // ── Network node audio I/O ──────────────────────────────────────────
    /// Record what is played into the node's null-sink (its monitor).
    virtual void openNetworkCapture(const QString &paName) = 0;
    virtual void closeNetworkCapture(const QString &paName) = 0;
    /// Play the node's received audio.  @p sinkName is where the stream
    /// starts; the engine moves it afterwards like any other stream.
    virtual void openNetworkPlayback(const QString &paName, const QString &sinkName) = 0;
    virtual void closeNetworkPlayback(const QString &paName) = 0;

signals:
    void sinksChanged();
    void sinkInputsChanged();
    void serverConnected();
    void serverDisconnected();
    void errorOccurred(const QString &message);
};

/**
 * @brief Receives a network node's captured audio and supplies the audio to
 *        play.  Implemented by NetEngine; called on the backend's audio
 *        thread, so both methods must be thread-safe and quick.
 *
 * Audio is always interleaved signed 16-bit little-endian, 48 kHz, stereo.
 */
class AudioTap {
public:
    virtual ~AudioTap() = default;
    virtual void capturedAudio(const QString &paName, const void *data, size_t bytes) = 0;
    /// Fill @p data completely (silence where nothing was received).
    virtual void playbackAudio(const QString &paName, void *data, size_t bytes) = 0;
};
