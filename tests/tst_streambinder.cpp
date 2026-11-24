#include <QTest>

#include "routing/streambinder.h"

using StreamBinder::Bindings;

static AudioRoute route(const QString &app, int ordinal, const QString &hint = {})
{
    AudioRoute r;
    r.sourceAppName = ordinal < 0 ? app : AudioRoute::makeKey(app, ordinal);
    r.mediaHint = hint;
    r.outputSinkNames = { QStringLiteral("out") };
    return r;
}

static PASinkInputInfo stream(uint32_t index, const QString &app, const QString &media = {})
{
    PASinkInputInfo s;
    s.index = index;
    s.appName = app;
    s.mediaName = media;
    return s;
}

static const QString F = QStringLiteral("Firefox");
static QString key(int n) { return AudioRoute::makeKey(F, n); }

class TestStreamBinder : public QObject {
    Q_OBJECT

private slots:
    void pairsInOrder()
    {
        const QVector<AudioRoute> routes { route(F, 1), route(F, 0) };
        // Streams listed out of order: the oldest (lowest index) is first
        const QVector<PASinkInputInfo> streams { stream(12, F), stream(10, F), stream(11, QStringLiteral("mpv")) };
        const Bindings b = StreamBinder::bind(routes, streams, {});
        QCOMPARE(b.size(), 2);
        QCOMPARE(b.value(10), key(0));
        QCOMPARE(b.value(12), key(1));
    }

    void mediaNameWinsOverOrder()
    {
        const QVector<AudioRoute> routes { route(F, 0, QStringLiteral("Music")), route(F, 1, QStringLiteral("Call")) };
        const QVector<PASinkInputInfo> streams { stream(10, F, QStringLiteral("Call")), stream(11, F, QStringLiteral("Music")) };
        const Bindings b = StreamBinder::bind(routes, streams, {});
        QCOMPARE(b.value(10), key(1));
        QCOMPARE(b.value(11), key(0));
    }

    void unknownMediaNameFallsBackToOrder()
    {
        const QVector<AudioRoute> routes { route(F, 0, QStringLiteral("Old title")) };
        const Bindings b = StreamBinder::bind(routes, { stream(10, F, QStringLiteral("New title")) }, {});
        QCOMPARE(b.value(10), key(0));
    }

    // The bug this replaces: when the first of two streams ended, the second
    // one became "stream 0" and jumped to the other output.
    void survivingStreamKeepsItsRoute()
    {
        const QVector<AudioRoute> routes { route(F, 0), route(F, 1) };
        Bindings b = StreamBinder::bind(routes, { stream(10, F), stream(11, F) }, {});
        QCOMPARE(b.value(11), key(1));

        b = StreamBinder::bind(routes, { stream(11, F) }, b);   // stream 10 ended
        QCOMPARE(b.size(), 1);
        QCOMPARE(b.value(11), key(1));

        b = StreamBinder::bind(routes, { stream(11, F), stream(15, F) }, b);   // a new one starts
        QCOMPARE(b.value(11), key(1));
        QCOMPARE(b.value(15), key(0));
    }

    void moreStreamsThanRoutes()
    {
        const Bindings b = StreamBinder::bind({ route(F, 0) }, { stream(10, F), stream(11, F) }, {});
        QCOMPARE(b.size(), 1);
        QVERIFY(b.contains(10));
    }

    void legacyRouteTakesAllRemainingStreams()
    {
        const QVector<AudioRoute> routes { route(F, -1), route(F, 0) };
        const Bindings b = StreamBinder::bind(routes, { stream(10, F), stream(11, F), stream(12, F) }, {});
        QCOMPARE(b.value(10), key(0));
        QCOMPARE(b.value(11), F);
        QCOMPARE(b.value(12), F);
    }

    void staleBindingsAreDropped()
    {
        Bindings previous;
        previous.insert(10, key(0));                       // route was deleted
        previous.insert(11, AudioRoute::makeKey(QStringLiteral("mpv"), 0));   // index reused by another app
        const QVector<AudioRoute> routes { route(QStringLiteral("mpv"), 0) };
        const Bindings b = StreamBinder::bind(routes, { stream(10, F), stream(11, F), stream(12, QStringLiteral("mpv")) }, previous);
        QCOMPARE(b.size(), 1);
        QCOMPARE(b.value(12), AudioRoute::makeKey(QStringLiteral("mpv"), 0));
    }

    void networkPlaybackStreamsAreNotApplications()
    {
        PASinkInputInfo ours = stream(10, QStringLiteral("SoundRoot"));
        ours.netNode = QStringLiteral("soundroot_virt_x");
        const Bindings b = StreamBinder::bind({ route(QStringLiteral("SoundRoot"), 0) }, { ours }, {});
        QVERIFY(b.isEmpty());
    }

    void recordersAreBoundLikePlayers()
    {
        CaptureRoute a, b;
        a.key = AudioRoute::makeKey(QStringLiteral("Teams"), 0);
        b.key = AudioRoute::makeKey(QStringLiteral("Teams"), 1);
        b.mediaHint = QStringLiteral("Screen share");
        auto rec = [](uint32_t index, const QString &media) {
            PASourceOutputInfo r;
            r.index = index;
            r.appName = QStringLiteral("Teams");
            r.mediaName = media;
            return r;
        };
        const QVector<PASourceOutputInfo> streams { rec(5, QStringLiteral("Screen share")), rec(7, QStringLiteral("Call")) };
        const Bindings bound = StreamBinder::bind(QVector<CaptureRoute>{ a, b }, streams, {});
        QCOMPARE(bound.value(5), b.key);   // by media name
        QCOMPARE(bound.value(7), a.key);
        QCOMPARE(StreamBinder::freeOrdinal(QVector<CaptureRoute>{ a, b }, QStringLiteral("Teams")), 2);
    }

    void freeOrdinal()
    {
        QCOMPARE(StreamBinder::freeOrdinal(QVector<AudioRoute>{}, F), 0);
        QCOMPARE(StreamBinder::freeOrdinal({ route(F, 0), route(F, 2), route(QStringLiteral("mpv"), 1) }, F), 1);
        QCOMPARE(StreamBinder::freeOrdinal({ route(F, -1) }, F), 0);
    }
};

QTEST_GUILESS_MAIN(TestStreamBinder)
#include "tst_streambinder.moc"
