#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "data/audiogroup.h"
#include "data/settings.h"

class TestAudioGroup : public QObject {
    Q_OBJECT

private slots:
    void routeKeys()
    {
        AudioRoute r;
        r.sourceAppName = AudioRoute::makeKey(QStringLiteral("Firefox"), 2);
        QCOMPARE(r.appName(), QStringLiteral("Firefox"));
        QCOMPARE(r.ordinal(), 2);

        r.sourceAppName = QStringLiteral("Waterfox");   // written by old versions
        QCOMPARE(r.appName(), QStringLiteral("Waterfox"));
        QCOMPARE(r.ordinal(), -1);
    }

    void roundTrip()
    {
        AudioGroup g(QStringLiteral("Gaming"), QStringLiteral("#e74c3c"), QStringLiteral("audio-headset"));
        g.active = true;
        AudioRoute r;
        r.sourceAppName = AudioRoute::makeKey(QStringLiteral("Discord"), 0);
        r.mediaHint = QStringLiteral("Voice");
        r.outputSinkNames = { QStringLiteral("headset"), QStringLiteral("soundroot_virt_aa") };
        g.routes.append(r);
        VirtualSinkDef vs;
        vs.displayName = QStringLiteral("To the laptop");
        vs.paName = QStringLiteral("soundroot_virt_aa");
        vs.loopbackTargets = { QStringLiteral("speakers") };
        vs.network.enabled = true;
        vs.network.channel = QStringLiteral("living room");
        vs.network.passphrase = QStringLiteral("secret words");
        vs.network.staticPeers = { QStringLiteral("192.168.1.20:47781") };
        g.virtualSinks.append(vs);

        const AudioGroup back = AudioGroup::fromJson(g.toJson());
        QCOMPARE(back.id, g.id);
        QCOMPARE(back.name, g.name);
        QCOMPARE(back.color, g.color);
        QCOMPARE(back.icon, g.icon);
        QCOMPARE(back.active, true);
        QCOMPARE(back.routes.size(), 1);
        QCOMPARE(back.routes[0].sourceAppName, r.sourceAppName);
        QCOMPARE(back.routes[0].mediaHint, r.mediaHint);
        QCOMPARE(back.routes[0].outputSinkNames, r.outputSinkNames);
        QCOMPARE(back.virtualSinks.size(), 1);
        QCOMPARE(back.virtualSinks[0].loopbackTargets, vs.loopbackTargets);
        QVERIFY(back.virtualSinks[0].network == vs.network);
        QCOMPARE(back.linkCount(), 3);
    }

    // A file as written by 0.8.1: no "media", no "network", a legacy route
    void readsOldFormat()
    {
        const QByteArray json = R"({
            "id": "abc", "name": "Batman", "color": "#4a90d9", "icon": "audio-card", "active": false,
            "routes": [
                { "source": "Waterfox", "volumePercent": 100, "outputs": ["alsa_output.analog"] },
                { "source": "haruna\t0", "volumePercent": 80, "outputs": ["alsa_output.analog", "hdmi"] }
            ],
            "virtualSinks": [
                { "displayName": "Mix", "paName": "soundroot_virt_08080fb6", "loopbackTargets": ["hdmi"] }
            ] })";
        const AudioGroup g = AudioGroup::fromJson(QJsonDocument::fromJson(json).object());
        QCOMPARE(g.id, QStringLiteral("abc"));
        QCOMPARE(g.routes.size(), 2);
        QCOMPARE(g.routes[0].ordinal(), -1);
        QCOMPARE(g.routes[1].ordinal(), 0);
        QCOMPARE(g.routes[1].outputSinkNames.size(), 2);
        QCOMPARE(g.virtualSinks.size(), 1);
        QVERIFY(!g.virtualSinks[0].isNetwork());
        // A plain virtual node is written back exactly as old versions wrote it
        QVERIFY(!g.virtualSinks[0].toJson().contains(QStringLiteral("network")));
    }

    void missingIdGetsAUniqueOne()
    {
        const QJsonObject noId { { QStringLiteral("name"), QStringLiteral("x") } };
        const AudioGroup a = AudioGroup::fromJson(noId);
        const AudioGroup b = AudioGroup::fromJson(noId);
        QVERIFY(!a.id.isEmpty());
        QVERIFY(a.id != b.id);
    }

    void dropsBrokenEntries()
    {
        const QByteArray json = R"({ "id": "g", "name": "n",
            "routes": [
                { "source": "A\t0", "outputs": ["x", "x", ""] },
                { "source": "A\t0", "outputs": ["y"] },
                { "source": "B\t0", "outputs": [] },
                { "source": "", "outputs": ["x"] }
            ],
            "virtualSinks": [ { "displayName": "v", "paName": "" } ] })";
        const AudioGroup g = AudioGroup::fromJson(QJsonDocument::fromJson(json).object());
        QCOMPARE(g.routes.size(), 1);   // duplicates merged, empty ones dropped
        QCOMPARE(g.routes[0].outputSinkNames, QStringList({ QStringLiteral("x"), QStringLiteral("y") }));
        QCOMPARE(g.virtualSinks.size(), 0);
    }

    void settingsSaveAndLoad()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sub/groups.json"));
        Settings::instance().setFilePath(path);

        SettingsData data;
        data.groups.append(AudioGroup(QStringLiteral("One")));
        data.groups.append(AudioGroup(QStringLiteral("Two")));
        data.groups[0].active = true;
        data.groups[1].active = true;   // invalid: two active rooms
        data.sinkLabels.insert(QStringLiteral("hdmi"), QStringLiteral("Living room TV"));
        QVERIFY(Settings::instance().save(data));

        // Holds passphrases: readable by the owner only
        QCOMPARE(QFile::permissions(path),
                 QFileDevice::ReadOwner | QFileDevice::WriteOwner
                     | QFileDevice::ReadUser | QFileDevice::WriteUser);

        const SettingsData back = Settings::instance().load();
        QCOMPARE(back.groups.size(), 2);
        QCOMPARE(back.groups[0].name, QStringLiteral("One"));
        QVERIFY(back.groups[0].active);
        QVERIFY(!back.groups[1].active);   // only one room can be active
        QCOMPARE(back.sinkLabels.value(QStringLiteral("hdmi")), QStringLiteral("Living room TV"));
        Settings::instance().setFilePath({});
    }

    void corruptFileIsKept()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("groups.json"));
        Settings::instance().setFilePath(path);
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{ \"groups\": [ { \"name\": \"half writ");
        }
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not parse")));
        const SettingsData data = Settings::instance().load();
        QVERIFY(data.groups.isEmpty());
        // The unreadable file was copied aside, so saving cannot destroy it
        const QStringList backups = QDir(dir.path()).entryList({ QStringLiteral("groups.json.corrupt-*") });
        QCOMPARE(backups.size(), 1);
        Settings::instance().setFilePath({});
    }

    void missingFileIsFine()
    {
        QTemporaryDir dir;
        Settings::instance().setFilePath(dir.filePath(QStringLiteral("nothing.json")));
        QVERIFY(Settings::instance().load().groups.isEmpty());
        Settings::instance().setFilePath({});
    }
};

QTEST_GUILESS_MAIN(TestAudioGroup)
#include "tst_audiogroup.moc"
