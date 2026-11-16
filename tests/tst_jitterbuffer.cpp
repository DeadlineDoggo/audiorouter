#include <QTest>

#include "net/jitterbuffer.h"

// 48 kHz stereo, 10 ms packets, 60 ms target - as used on the network
constexpr int kPacket = 480;
constexpr int kTarget = 2880;

/// Sample value of frame @p ts: never 0, so silence is recognisable.
static int16_t ramp(qint64 ts) { return static_cast<int16_t>(ts % 20000 + 1); }

static QVector<int16_t> packet(qint64 ts, int frames = kPacket)
{
    QVector<int16_t> p(frames * 2);
    for (int i = 0; i < frames; ++i) {
        p[2 * i] = ramp(ts + i);
        p[2 * i + 1] = static_cast<int16_t>(-ramp(ts + i));
    }
    return p;
}

static void send(JitterBuffer &jb, qint64 ts, int frames = kPacket)
{
    const auto p = packet(ts, frames);
    jb.insert(ts, p.constData(), frames);
}

/// Left channel of the next @p frames frames.
static QVector<int16_t> pull(JitterBuffer &jb, int frames = kPacket)
{
    QVector<int16_t> out(frames * 2, 12345);
    jb.read(out.data(), frames);
    QVector<int16_t> left(frames);
    for (int i = 0; i < frames; ++i) {
        left[i] = out[2 * i];
        Q_ASSERT(out[2 * i + 1] == -out[2 * i]);   // channels stay paired
    }
    return left;
}

static bool isSilence(const QVector<int16_t> &v)
{
    return std::all_of(v.cbegin(), v.cend(), [](int16_t s) { return s == 0; });
}

class TestJitterBuffer : public QObject {
    Q_OBJECT

private slots:
    void silentUntilTheTargetIsQueued()
    {
        JitterBuffer jb;
        QVERIFY(isSilence(pull(jb)));           // nothing received yet
        for (int i = 0; i < 5; ++i) {           // 50 ms: below the 60 ms target
            send(jb, 1000 + i * kPacket);
            QVERIFY(isSilence(pull(jb)));
        }
        QVERIFY(!jb.isPlaying());
        send(jb, 1000 + 5 * kPacket);           // 60 ms queued
        const auto first = pull(jb);
        QVERIFY(jb.isPlaying());
        QCOMPARE(first[0], ramp(1000));         // starts at the very first frame: nothing lost
        QCOMPARE(first[kPacket - 1], ramp(1000 + kPacket - 1));
    }

    void steadyStreamPlaysBackExactly()
    {
        JitterBuffer jb;
        qint64 sendTs = 0, playTs = 0;
        for (int i = 0; i < 6; ++i, sendTs += kPacket) send(jb, sendTs);
        for (int i = 0; i < 500; ++i) {         // 5 seconds
            send(jb, sendTs); sendTs += kPacket;
            const auto out = pull(jb);
            for (int f = 0; f < kPacket; ++f, ++playTs)
                QCOMPARE(out[f], ramp(playTs));
        }
        const auto st = jb.stats();
        QCOMPARE(st.underruns, 0);
        QCOMPARE(st.late, 0);
        QCOMPARE(st.skips, 0);
        QCOMPARE(st.slips, 0);
    }

    void reorderedPacketsAreFine()
    {
        JitterBuffer jb;
        for (int i : { 1, 0, 3, 2, 5, 4, 7, 6 })
            send(jb, i * kPacket);
        qint64 playTs = 0;
        for (int i = 0; i < 8; ++i) {
            const auto out = pull(jb);
            for (int f = 0; f < kPacket; ++f, ++playTs)
                QCOMPARE(out[f], ramp(playTs));
        }
    }

    void aLostPacketIsAGapOfSilence()
    {
        JitterBuffer jb;
        for (int i = 0; i < 10; ++i) {
            if (i != 3) send(jb, i * kPacket);
        }
        for (int i = 0; i < 10; ++i) {
            const auto out = pull(jb);
            if (i == 3) QVERIFY(isSilence(out));
            else        QCOMPARE(out[0], ramp(i * kPacket));
        }
    }

    void duplicatesAndLatePacketsAreIgnored()
    {
        JitterBuffer jb;
        for (int i = 0; i < 8; ++i) send(jb, i * kPacket);
        send(jb, 2 * kPacket);                  // duplicate: harmless
        pull(jb); pull(jb);                     // frames 0..959 played
        send(jb, 0);                            // far too late
        QCOMPARE(jb.stats().late, 1);
        QCOMPARE(pull(jb)[0], ramp(2 * kPacket));
    }

    void runningDryRefillsBeforeContinuing()
    {
        JitterBuffer jb;
        qint64 ts = 0;
        for (int i = 0; i < 6; ++i, ts += kPacket) send(jb, ts);
        for (int i = 0; i < 6; ++i) pull(jb);   // everything played
        QVERIFY(isSilence(pull(jb)));           // dry
        QCOMPARE(jb.stats().underruns, 1);

        // Packets trickle in again: not played one by one (that would
        // stutter) but only once the target is queued again
        for (int i = 0; i < 5; ++i, ts += kPacket) {
            send(jb, ts);
            QVERIFY(isSilence(pull(jb)));
        }
        send(jb, ts);
        QCOMPARE(pull(jb)[0], ramp(6 * kPacket));   // continues exactly where it stopped
    }

    void senderPausingDuringSilenceIsPickedUpAgain()
    {
        JitterBuffer jb;
        qint64 ts = 0;
        for (int i = 0; i < 6; ++i, ts += kPacket) send(jb, ts);
        for (int i = 0; i < 6; ++i) pull(jb);
        for (int i = 0; i < 100; ++i)           // one second of nothing
            QVERIFY(isSilence(pull(jb)));

        ts += 48000;                            // the sender's clock kept running meanwhile
        const qint64 resumed = ts;
        for (int i = 0; i < 6; ++i, ts += kPacket) send(jb, ts);
        QCOMPARE(pull(jb)[0], ramp(resumed));   // first frame after the pause is not lost
    }

    void senderRestartingWithANewTimeline()
    {
        JitterBuffer jb;
        for (int i = 0; i < 12; ++i) send(jb, 1000000 + i * kPacket);
        pull(jb);
        // A restarted sender counts from 0 again: every packet looks ancient.
        // After a few of them the buffer realigns instead of dropping forever.
        qint64 ts = 0;
        for (int i = 0; i < 30; ++i, ts += kPacket) send(jb, ts);
        QVERIFY(jb.stats().resyncs >= 2);
        bool sawNew = false;
        for (int i = 0; i < 40 && !sawNew; ++i) {
            const auto out = pull(jb);
            sawNew = !isSilence(out) && out[0] < 20000 && out[0] == ramp(out[0] - 1);
        }
        QVERIFY(sawNew);
    }

    void aBurstAfterAStallDoesNotAddDelayForever()
    {
        JitterBuffer jb;
        qint64 ts = 0;
        // Half a second arrives at once
        for (int i = 0; i < 50; ++i, ts += kPacket) send(jb, ts);
        pull(jb);
        QCOMPARE(jb.stats().skips, 1);
        QVERIFY(jb.queuedFrames() <= kTarget);
    }

    // The receiving sound card runs slightly faster or slower than the
    // sender's.  Uncorrected, the queue would empty or overflow in minutes.
    void clockDriftIsAbsorbed_data()
    {
        // 0.1 % = 1000 ppm, several times worse than real sound cards
        QTest::addColumn<int>("extraEverySecondRead");
        QTest::newRow("receiver 0.1% fast") << 1;
        QTest::newRow("receiver 0.1% slow") << -1;
    }

    void clockDriftIsAbsorbed()
    {
        QFETCH(int, extraEverySecondRead);
        JitterBuffer jb;
        qint64 ts = 0;
        for (int i = 0; i < 6; ++i, ts += kPacket) send(jb, ts);

        QVector<int16_t> out((kPacket + 1) * 2);
        for (int i = 0; i < 30000; ++i) {       // five minutes: 14400 frames of drift
            send(jb, ts); ts += kPacket;
            jb.read(out.data(), kPacket + (i % 2 ? extraEverySecondRead : 0));
            if (i > 3000) {                     // once it has settled
                QVERIFY2(jb.queuedFrames() > kPacket, "queue ran empty");
                QVERIFY2(jb.queuedFrames() < 2 * kTarget, "queue keeps growing");
            }
        }
        const auto st = jb.stats();
        QCOMPARE(st.underruns, 0);
        QCOMPARE(st.skips, 0);
        // Corrected in the right direction, by about the amount that drifted
        if (extraEverySecondRead > 0) QVERIFY(st.slips < -12000);
        else                          QVERIFY(st.slips > 12000);
    }

    void oddSizedReadsAndWrites()
    {
        JitterBuffer jb(2, 48000, 60, 200);     // small ring: forces wrap-around
        qint64 sendTs = 0, playTs = 0;
        const int sizes[] = { 441, 7, 1024, 133, 480, 960, 1 };
        for (int i = 0; i < 6; ++i, sendTs += kPacket) send(jb, sendTs);
        QCOMPARE(pull(jb, 1)[0], ramp(0));      // start playing
        playTs = 1;
        for (int round = 0; round < 300; ++round) {
            const int n = sizes[round % 7];
            while (sendTs - playTs < kTarget + n) { send(jb, sendTs, 100); sendTs += 100; }
            const auto out = pull(jb, n);
            for (int f = 0; f < n; ++f, ++playTs)
                QCOMPARE(out[f], ramp(playTs));
        }
    }
};

QTEST_GUILESS_MAIN(TestJitterBuffer)
#include "tst_jitterbuffer.moc"
