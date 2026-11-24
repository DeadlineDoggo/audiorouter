#include <QCoreApplication>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QTest>
#include <QThread>
#include <QUdpSocket>

#include "net/netengine.h"
#include "net/netprotocol.h"

// Two (or three) NetEngine instances in one process talk to each other over
// the loopback interface, exactly as two machines would over a LAN.

using Spec = NetworkController::NodeSpec;

constexpr int kFrames = NetProtocol::kFramesPerPacket;   // 5 ms

static NetEngine::Options options(const QString &name)
{
    NetEngine::Options o;
    o.dataPort = 0;          // any free port: several engines share this machine
    o.multicast = false;     // peers are given by address
    o.deviceName = name;
    return o;
}

static Spec spec(const QString &pa, const QString &channel, const QString &pass,
                 const QStringList &peers = {})
{
    return { pa, pa + QStringLiteral(" node"), channel, pass, peers };
}

static QString peerOf(const NetEngine &engine)
{
    return QStringLiteral("127.0.0.1:%1").arg(engine.dataPort());
}

static QVariantList peers(const NetEngine &engine, const QString &pa)
{
    return engine.nodeStatus(pa).value(QStringLiteral("peers")).toList();
}

/// One packet of a ramp that is never 0; right channel is the negated left.
static QVector<int16_t> rampPacket(qint64 firstFrame, int scale = 1)
{
    QVector<int16_t> p(kFrames * 2);
    for (int i = 0; i < kFrames; ++i) {
        const auto v = static_cast<int16_t>(((firstFrame + i) % 10000 + 1) * scale);
        p[2 * i] = v;
        p[2 * i + 1] = static_cast<int16_t>(-v);
    }
    return p;
}

static void play(NetEngine &engine, const QString &pa, const QVector<int16_t> &pcm)
{
    engine.capturedAudio(pa, pcm.constData(), static_cast<size_t>(pcm.size()) * sizeof(int16_t));
}

static QVector<int16_t> listen(NetEngine &engine, const QString &pa)
{
    QVector<int16_t> out(kFrames * 2, 999);
    engine.playbackAudio(pa, out.data(), static_cast<size_t>(out.size()) * sizeof(int16_t));
    return out;
}

/// Stream @p packets x 5 ms from @p sender while @p receiver plays, the way
/// two sound cards would: one packet in, one packet out.  Returns the left
/// channel of everything the receiver played.
static QVector<int16_t> stream(NetEngine &sender, const QString &senderPa,
                               NetEngine &receiver, const QString &receiverPa,
                               int packets, int scale = 1, qint64 startFrame = 0)
{
    QVector<int16_t> heard;
    for (int i = 0; i < packets; ++i) {
        play(sender, senderPa, rampPacket(startFrame + qint64(i) * kFrames, scale));
        QThread::msleep(3);   // let the packet cross the "network"
        const auto out = listen(receiver, receiverPa);
        for (int f = 0; f < kFrames; ++f) heard.append(out[2 * f]);
    }
    return heard;
}

class TestNetEngine : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(NetCrypto::init()); }

    void devicesOnAChannelFindEachOther()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        NetEngine b(options(QStringLiteral("laptop")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);

        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        QTRY_COMPARE(a.nodeStatus(QStringLiteral("A")).value(QStringLiteral("state")).toString(),
                     QStringLiteral("online"));
        QVERIFY(peers(a, QStringLiteral("A")).isEmpty());

        // Only B is told where A is; A learns about B from B's first packet
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
        b.setNodeListening(QStringLiteral("B"), true);

        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peers(b, QStringLiteral("B")).size(), 1, 5000);
        QCOMPARE(peers(a, QStringLiteral("A"))[0].toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("laptop"));
        QCOMPARE(peers(b, QStringLiteral("B"))[0].toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("desktop"));
        // A hears that B plays the channel, B hears that A does not
        QTRY_COMPARE_WITH_TIMEOUT(
            peers(a, QStringLiteral("A"))[0].toMap().value(QStringLiteral("listening")).toBool(), true, 3000);
        QCOMPARE(peers(b, QStringLiteral("B"))[0].toMap().value(QStringLiteral("listening")).toBool(), false);
    }

    void audioCrossesTheNetworkIntact()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        NetEngine b(options(QStringLiteral("laptop")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
        b.setNodeListening(QStringLiteral("B"), true);
        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            peers(a, QStringLiteral("A"))[0].toMap().value(QStringLiteral("listening")).toBool(), true, 3000);

        const QVector<int16_t> heard =
            stream(a, QStringLiteral("A"), b, QStringLiteral("B"), 300);   // 1.5 s

        // Silence while the jitter buffer fills, then the ramp - sample for
        // sample, from its very first frame.
        qsizetype start = 0;
        while (start < heard.size() && heard[start] == 0) ++start;
        QVERIFY2(start < heard.size(), "nothing was received");
        QVERIFY2(start >= 2880 - kFrames, "played before the 60 ms buffer had filled");
        QVERIFY2(start <= 2 * 2880, "took too long to start");
        QCOMPARE(heard[start], int16_t(1));
        qint64 frame = 0;
        for (qsizetype i = start; i < heard.size(); ++i, ++frame)
            QCOMPARE(heard[i], static_cast<int16_t>(frame % 10000 + 1));
        QVERIFY(frame > 48000);   // more than a second, without a single glitch

        // And nothing comes back the other way: B did not play anything in
        const auto atA = listen(a, QStringLiteral("A"));
        QVERIFY(std::all_of(atA.cbegin(), atA.cend(), [](int16_t s) { return s == 0; }));
    }

    void bothDirectionsAtOnce()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        NetEngine b(options(QStringLiteral("laptop")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
        a.setNodeListening(QStringLiteral("A"), true);
        b.setNodeListening(QStringLiteral("B"), true);
        QTRY_VERIFY_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size() == 1
                                 && peers(a, QStringLiteral("A"))[0].toMap().value(QStringLiteral("listening")).toBool()
                                 && peers(b, QStringLiteral("B")).size() == 1
                                 && peers(b, QStringLiteral("B"))[0].toMap().value(QStringLiteral("listening")).toBool(),
                                 6000);

        bool aHeardB = false, bHeardA = false;
        for (int i = 0; i < 120; ++i) {
            play(a, QStringLiteral("A"), rampPacket(qint64(i) * kFrames, 1));    // positive left channel
            play(b, QStringLiteral("B"), rampPacket(qint64(i) * kFrames, -1));   // negative left channel
            QThread::msleep(3);
            const auto atA = listen(a, QStringLiteral("A"));
            const auto atB = listen(b, QStringLiteral("B"));
            // Each side hears only the other, never itself
            QVERIFY(atA[0] <= 0);
            QVERIFY(atB[0] >= 0);
            aHeardB |= atA[0] < 0;
            bHeardA |= atB[0] > 0;
        }
        QVERIFY(aHeardB);
        QVERIFY(bHeardA);
    }

    // Two nodes of one SoundRoot on the same channel are a pair: each hears
    // the other.  No address is configured and multicast is off, so this
    // only works if they find each other inside the machine.
    void twoNodesOfOneDeviceArePaired()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        QTRY_VERIFY(a.dataPort() != 0);
        a.startNode(spec(QStringLiteral("L"), QStringLiteral("den"), QStringLiteral("pw")));
        a.startNode(spec(QStringLiteral("R"), QStringLiteral("den"), QStringLiteral("pw")));
        a.startNode(spec(QStringLiteral("X"), QStringLiteral("other"), QStringLiteral("pw")));
        a.setNodeListening(QStringLiteral("L"), true);
        a.setNodeListening(QStringLiteral("R"), true);
        a.setNodeListening(QStringLiteral("X"), true);
        QTRY_VERIFY_WITH_TIMEOUT(peers(a, QStringLiteral("L")).size() == 1
                                 && peers(a, QStringLiteral("L"))[0].toMap().value(QStringLiteral("listening")).toBool()
                                 && peers(a, QStringLiteral("R")).size() == 1
                                 && peers(a, QStringLiteral("R"))[0].toMap().value(QStringLiteral("listening")).toBool(),
                                 6000);
        QVERIFY(peers(a, QStringLiteral("X")).isEmpty());   // a different channel stays alone

        bool lHeardR = false, rHeardL = false;
        for (int i = 0; i < 120; ++i) {
            play(a, QStringLiteral("L"), rampPacket(qint64(i) * kFrames, 1));    // positive left channel
            play(a, QStringLiteral("R"), rampPacket(qint64(i) * kFrames, -1));   // negative left channel
            QThread::msleep(3);
            const auto atL = listen(a, QStringLiteral("L"));
            const auto atR = listen(a, QStringLiteral("R"));
            const auto atX = listen(a, QStringLiteral("X"));
            // Each node hears only its partner, never itself
            QVERIFY(atL[0] <= 0);
            QVERIFY(atR[0] >= 0);
            QCOMPARE(int(atX[0]), 0);
            lHeardR |= atL[0] < 0;
            rHeardL |= atR[0] > 0;
        }
        QVERIFY(lHeardR);
        QVERIFY(rHeardL);
    }

    void threeDevicesAreMixed()
    {
        NetEngine a(options(QStringLiteral("a")));
        NetEngine b(options(QStringLiteral("b")));
        NetEngine c(options(QStringLiteral("c")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0 && c.dataPort() != 0);
        c.startNode(spec(QStringLiteral("C"), QStringLiteral("den"), QStringLiteral("pw")));
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(c) }));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(c) }));
        c.setNodeListening(QStringLiteral("C"), true);

        QTRY_COMPARE_WITH_TIMEOUT(peers(c, QStringLiteral("C")).size(), 2, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size() >= 1
                                 && peers(b, QStringLiteral("B")).size() >= 1, 6000);
        QTest::qWait(1200);   // one beacon round: A and B learn that C listens

        // A plays a constant 1000, B a constant 2000; C must hear 3000
        QVector<int16_t> one(kFrames * 2, 1000), two(kFrames * 2, 2000);
        int best = 0;
        for (int i = 0; i < 160; ++i) {
            play(a, QStringLiteral("A"), one);
            play(b, QStringLiteral("B"), two);
            QThread::msleep(3);
            const auto out = listen(c, QStringLiteral("C"));
            best = qMax(best, int(out[0]));
            QVERIFY(out[0] == 0 || out[0] == 1000 || out[0] == 2000 || out[0] == 3000);
        }
        QCOMPARE(best, 3000);
    }

    void aDifferentPassphraseIsADifferentWorld()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        NetEngine b(options(QStringLiteral("intruder")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("the real one")));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("a guess"), { peerOf(a) }));
        a.setNodeListening(QStringLiteral("A"), true);
        b.setNodeListening(QStringLiteral("B"), true);

        QTest::qWait(2500);   // several beacon rounds
        QVERIFY(peers(a, QStringLiteral("A")).isEmpty());
        QVERIFY(peers(b, QStringLiteral("B")).isEmpty());

        const auto heard = stream(b, QStringLiteral("B"), a, QStringLiteral("A"), 60);
        QVERIFY(std::all_of(heard.cbegin(), heard.cend(), [](int16_t s) { return s == 0; }));
    }

    void forgedAndReplayedPacketsAreIgnored()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        QTRY_VERIFY(a.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        a.setNodeListening(QStringLiteral("A"), true);
        QTRY_COMPARE(a.nodeStatus(QStringLiteral("A")).value(QStringLiteral("state")).toString(),
                     QStringLiteral("online"));

        // Someone who knows the key but never answers a challenge - which is
        // all a recording of an old session can do: its audio must not play.
        const auto keys = NetCrypto::deriveKeys(QStringLiteral("den"), QStringLiteral("pw"));
        QUdpSocket attacker;
        QVERIFY(attacker.bind(QHostAddress::LocalHost, 0));
        NetProtocol::Header h;
        h.channelId = keys.channelId;
        h.senderId = 0xABCDEF;
        const QByteArray pcm(kFrames * 4, '\x55');
        for (int i = 0; i < 40; ++i) {
            h.type = NetProtocol::Audio;
            h.counter = quint64(i + 1);
            attacker.writeDatagram(
                NetProtocol::sealPacket(keys, h, NetProtocol::encodeAudio(qint64(i) * kFrames, pcm)),
                QHostAddress::LocalHost, a.dataPort());
            attacker.writeDatagram(QByteArray(100, 'z'), QHostAddress::LocalHost, a.dataPort());   // junk
        }
        QTest::qWait(300);
        for (int i = 0; i < 20; ++i) {
            const auto out = listen(a, QStringLiteral("A"));
            QVERIFY(std::all_of(out.cbegin(), out.cend(), [](int16_t s) { return s == 0; }));
        }
        QVERIFY(peers(a, QStringLiteral("A")).isEmpty());   // and it never counts as a peer

        // It was challenged, though
        bool challenged = false;
        while (attacker.hasPendingDatagrams()) {
            QByteArray d(int(attacker.pendingDatagramSize()), '\0');
            attacker.readDatagram(d.data(), d.size());
            NetProtocol::Header rh;
            challenged |= NetProtocol::decodeHeader(d, &rh) && rh.type == NetProtocol::Hello;
        }
        QVERIFY(challenged);
    }

    void nothingIsSentToADeviceThatDoesNotListen()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        NetEngine b(options(QStringLiteral("laptop")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 5000);

        // B's OUT side is not wired (it does not listen)
        const auto heard = stream(a, QStringLiteral("A"), b, QStringLiteral("B"), 80);
        QVERIFY(std::all_of(heard.cbegin(), heard.cend(), [](int16_t s) { return s == 0; }));
    }

    void leavingIsNoticedAtOnce()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        NetEngine b(options(QStringLiteral("laptop")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 5000);

        b.stopNode(QStringLiteral("B"));
        // Well inside the 5 s timeout: B said goodbye
        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 0, 1500);
        QTRY_VERIFY(b.nodeStatus(QStringLiteral("B")).isEmpty());

        // ...and can come straight back
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 5000);
    }

    void aCrashedPeerTimesOut()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        QTRY_VERIFY(a.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        {
            NetEngine b(options(QStringLiteral("laptop")));
            QTRY_VERIFY(b.dataPort() != 0);
            b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
            QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 5000);
            // Make it vanish without a goodbye: close A's ears for B's Bye
            a.stopNode(QStringLiteral("A"));
        }
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        QTest::qWait(500);
        QVERIFY(peers(a, QStringLiteral("A")).isEmpty());
    }

    void changedSettingsRejoin()
    {
        NetEngine a(options(QStringLiteral("desktop")));
        NetEngine b(options(QStringLiteral("laptop")));
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("kitchen"), QStringLiteral("pw"), { peerOf(a) }));
        QTest::qWait(1500);
        QVERIFY(peers(a, QStringLiteral("A")).isEmpty());   // different channel

        // Same node, now pointed at the right channel
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw"), { peerOf(a) }));
        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 5000);
    }

    void discoveryByMulticast()
    {
        bool usable = false;
        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const auto &iface : interfaces) {
            const auto f = iface.flags();
            usable |= (f & QNetworkInterface::IsUp) && (f & QNetworkInterface::CanMulticast)
                      && !(f & QNetworkInterface::IsLoopBack) && !iface.addressEntries().isEmpty();
        }
        if (!usable)
            QSKIP("no network interface that can do multicast");

        // Does this machine deliver multicast to itself at all?  A host
        // firewall (firewalld's default zone, for one) drops it.  That is the
        // machine's configuration, not something this test can judge - use
        // tests/run-in-netns.sh to run it without a firewall in the way.
        {
            const quint16 probePort = static_cast<quint16>(47900 + QCoreApplication::applicationPid() % 100);
            QUdpSocket rx, tx;
            rx.setProxy(QNetworkProxy::NoProxy);
            tx.setProxy(QNetworkProxy::NoProxy);
            QVERIFY(rx.bind(QHostAddress::AnyIPv4, probePort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint));
            QVERIFY(tx.bind(QHostAddress::AnyIPv4, 0));
            tx.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
            for (const auto &iface : interfaces) {
                if (!(iface.flags() & QNetworkInterface::CanMulticast)) continue;
                rx.joinMulticastGroup(NetProtocol::multicastGroup(), iface);
                tx.setMulticastInterface(iface);
                tx.writeDatagram("probe", NetProtocol::multicastGroup(), probePort);
            }
            if (!rx.waitForReadyRead(500))
                QSKIP("multicast is not delivered on this machine (host firewall?)");
        }

        // Its own discovery port, so a SoundRoot running on this machine is
        // left alone.  No addresses are configured anywhere.
        NetEngine::Options oa = options(QStringLiteral("desktop"));
        oa.multicast = true;
        oa.discoveryPort = static_cast<quint16>(47800 + QCoreApplication::applicationPid() % 100);
        NetEngine::Options ob = oa;
        ob.deviceName = QStringLiteral("laptop");

        NetEngine a(oa);
        NetEngine b(ob);
        QTRY_VERIFY(a.dataPort() != 0 && b.dataPort() != 0);
        a.startNode(spec(QStringLiteral("A"), QStringLiteral("den"), QStringLiteral("pw")));
        b.startNode(spec(QStringLiteral("B"), QStringLiteral("den"), QStringLiteral("pw")));

        QTRY_COMPARE_WITH_TIMEOUT(peers(a, QStringLiteral("A")).size(), 1, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(peers(b, QStringLiteral("B")).size(), 1, 8000);
    }
};

QTEST_GUILESS_MAIN(TestNetEngine)
#include "tst_netengine.moc"
