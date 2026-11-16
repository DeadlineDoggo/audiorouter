#include <QTest>

#include "net/netcrypto.h"
#include "net/netprotocol.h"

using namespace NetProtocol;

class TestNetProtocol : public QObject {
    Q_OBJECT

    NetCrypto::ChannelKeys m_keys;

private slots:
    void initTestCase()
    {
        QVERIFY(NetCrypto::init());
        m_keys = NetCrypto::deriveKeys(QStringLiteral("living room"), QStringLiteral("correct horse"));
        QVERIFY(m_keys.isValid());
    }

    void keysDependOnChannelAndPassphrase()
    {
        const auto same = NetCrypto::deriveKeys(QStringLiteral(" living room "), QStringLiteral("correct horse"));
        QCOMPARE(same.aeadKey, m_keys.aeadKey);        // surrounding spaces do not matter
        QCOMPARE(same.channelId, m_keys.channelId);

        const auto otherPass = NetCrypto::deriveKeys(QStringLiteral("living room"), QStringLiteral("wrong horse"));
        QVERIFY(otherPass.aeadKey != m_keys.aeadKey);
        QVERIFY(otherPass.channelId != m_keys.channelId);

        const auto otherChannel = NetCrypto::deriveKeys(QStringLiteral("kitchen"), QStringLiteral("correct horse"));
        QVERIFY(otherChannel.aeadKey != m_keys.aeadKey);

        // The public channel id does not give away the key
        QVERIFY(!m_keys.aeadKey.contains(m_keys.channelId));

        QVERIFY(!NetCrypto::deriveKeys({}, QStringLiteral("x")).isValid());
        QVERIFY(!NetCrypto::deriveKeys(QStringLiteral("x"), {}).isValid());
    }

    void packetRoundTrip()
    {
        Header h;
        h.type = Audio;
        h.channelId = m_keys.channelId;
        h.senderId = 0x1122334455667788ULL;
        h.counter = 42;
        const QByteArray pcm(kFramesPerPacket * kBytesPerFrame, 'x');
        const QByteArray payload = encodeAudio(123456789, pcm);
        const QByteArray packet = sealPacket(m_keys, h, payload);
        QCOMPARE(packet.size(), kHeaderBytes + payload.size() + NetCrypto::kTagBytes);
        QVERIFY(packet.size() <= 1472);                 // fits one Ethernet frame
        QVERIFY(!packet.contains(QByteArray(32, 'x'))); // really encrypted

        Header back;
        QVERIFY(decodeHeader(packet, &back));
        QCOMPARE(back.type, Audio);
        QCOMPARE(back.channelId, m_keys.channelId);
        QCOMPARE(back.senderId, h.senderId);
        QCOMPARE(back.counter, h.counter);

        QByteArray plain;
        QVERIFY(openPacket(m_keys, packet, back, &plain));
        QCOMPARE(plain, payload);

        AudioPayload audio;
        QVERIFY(decodeAudio(plain, &audio));
        QCOMPARE(audio.codec, kCodecPcm16);
        QCOMPARE(int(audio.channels), kChannels);
        QCOMPARE(int(audio.sampleRate), kSampleRate);
        QCOMPARE(audio.timestamp, qint64(123456789));
        QCOMPARE(audio.samples, pcm);
    }

    void tamperingIsDetected()
    {
        Header h;
        h.type = Beacon;
        h.channelId = m_keys.channelId;
        h.senderId = 7;
        h.counter = 1;
        const QByteArray packet = sealPacket(m_keys, h, QByteArrayLiteral("{\"name\":\"laptop\"}"));

        // Flipping any single byte - header or body - must fail authentication
        for (int pos : { 4, 5, 9, 17, 30, kHeaderBytes, kHeaderBytes + 5, int(packet.size()) - 1 }) {
            QByteArray bad = packet;
            bad[pos] = char(bad[pos] ^ 0x01);
            Header bh;
            QByteArray plain;
            const bool opened = decodeHeader(bad, &bh) && openPacket(m_keys, bad, bh, &plain);
            QVERIFY2(!opened, qPrintable(QStringLiteral("byte %1").arg(pos)));
        }
    }

    void wrongKeyCannotOpen()
    {
        Header h;
        h.channelId = m_keys.channelId;
        h.senderId = 7;
        h.counter = 1;
        const QByteArray packet = sealPacket(m_keys, h, QByteArrayLiteral("secret"));
        const auto other = NetCrypto::deriveKeys(QStringLiteral("living room"), QStringLiteral("guess"));
        QByteArray plain;
        QVERIFY(!openPacket(other, packet, h, &plain));
    }

    void garbageIsNotAPacket()
    {
        Header h;
        QVERIFY(!decodeHeader({}, &h));
        QVERIFY(!decodeHeader(QByteArray(10, 'a'), &h));
        QVERIFY(!decodeHeader(QByteArray(200, 'a'), &h));           // wrong magic
        QVERIFY(!decodeHeader(QByteArrayLiteral("SRN1") + QByteArray(20, '\0'), &h));   // too short
        AudioPayload audio;
        QVERIFY(!decodeAudio(QByteArray(5, '\0'), &audio));
    }

    void replayWindow()
    {
        ReplayWindow w;
        QVERIFY(w.accept(10));
        QVERIFY(!w.accept(10));      // exact repeat
        QVERIFY(w.accept(12));
        QVERIFY(w.accept(11));       // reordered: fine once
        QVERIFY(!w.accept(11));
        QVERIFY(w.accept(500));      // jump ahead
        QVERIFY(!w.accept(12));      // far behind now
        QVERIFY(w.accept(499));
        QVERIFY(w.accept(437));      // 63 behind: still inside the window
        QVERIFY(!w.accept(436));     // 64 behind: outside
        QVERIFY(!w.accept(500));
    }

    void onlyLocalNetworkAddresses()
    {
        for (const char *ok : { "127.0.0.1", "192.168.1.20", "10.1.2.3", "172.16.0.1", "172.31.255.254",
                                "169.254.10.10", "100.100.1.1", "::ffff:192.168.1.5", "fe80::1", "fd00::1" })
            QVERIFY2(isLocalNetworkAddress(QHostAddress(QString::fromLatin1(ok))), ok);
        for (const char *no : { "8.8.8.8", "172.32.0.1", "192.169.0.1", "100.128.0.1", "1.1.1.1",
                                "2001:4860:4860::8888", "::ffff:8.8.8.8" })
            QVERIFY2(!isLocalNetworkAddress(QHostAddress(QString::fromLatin1(no))), no);
    }

    void generatedPassphrases()
    {
        const QString a = NetCrypto::generatePassphrase();
        const QString b = NetCrypto::generatePassphrase();
        QCOMPARE(a.size(), 23);   // 4 x 5 characters + 3 dashes
        QVERIFY(a != b);
        QVERIFY(NetCrypto::deriveKeys(QStringLiteral("c"), a).isValid());
    }
};

QTEST_GUILESS_MAIN(TestNetProtocol)
#include "tst_netprotocol.moc"
