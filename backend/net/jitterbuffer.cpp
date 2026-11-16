#include "jitterbuffer.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr int kLateResync = 8;   // this many late packets in a row → sender restarted
}

JitterBuffer::JitterBuffer(int channels, int sampleRate, int targetMs, int capacityMs)
    : m_channels(std::max(1, channels))
    , m_capacity(static_cast<qint64>(sampleRate) * capacityMs / 1000)
    , m_target(static_cast<qint64>(sampleRate) * targetMs / 1000)
    , m_slack(static_cast<qint64>(sampleRate) * std::max(15, targetMs / 3) / 1000)
    , m_idleFrames(sampleRate / 2)
    , m_ring(static_cast<qsizetype>(m_capacity * m_channels), 0)
{
}

int16_t *JitterBuffer::slot(qint64 timestamp)
{
    const qint64 pos = ((timestamp % m_capacity) + m_capacity) % m_capacity;
    return m_ring.data() + pos * m_channels;
}

void JitterBuffer::clearRange(qint64 from, qint64 to)
{
    if (to - from >= m_capacity) {
        std::fill(m_ring.begin(), m_ring.end(), int16_t(0));
        return;
    }
    for (qint64 ts = from; ts < to;) {
        const qint64 pos = ((ts % m_capacity) + m_capacity) % m_capacity;
        const qint64 run = std::min(to - ts, m_capacity - pos);
        std::memset(slot(ts), 0, static_cast<size_t>(run * m_channels) * sizeof(int16_t));
        ts += run;
    }
}

void JitterBuffer::restart(qint64 timestamp)
{
    std::fill(m_ring.begin(), m_ring.end(), int16_t(0));
    m_started = true;
    m_buffering = true;
    m_fresh = true;
    m_readTs = timestamp;
    m_highTs = timestamp;
    m_stalled = 0;
    m_lateRun = 0;
    m_avgFill = static_cast<double>(m_target);
}

void JitterBuffer::reset()
{
    QMutexLocker lock(&m_mutex);
    std::fill(m_ring.begin(), m_ring.end(), int16_t(0));
    m_started = false;
    m_buffering = false;
}

void JitterBuffer::insert(qint64 timestamp, const int16_t *frames, int frameCount)
{
    if (!frames || frameCount <= 0 || frameCount > m_capacity) return;
    QMutexLocker lock(&m_mutex);

    if (!m_started) {
        restart(timestamp);
        ++m_stats.resyncs;
    } else if (m_fresh && timestamp < m_readTs && m_readTs - timestamp < m_capacity / 2) {
        // Still filling for the first time and this packet is older than the
        // one we started on (they arrived out of order): start from it.
        m_readTs = timestamp;
    } else if (timestamp + frameCount <= m_readTs) {
        // Its slot has been played already
        ++m_stats.late;
        if (++m_lateRun < kLateResync) return;
        // Everything is "late": the sender started a new timeline
        restart(timestamp);
        ++m_stats.resyncs;
    } else if (timestamp + frameCount > m_readTs + m_capacity) {
        // Far in the future: the sender resumed after a long pause
        restart(timestamp);
        ++m_stats.resyncs;
    }
    m_lateRun = 0;

    // Drop the part that was played already
    qint64 ts = timestamp;
    const int16_t *src = frames;
    qint64 count = frameCount;
    if (ts < m_readTs) {
        const qint64 skip = m_readTs - ts;
        ts += skip;
        src += skip * m_channels;
        count -= skip;
    }

    for (qint64 done = 0; done < count;) {
        const qint64 pos = (((ts + done) % m_capacity) + m_capacity) % m_capacity;
        const qint64 run = std::min(count - done, m_capacity - pos);
        std::memcpy(slot(ts + done), src + done * m_channels,
                    static_cast<size_t>(run * m_channels) * sizeof(int16_t));
        done += run;
    }
    m_highTs = std::max(m_highTs, ts + count);
    m_stalled = 0;
    ++m_stats.inserted;
}

void JitterBuffer::read(int16_t *out, int frameCount)
{
    if (!out || frameCount <= 0) return;
    const size_t bytes = static_cast<size_t>(frameCount) * static_cast<size_t>(m_channels)
                         * sizeof(int16_t);
    QMutexLocker lock(&m_mutex);

    if (!m_started) {
        std::memset(out, 0, bytes);
        return;
    }

    qint64 fill = m_highTs - m_readTs;

    if (!m_buffering && fill <= 0) {
        ++m_stats.underruns;
        m_buffering = true;
        m_readTs = m_highTs;   // nothing beyond this was received
        fill = 0;
    }
    if (m_buffering) {
        if (fill < m_target) {
            // Wait for the queue to build up, do not consume
            std::memset(out, 0, bytes);
            m_stalled += frameCount;
            if (m_stalled >= m_idleFrames && fill <= 0)
                m_started = false;   // the sender stopped; realign on its next packet
            return;
        }
        m_buffering = false;
        m_fresh = false;
        // Start the average at the target: only a level that stays off for
        // a while is drift; a one-off burst is not.
        m_avgFill = static_cast<double>(m_target);
    }

    // Too much queued means too much delay: jump to the target
    if (fill > 2 * m_target + 2 * m_slack) {
        const qint64 newRead = m_highTs - m_target;
        clearRange(m_readTs, newRead);
        m_readTs = newRead;
        fill = m_target;
        m_avgFill = static_cast<double>(m_target);
        ++m_stats.skips;
    }

    // Clock drift: nudge by one frame per call while the average level is off
    m_avgFill += (static_cast<double>(fill) - m_avgFill) / 64.0;
    int slip = 0;
    if (m_avgFill > static_cast<double>(m_target + m_slack) && fill > frameCount + 1)
        slip = 1;            // too full: drop a frame
    else if (m_avgFill < static_cast<double>(m_target - m_slack) && frameCount > 1)
        slip = -1;           // too empty: repeat a frame

    if (slip > 0) {
        clearRange(m_readTs, m_readTs + 1);
        ++m_readTs;
    }
    const qint64 take = slip < 0 ? frameCount - 1 : frameCount;

    for (qint64 done = 0; done < take;) {
        const qint64 pos = (((m_readTs + done) % m_capacity) + m_capacity) % m_capacity;
        const qint64 run = std::min(take - done, m_capacity - pos);
        std::memcpy(out + done * m_channels, slot(m_readTs + done),
                    static_cast<size_t>(run * m_channels) * sizeof(int16_t));
        done += run;
    }
    clearRange(m_readTs, m_readTs + take);
    m_readTs += take;

    if (slip < 0) {
        std::memcpy(out + take * m_channels, out + (take - 1) * m_channels,
                    static_cast<size_t>(m_channels) * sizeof(int16_t));
    }
    m_stats.slips += slip;
}

qint64 JitterBuffer::queuedFrames() const
{
    QMutexLocker lock(&m_mutex);
    return m_started ? std::max<qint64>(0, m_highTs - m_readTs) : 0;
}

bool JitterBuffer::isPlaying() const
{
    QMutexLocker lock(&m_mutex);
    return m_started && !m_buffering;
}

JitterBuffer::Stats JitterBuffer::stats() const
{
    QMutexLocker lock(&m_mutex);
    return m_stats;
}
