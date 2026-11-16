#pragma once

#include <QMutex>
#include <QVector>

#include <cstdint>

/**
 * @brief Reorders and paces the audio of one remote sender.
 *
 * Packets arrive from the network irregularly, out of order or not at all;
 * the sound card wants samples at a steady rate.  The buffer keeps about
 * `targetMs` of audio queued so short network hiccups are not heard:
 *
 *  - packets are placed by their timestamp, so order does not matter and a
 *    lost packet is simply a gap of silence;
 *  - when it runs dry it stops and refills to the target before continuing
 *    (also how a sender that pauses during silence is picked up again);
 *  - when far too much is queued (a burst after a stall) it skips ahead;
 *  - the two devices' sound cards never run at exactly the same speed, so
 *    the level slowly drifts; single frames are dropped or repeated to hold
 *    it near the target.
 *
 * Samples are interleaved signed 16-bit.  insert() and read() may be called
 * from different threads.
 */
class JitterBuffer {
public:
    struct Stats {
        quint64 inserted = 0;   ///< packets accepted
        quint64 late = 0;       ///< packets that arrived after their slot was played
        quint64 underruns = 0;  ///< times the buffer ran dry while playing
        quint64 skips = 0;      ///< times it jumped ahead to cut latency
        quint64 resyncs = 0;    ///< times it restarted on a new timeline
        qint64  slips = 0;      ///< frames dropped (+) or repeated (-) against drift
    };

    explicit JitterBuffer(int channels = 2, int sampleRate = 48000,
                          int targetMs = 60, int capacityMs = 1000);

    /// @p timestamp is the sender's frame counter of the first frame.
    void insert(qint64 timestamp, const int16_t *frames, int frameCount);
    /// Always fills @p out completely; silence where there is no audio.
    void read(int16_t *out, int frameCount);

    /// Frames currently queued ahead of the read position.
    qint64 queuedFrames() const;
    bool   isPlaying() const;
    Stats  stats() const;
    void   reset();

private:
    void restart(qint64 timestamp);
    void clearRange(qint64 from, qint64 to);
    int16_t *slot(qint64 timestamp);

    mutable QMutex   m_mutex;
    const int        m_channels;
    const qint64     m_capacity;      // frames
    const qint64     m_target;        // frames
    const qint64     m_slack;         // frames around the target that are fine
    const qint64     m_idleFrames;    // stalled this long → stream ended
    QVector<int16_t> m_ring;

    bool   m_started = false;
    bool   m_buffering = false;
    bool   m_fresh = false;  // nothing played since the last restart
    qint64 m_readTs = 0;     // next frame to play
    qint64 m_highTs = 0;     // end of the newest audio received
    qint64 m_stalled = 0;    // frames of silence output while waiting
    int    m_lateRun = 0;    // consecutive late packets
    double m_avgFill = 0;
    Stats  m_stats;
};
