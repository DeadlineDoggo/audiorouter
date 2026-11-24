#include <QTemporaryDir>
#include <QTest>

#include "data/settings.h"
#include "fakebackend.h"
#include "models/grouplistmodel.h"
#include "routing/routingengine.h"

// Shorthands for node keys
static QString in(uint32_t index) { return QStringLiteral("input:%1").arg(index); }
static QString sink(const QString &name) { return QStringLiteral("sink:") + name; }
static QString vin(const QString &pa) { return QStringLiteral("vsinkin:") + pa; }
static QString vout(const QString &pa) { return QStringLiteral("vsinkout:") + pa; }
static QString mic(const QString &name) { return QStringLiteral("mic:") + name; }
static QString rec(uint32_t index) { return QStringLiteral("rec:%1").arg(index); }

static const QString SPK = QStringLiteral("speakers");
static const QString HP  = QStringLiteral("headphones");
static const QString TV  = QStringLiteral("hdmi");
static const QString MIC = QStringLiteral("builtin_mic");
static const QString USB = QStringLiteral("usb_mic");
static const QString TEAMS = QStringLiteral("Teams for Linux");

static QStringList sorted(QStringList list) { list.sort(); return list; }

/// One test's world: a fake server with three outputs, an empty config file,
/// a room model and the engine under test.
struct Rig {
    QTemporaryDir  dir;
    FakeBackend    backend;
    FakeNetwork    net;
    GroupListModel groups;
    RoutingEngine  engine { &backend, &groups };

    Rig()
    {
        Settings::instance().setFilePath(dir.filePath(QStringLiteral("groups.json")));
        groups.load();
        engine.setNetworkController(&net);
        backend.addSink(SPK, QStringLiteral("Built-in Speakers"));   // the default
        backend.addSink(HP, QStringLiteral("USB Headphones"));
        backend.addSink(TV, QStringLiteral("Living Room TV"));
        backend.addSource(MIC, QStringLiteral("Built-in Microphone"));   // the default
        backend.addSource(USB, QStringLiteral("USB Microphone"));
    }
    ~Rig() { Settings::instance().setFilePath({}); }

    int addRoom(const QString &name)
    {
        groups.addGroup(name, QStringLiteral("#4a90d9"));
        return groups.rowCount() - 1;
    }

    /// The outputs of the combine-sink a stream plays on, sorted.
    QStringList combineOf(uint32_t stream) const
    {
        QStringList slaves = backend.combines.value(backend.sinkOf(stream));
        slaves.sort();
        return slaves;
    }

    bool hasWire(int row, const QString &from, const QString &to)
    {
        const auto wires = engine.connections(row);
        for (const auto &w : wires) {
            const auto m = w.toMap();
            if (m.value(QStringLiteral("from")) == from && m.value(QStringLiteral("to")) == to)
                return true;
        }
        return false;
    }
};

class TestRoutingEngine : public QObject {
    Q_OBJECT

private slots:
    // ── Applying a room ─────────────────────────────────────────────────

    void wiringAnInactiveRoomChangesNothingLive()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));

        QVERIFY(rig.engine.connectNodes(room, in(s), sink(HP)));
        QCOMPARE(rig.backend.sinkOf(s), SPK);
        QCOMPARE(rig.backend.moveCalls, 0);
        QVERIFY(rig.hasWire(room, in(s), sink(HP)));
    }

    void activationRoutesStreams()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));

        rig.engine.activateRoom(room);
        QCOMPARE(rig.backend.sinkOf(s), HP);
        QVERIFY(rig.groups.isGroupActive(room));
    }

    void wiringAnActiveRoomAppliesImmediately()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        rig.engine.activateRoom(room);
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));

        rig.engine.connectNodes(room, sink(TV), in(s));   // either direction works
        QCOMPARE(rig.backend.sinkOf(s), TV);
    }

    void severalOutputsUseOneCombineSink()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Party"));
        rig.engine.activateRoom(room);
        const uint32_t a = rig.backend.addStream(QStringLiteral("mpv"));
        const uint32_t b = rig.backend.addStream(QStringLiteral("Firefox"));

        rig.engine.connectNodes(room, in(a), sink(HP));
        rig.engine.connectNodes(room, in(a), sink(TV));
        QTRY_COMPARE(rig.combineOf(a), sorted({ HP, TV }));

        // A second stream with the same outputs shares the module
        rig.engine.connectNodes(room, in(b), sink(TV));
        rig.engine.connectNodes(room, in(b), sink(HP));
        QTRY_COMPARE(rig.backend.sinkOf(b), rig.backend.sinkOf(a));
        QCOMPARE(rig.backend.combines.size(), 1);
    }

    void combineSinkThatLoadsLate()
    {
        Rig rig;
        rig.backend.asyncCombine = true;
        const int room = rig.addRoom(QStringLiteral("Party"));
        rig.engine.activateRoom(room);
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));
        rig.engine.connectNodes(room, in(s), sink(TV));

        // Requested, not there yet: the stream stays where it was
        QCOMPARE(rig.backend.pendingCombines.size(), 1);
        QCOMPARE(rig.backend.sinkOf(s), HP);

        // Its arrival alone (no timer, no retry) finishes the job
        rig.backend.flush();
        QTRY_COMPARE(rig.combineOf(s), sorted({ HP, TV }));
    }

    void changingOutputsNeverDropsTheStreamOnTheDefault()
    {
        Rig rig;
        rig.backend.asyncCombine = true;
        const int room = rig.addRoom(QStringLiteral("Party"));
        rig.engine.activateRoom(room);
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));
        rig.engine.connectNodes(room, in(s), sink(TV));
        rig.backend.flush();
        QTRY_COMPARE(rig.combineOf(s), sorted({ HP, TV }));
        const QString first = rig.backend.sinkOf(s);

        // Adding a third output needs a new combine-sink.  Until that has
        // loaded the old one must stay, or the stream would fall back to the
        // speakers for a moment.
        rig.engine.connectNodes(room, in(s), sink(SPK));
        QCOMPARE(rig.backend.sinkOf(s), first);
        QVERIFY(rig.backend.combines.contains(first));

        rig.backend.flush();
        QTRY_COMPARE(rig.combineOf(s), sorted({ HP, TV, SPK }));
        QTRY_VERIFY(!rig.backend.combines.contains(first));   // and then it is cleaned up
    }

    // ── Things that happen later ────────────────────────────────────────

    void streamThatStartsLaterIsRouted()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const uint32_t first = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(first), sink(HP));
        rig.engine.activateRoom(room);
        rig.backend.removeStream(first);

        // The application plays again: a brand-new stream index.  Nothing
        // but the server's own notification tells the engine.
        const uint32_t second = rig.backend.addStream(QStringLiteral("mpv"));
        QCOMPARE(rig.backend.sinkOf(second), SPK);
        QTRY_COMPARE(rig.backend.sinkOf(second), HP);
    }

    void deviceThatIsPluggedInLaterIsUsed()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));
        rig.engine.activateRoom(room);
        QCOMPARE(rig.backend.sinkOf(s), HP);

        rig.backend.removeSink(HP);                      // unplugged
        QTRY_COMPARE(rig.backend.sinkOf(s), SPK);        // server fallback
        QVERIFY(rig.hasWire(room, in(s), sink(HP)));     // the wire is kept

        const auto offline = rig.engine.offlineSinks(room);
        QCOMPARE(offline.size(), 1);
        QCOMPARE(offline[0].toMap().value(QStringLiteral("name")).toString(), HP);
        // Remembered from when it was present
        QCOMPARE(offline[0].toMap().value(QStringLiteral("description")).toString(),
                 QStringLiteral("USB Headphones"));

        rig.backend.addSink(HP, QStringLiteral("USB Headphones"));   // plugged back in
        QTRY_COMPARE(rig.backend.sinkOf(s), HP);
        QVERIFY(rig.engine.offlineSinks(room).isEmpty());
    }

    void oneOfTwoOutputsUnplugged()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Party"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));
        rig.engine.connectNodes(room, in(s), sink(TV));
        rig.engine.activateRoom(room);
        QTRY_COMPARE(rig.combineOf(s), sorted({ HP, TV }));

        rig.backend.removeSink(TV);
        QTRY_COMPARE(rig.backend.sinkOf(s), HP);          // plays on what is left
        QTRY_VERIFY(rig.backend.combines.isEmpty());      // combine-sink is gone

        rig.backend.addSink(TV);
        QTRY_COMPARE(rig.combineOf(s), sorted({ HP, TV }));
    }

    void serverRestartIsRecoveredFrom()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Mix"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), vin(pa));
        rig.engine.connectNodes(room, vout(pa), sink(HP));
        rig.engine.activateRoom(room);
        QTRY_COMPARE(rig.backend.sinkOf(s), pa);   // once the node's sink exists

        rig.backend.killServer();
        QVERIFY(rig.backend.virtualSinks.isEmpty());

        // It comes back with its devices; the application reconnects
        rig.backend.connected = true;
        rig.backend.addSink(SPK);
        rig.backend.addSink(HP);
        const uint32_t again = rig.backend.addStream(QStringLiteral("mpv"));
        emit rig.backend.serverConnected();

        QTRY_VERIFY(rig.backend.virtualSinks.contains(pa));
        QTRY_COMPARE(rig.backend.sinkOf(again), pa);
        QTRY_COMPARE(rig.backend.loopbacks.value(pa), QStringList({ HP }));
    }

    void doesNotFightAServerThatUndoesMoves()
    {
        Rig rig;
        rig.backend.refuseMoves = true;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("backing off")));
        rig.engine.activateRoom(room);
        for (int i = 0; i < 50; ++i)
            rig.engine.reconcile();
        QVERIFY(rig.backend.moveCalls <= 6);
    }

    // ── Deactivating and switching ──────────────────────────────────────

    void deactivationPutsEverythingBack()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Party"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Mix"));
        const uint32_t a = rig.backend.addStream(QStringLiteral("mpv"));
        const uint32_t b = rig.backend.addStream(QStringLiteral("Firefox"));
        const uint32_t untouched = rig.backend.addStream(QStringLiteral("Game"), {}, TV);
        rig.engine.connectNodes(room, in(a), sink(HP));
        rig.engine.connectNodes(room, in(a), sink(TV));
        rig.engine.connectNodes(room, in(b), vin(pa));
        rig.engine.connectNodes(room, vout(pa), sink(HP));
        rig.engine.activateRoom(room);
        QVERIFY(rig.backend.virtualSinks.contains(pa));
        QCOMPARE(rig.backend.loopbacks.value(pa), QStringList({ HP }));

        rig.engine.deactivateRoom(room);
        QTRY_COMPARE(rig.backend.sinkOf(a), SPK);
        QCOMPARE(rig.backend.sinkOf(b), SPK);
        QCOMPARE(rig.backend.sinkOf(untouched), TV);   // not ours: left alone
        QTRY_VERIFY(rig.backend.combines.isEmpty());
        QVERIFY(rig.backend.virtualSinks.isEmpty());
        QVERIFY(rig.backend.loopbacks.isEmpty());
        QVERIFY(!rig.groups.isGroupActive(room));
    }

    void removingAWireFromAnActiveRoomRestoresTheStream()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));
        rig.engine.activateRoom(room);
        QCOMPARE(rig.backend.sinkOf(s), HP);

        rig.engine.disconnectNodes(room, in(s), sink(HP));
        QCOMPARE(rig.backend.sinkOf(s), SPK);
        QVERIFY(rig.engine.connections(room).isEmpty());
    }

    void onlyOneRoomIsActive()
    {
        Rig rig;
        const int music = rig.addRoom(QStringLiteral("Music"));
        const int movie = rig.addRoom(QStringLiteral("Movie"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        const uint32_t t = rig.backend.addStream(QStringLiteral("Firefox"));
        rig.engine.connectNodes(music, in(s), sink(HP));
        rig.engine.connectNodes(music, in(t), sink(HP));
        rig.engine.connectNodes(movie, in(s), sink(TV));

        rig.engine.activateRoom(music);
        rig.engine.activateRoom(movie);
        QVERIFY(!rig.groups.isGroupActive(music));
        QVERIFY(rig.groups.isGroupActive(movie));
        QCOMPARE(rig.backend.sinkOf(s), TV);    // rerouted by the new room
        QCOMPARE(rig.backend.sinkOf(t), SPK);   // only the old room wired it
    }

    void shortcutsCycleFromTheActiveRoom()
    {
        Rig rig;
        rig.addRoom(QStringLiteral("A"));
        rig.addRoom(QStringLiteral("B"));
        rig.addRoom(QStringLiteral("C"));

        rig.engine.activateRelative(+1);
        QCOMPARE(rig.groups.activeRow(), 0);
        rig.engine.activateRelative(+1);
        QCOMPARE(rig.groups.activeRow(), 1);
        rig.engine.activateRelative(-1);
        rig.engine.activateRelative(-1);
        QCOMPARE(rig.groups.activeRow(), 2);   // wrapped around
        rig.engine.activateRelative(+1);
        QCOMPARE(rig.groups.activeRow(), 0);
    }

    void removingAnActiveRoomTearsItDown()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Mix"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), vin(pa));
        rig.engine.activateRoom(room);

        rig.engine.removeRoom(room);
        QCOMPARE(rig.groups.rowCount(), 0);
        QVERIFY(rig.backend.virtualSinks.isEmpty());
        QCOMPARE(rig.backend.sinkOf(s), SPK);
    }

    // ── What is saved, and what the UI is shown ─────────────────────────

    void configurationSurvivesARestart()
    {
        QTemporaryDir dir;
        Settings::instance().setFilePath(dir.filePath(QStringLiteral("groups.json")));
        QString pa;
        {
            FakeBackend backend;
            GroupListModel groups;
            groups.load();
            RoutingEngine engine(&backend, &groups);
            backend.addSink(SPK);
            backend.addSink(HP, QStringLiteral("USB Headphones"));
            groups.addGroup(QStringLiteral("Music"), QStringLiteral("#50c878"));
            pa = engine.addVirtualSink(0, QStringLiteral("Mix"));
            const uint32_t s = backend.addStream(QStringLiteral("mpv"), QStringLiteral("Song"));
            engine.connectNodes(0, in(s), sink(HP));
            engine.connectNodes(0, in(s), vin(pa));
            engine.connectNodes(0, vout(pa), sink(SPK));
            engine.activateRoom(0);
        }
        {
            // A new session: other stream indices, and the app is not
            // playing yet when the room is applied.
            FakeBackend backend;
            backend.nextIndex = 700;
            GroupListModel groups;
            groups.load();
            RoutingEngine engine(&backend, &groups);
            backend.addSink(SPK);
            backend.addSink(HP);
            emit backend.serverConnected();

            QCOMPARE(groups.rowCount(), 1);
            QVERIFY(groups.isGroupActive(0));
            QCOMPARE(groups.data(groups.index(0), GroupListModel::LinkCountRole).toInt(), 3);
            QVERIFY(backend.virtualSinks.contains(pa));
            QCOMPARE(backend.loopbacks.value(pa), QStringList({ SPK }));

            // Shown as a source that is not running
            const auto ghosts = engine.offlineSources(0);
            QCOMPARE(ghosts.size(), 1);
            QCOMPARE(ghosts[0].toMap().value(QStringLiteral("appName")).toString(), QStringLiteral("mpv"));

            const uint32_t s = backend.addStream(QStringLiteral("mpv"), QStringLiteral("Song"));
            QTRY_COMPARE(backend.combines.value(backend.sinkOf(s)).size(), 2);
            QVERIFY(engine.offlineSources(0).isEmpty());
        }
        Settings::instance().setFilePath({});
    }

    void aSecondStreamOfAnAppGetsItsOwnRoute()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        rig.engine.activateRoom(room);
        const uint32_t music = rig.backend.addStream(QStringLiteral("Firefox"), QStringLiteral("Music"));
        const uint32_t call = rig.backend.addStream(QStringLiteral("Firefox"), QStringLiteral("Call"));

        // Wire the *second* stream first: it must not be mistaken for "stream 0"
        rig.engine.connectNodes(room, in(call), sink(HP));
        QCOMPARE(rig.backend.sinkOf(call), HP);
        QCOMPARE(rig.backend.sinkOf(music), SPK);

        rig.engine.connectNodes(room, in(music), sink(TV));
        QCOMPARE(rig.backend.sinkOf(music), TV);
        QCOMPARE(rig.backend.sinkOf(call), HP);

        // The music tab closes; the call stays on the headphones
        rig.backend.removeStream(music);
        rig.engine.reconcile();
        QCOMPARE(rig.backend.sinkOf(call), HP);
    }

    void removingAStreamsWiresWhileItIsNotPlaying()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), sink(HP));
        rig.backend.removeStream(s);

        const auto ghosts = rig.engine.offlineSources(room);
        QCOMPARE(ghosts.size(), 1);
        const QString ghostKey = ghosts[0].toMap().value(QStringLiteral("key")).toString();
        QVERIFY(rig.hasWire(room, ghostKey, sink(HP)));

        // The saved source can still be rewired or removed
        QVERIFY(rig.engine.connectNodes(room, ghostKey, sink(TV)));
        QCOMPARE(rig.engine.connections(room).size(), 2);
        rig.engine.disconnectNode(room, ghostKey);
        QVERIFY(rig.engine.connections(room).isEmpty());
        QVERIFY(rig.engine.offlineSources(room).isEmpty());
    }

    void invalidWiresAreRefused()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Music"));
        const int other = rig.addRoom(QStringLiteral("Other"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Mix"));
        const QString foreign = rig.engine.addVirtualSink(other, QStringLiteral("Theirs"));
        const uint32_t a = rig.backend.addStream(QStringLiteral("mpv"));
        const uint32_t b = rig.backend.addStream(QStringLiteral("Firefox"));

        QVERIFY(!rig.engine.connectNodes(room, in(a), in(b)));           // source to source
        QVERIFY(!rig.engine.connectNodes(room, sink(HP), sink(TV)));     // output to output
        QVERIFY(!rig.engine.connectNodes(room, in(9999), sink(HP)));     // no such stream
        QVERIFY(!rig.engine.connectNodes(room, in(a), vin(foreign)));    // another room's node
        QVERIFY(!rig.engine.connectNodes(room, vout(pa), vin(pa)));      // node into itself
        QVERIFY(!rig.engine.connectNodes(room, in(a), vout(pa)));        // into an output side
        QVERIFY(!rig.engine.connectNodes(7, in(a), sink(HP)));           // no such room
        QVERIFY(rig.engine.connections(room).isEmpty());
    }

    void virtualNodesCannotFormALoop()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Studio"));
        const QString a = rig.engine.addVirtualSink(room, QStringLiteral("A"));
        const QString b = rig.engine.addVirtualSink(room, QStringLiteral("B"));
        const QString c = rig.engine.addVirtualSink(room, QStringLiteral("C"));

        QVERIFY(rig.engine.connectNodes(room, vout(a), vin(b)));
        QVERIFY(rig.engine.connectNodes(room, vout(b), vin(c)));
        QVERIFY(!rig.engine.canConnect(room, vout(c), vin(a)));   // would howl
        QVERIFY(!rig.engine.connectNodes(room, vout(b), vin(a)));
        QVERIFY(rig.engine.canConnect(room, vout(a), vin(c)));    // a second path is fine
    }

    void removingAVirtualNodeRemovesItsWires()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Studio"));
        const QString a = rig.engine.addVirtualSink(room, QStringLiteral("A"));
        const QString b = rig.engine.addVirtualSink(room, QStringLiteral("B"));
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), vin(a));
        rig.engine.connectNodes(room, vout(b), vin(a));
        rig.engine.connectNodes(room, vout(a), sink(HP));
        rig.engine.activateRoom(room);
        QTRY_COMPARE(rig.backend.sinkOf(s), a);

        rig.engine.removeVirtualSink(room, a);
        QVERIFY(rig.engine.connections(room).isEmpty());
        QVERIFY(!rig.backend.virtualSinks.contains(a));
        QVERIFY(rig.backend.virtualSinks.contains(b));
        QTRY_COMPARE(rig.backend.sinkOf(s), SPK);
    }

    void uiIsToldOnlyWhenTheTopologyChanges()
    {
        Rig rig;
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        QCoreApplication::processEvents();
        int notifications = 0;
        connect(&rig.engine, &RoutingEngine::connectionsChanged, this, [&]() { ++notifications; });

        // A volume change: the server reports "streams changed"
        rig.backend.inputs[0].volumePercent = 40;
        emit rig.backend.sinkInputsChanged();
        emit rig.backend.sinkInputsChanged();
        QCOMPARE(notifications, 0);

        rig.backend.removeStream(s);
        QCOMPARE(notifications, 1);
    }

    // ── Network nodes ───────────────────────────────────────────────────

    void networkNodeLifecycle()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Shared"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("To the laptop"));
        rig.engine.configureVirtualSink(room, pa, {
            { QStringLiteral("networkEnabled"), true },
            { QStringLiteral("channel"), QStringLiteral("living room") },
            { QStringLiteral("passphrase"), QStringLiteral("correct horse") },
            { QStringLiteral("staticPeers"), QStringList({ QStringLiteral("192.168.1.9") }) },
        });
        const uint32_t s = rig.backend.addStream(QStringLiteral("mpv"));
        rig.engine.connectNodes(room, in(s), vin(pa));

        // Nothing touches the network while the room is inactive
        QVERIFY(rig.net.nodes.isEmpty());

        rig.engine.activateRoom(room);
        QTRY_VERIFY(rig.net.nodes.contains(pa));
        QCOMPARE(rig.net.nodes[pa].channel, QStringLiteral("living room"));
        QCOMPARE(rig.net.nodes[pa].passphrase, QStringLiteral("correct horse"));
        QCOMPARE(rig.net.nodes[pa].staticPeers, QStringList({ QStringLiteral("192.168.1.9") }));
        QTRY_COMPARE(rig.backend.sinkOf(s), pa);            // the app plays into the node
        QTRY_VERIFY(rig.backend.captures.contains(pa));     // and the node is recorded
        // The node's own input is NOT looped to local outputs
        QVERIFY(rig.backend.loopbacks.isEmpty());
        // Nothing is wired to OUT: we do not play, and tell the peers so
        QCOMPARE(rig.backend.playbackStream(pa), kInvalidIndex);
        QCOMPARE(rig.net.listening.value(pa), false);

        // Wire OUT to two devices: received audio plays on both
        rig.engine.connectNodes(room, vout(pa), sink(HP));
        rig.engine.connectNodes(room, vout(pa), sink(TV));
        QTRY_VERIFY(rig.backend.playbackStream(pa) != kInvalidIndex);
        QTRY_COMPARE(rig.combineOf(rig.backend.playbackStream(pa)), sorted({ HP, TV }));
        QCOMPARE(rig.net.listening.value(pa), true);

        // Unwire OUT again
        rig.engine.disconnectNode(room, vout(pa));
        QTRY_COMPARE(rig.backend.playbackStream(pa), kInvalidIndex);
        QCOMPARE(rig.net.listening.value(pa), false);

        rig.engine.deactivateRoom(room);
        QVERIFY(rig.net.nodes.isEmpty());
        QVERIFY(rig.backend.captures.isEmpty());
        QVERIFY(rig.backend.virtualSinks.isEmpty());
    }

    void networkSettingsChangeRestartsTheNode()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Shared"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Net"));
        rig.engine.configureVirtualSink(room, pa, {
            { QStringLiteral("networkEnabled"), true },
            { QStringLiteral("channel"), QStringLiteral("a") },
            { QStringLiteral("passphrase"), QStringLiteral("one") },
        });
        rig.engine.activateRoom(room);
        QCOMPARE(rig.net.starts, 1);

        rig.engine.reconcile();
        rig.engine.reconcile();
        QCOMPARE(rig.net.starts, 1);   // unchanged settings: left alone

        rig.engine.configureVirtualSink(room, pa, { { QStringLiteral("passphrase"), QStringLiteral("two") } });
        QCOMPARE(rig.net.starts, 2);
        QCOMPARE(rig.net.nodes[pa].passphrase, QStringLiteral("two"));

        // Switched back to a plain virtual node: leaves the network, and
        // passes its input through to its outputs again
        rig.engine.connectNodes(room, vout(pa), sink(HP));
        rig.engine.configureVirtualSink(room, pa, { { QStringLiteral("networkEnabled"), false } });
        QVERIFY(rig.net.nodes.isEmpty());
        QCOMPARE(rig.backend.loopbacks.value(pa), QStringList({ HP }));
        QCOMPARE(rig.backend.playbackStream(pa), kInvalidIndex);
    }

    void incompleteNetworkSettingsStartNothing()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Shared"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Net"));
        rig.engine.configureVirtualSink(room, pa, {
            { QStringLiteral("networkEnabled"), true },
            { QStringLiteral("channel"), QStringLiteral("a") },
        });   // no passphrase
        rig.engine.activateRoom(room);
        QVERIFY(rig.net.nodes.isEmpty());
        QVERIFY(rig.backend.captures.isEmpty());
    }

    // ── Recording applications ──────────────────────────────────────────

    void aMicrophoneIsWiredIntoACall()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        const uint32_t call = rig.backend.addRecorder(TEAMS);
        QCOMPARE(rig.backend.sourceOf(call), MIC);

        QVERIFY(rig.engine.connectNodes(room, rec(call), mic(USB)));   // either direction
        QCOMPARE(rig.backend.recorderMoveCalls, 0);                    // room is off
        QVERIFY(rig.hasWire(room, mic(USB), rec(call)));

        rig.engine.activateRoom(room);
        QCOMPARE(rig.backend.sourceOf(call), USB);
    }

    void aNodeIsSentIntoACall()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Call"));
        rig.engine.activateRoom(room);
        const uint32_t music = rig.backend.addStream(QStringLiteral("Spotify"));
        const uint32_t call = rig.backend.addRecorder(TEAMS);

        QVERIFY(rig.engine.connectNodes(room, in(music), vin(pa)));
        QVERIFY(rig.engine.connectNodes(room, mic(MIC), vin(pa)));
        QVERIFY(rig.engine.connectNodes(room, vout(pa), rec(call)));
        QVERIFY(rig.engine.connectNodes(room, vout(pa), sink(HP)));   // and I hear it too

        QCOMPARE(rig.backend.sinkOf(music), pa);
        QCOMPARE(rig.backend.micInputs.value(pa), QStringList({ MIC }));
        QCOMPARE(rig.backend.sourceOf(call), virtualMicName(pa));
        QCOMPARE(rig.backend.loopbacks.value(pa), QStringList({ HP }));
        QVERIFY(rig.hasWire(room, mic(MIC), vin(pa)));
        QVERIFY(rig.hasWire(room, vout(pa), rec(call)));
        QCOMPARE(rig.groups.data(rig.groups.index(room), GroupListModel::LinkCountRole).toInt(), 4);
    }

    void aSecondWireIntoARecorderReplacesTheFirst()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Call"));
        rig.engine.activateRoom(room);
        const uint32_t call = rig.backend.addRecorder(TEAMS);

        rig.engine.connectNodes(room, mic(USB), rec(call));
        QVERIFY(rig.engine.canConnect(room, vout(pa), rec(call)));
        rig.engine.connectNodes(room, vout(pa), rec(call));

        QCOMPARE(rig.groups.groupAt(room)->captures.size(), 1);
        QVERIFY(!rig.hasWire(room, mic(USB), rec(call)));
        QVERIFY(rig.hasWire(room, vout(pa), rec(call)));
        QCOMPARE(rig.backend.sourceOf(call), virtualMicName(pa));
    }

    void aNewCallIsRoutedLikeTheLastOne()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        rig.engine.activateRoom(room);
        const uint32_t first = rig.backend.addRecorder(TEAMS, QStringLiteral("RecordStream"));
        rig.engine.connectNodes(room, mic(USB), rec(first));
        QCOMPARE(rig.backend.sourceOf(first), USB);

        // The call ends; it is shown as an application that is not recording
        rig.backend.removeRecorder(first);
        const auto ghosts = rig.engine.offlineRecorders(room);
        QCOMPARE(ghosts.size(), 1);
        QCOMPARE(ghosts[0].toMap().value(QStringLiteral("appName")).toString(), TEAMS);

        // The next call opens a new stream, on the default microphone
        const uint32_t second = rig.backend.addRecorder(TEAMS, QStringLiteral("RecordStream"));
        QTRY_COMPARE(rig.backend.sourceOf(second), USB);
        QVERIFY(rig.hasWire(room, mic(USB), rec(second)));
        QVERIFY(rig.engine.offlineRecorders(room).isEmpty());
    }

    void recordersGoBackToTheDefaultMicrophone()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Call"));
        rig.engine.activateRoom(room);
        const uint32_t call = rig.backend.addRecorder(TEAMS);
        const uint32_t other = rig.backend.addRecorder(QStringLiteral("OBS"));
        rig.engine.connectNodes(room, vout(pa), rec(call));
        rig.engine.connectNodes(room, mic(USB), rec(other));
        QCOMPARE(rig.backend.sourceOf(call), virtualMicName(pa));

        // Removing a wire of an active room
        rig.engine.disconnectNodes(room, mic(USB), rec(other));
        QCOMPARE(rig.backend.sourceOf(other), MIC);

        // Switching the room off
        rig.engine.deactivateRoom(room);
        QCOMPARE(rig.backend.sourceOf(call), MIC);
        QVERIFY(!rig.backend.virtualSinks.contains(pa));
        QCOMPARE(rig.backend.sourceIndexOf(virtualMicName(pa)), kInvalidIndex);

        // ... and on again
        rig.engine.activateRoom(room);
        QCOMPARE(rig.backend.sourceOf(call), virtualMicName(pa));
    }

    void microphonesFollowThePlug()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Call"));
        rig.engine.activateRoom(room);
        const uint32_t call = rig.backend.addRecorder(TEAMS);
        rig.engine.connectNodes(room, mic(USB), vin(pa));
        rig.engine.connectNodes(room, mic(USB), rec(call));
        QCOMPARE(rig.backend.micInputs.value(pa), QStringList({ USB }));

        rig.backend.removeSource(USB);
        QTRY_VERIFY(!rig.backend.micInputs.contains(pa));
        QCOMPARE(rig.backend.sourceOf(call), MIC);   // the server re-homed it
        const auto ghosts = rig.engine.offlineMics(room);
        QCOMPARE(ghosts.size(), 1);
        QCOMPARE(ghosts[0].toMap().value(QStringLiteral("key")).toString(), mic(USB));
        QCOMPARE(ghosts[0].toMap().value(QStringLiteral("description")).toString(),
                 QStringLiteral("USB Microphone"));   // remembered while it was there

        rig.backend.addSource(USB, QStringLiteral("USB Microphone"));
        QTRY_COMPARE(rig.backend.micInputs.value(pa), QStringList({ USB }));
        QTRY_COMPARE(rig.backend.sourceOf(call), USB);
        QVERIFY(rig.engine.offlineMics(room).isEmpty());
    }

    void invalidRecordingWiresAreRefused()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        const int other = rig.addRoom(QStringLiteral("Other"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Call"));
        const QString foreign = rig.engine.addVirtualSink(other, QStringLiteral("Elsewhere"));
        const uint32_t call = rig.backend.addRecorder(TEAMS);
        const uint32_t music = rig.backend.addStream(QStringLiteral("Spotify"));

        QVERIFY(!rig.engine.canConnect(room, mic(MIC), sink(HP)));        // no monitoring yet
        QVERIFY(!rig.engine.canConnect(room, in(music), rec(call)));      // through a node
        QVERIFY(!rig.engine.canConnect(room, mic(MIC), vin(foreign)));    // another room's node
        QVERIFY(!rig.engine.canConnect(room, vout(foreign), rec(call)));
        QVERIFY(!rig.engine.canConnect(room, mic(MIC), rec(9999)));       // no such recorder
        QVERIFY(!rig.engine.canConnect(room, mic(virtualMicName(pa)), rec(call)));
        QVERIFY(!rig.engine.canConnect(room, mic(MIC), mic(USB)));
        QVERIFY(!rig.engine.canConnect(room, rec(call), sink(HP)));

        // A network node's OUT is what other devices send: not recordable
        rig.engine.connectNodes(room, vout(pa), rec(call));
        rig.engine.configureVirtualSink(room, pa, {
            { QStringLiteral("networkEnabled"), true },
            { QStringLiteral("channel"), QStringLiteral("den") },
            { QStringLiteral("passphrase"), QStringLiteral("pw") },
        });
        QVERIFY(rig.groups.groupAt(room)->captures.isEmpty());
        QVERIFY(!rig.engine.canConnect(room, vout(pa), rec(call)));
        // ... but a microphone may go to the network
        QVERIFY(rig.engine.canConnect(room, mic(MIC), vin(pa)));
    }

    void removingThingsRemovesTheirRecordingWires()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        const QString pa = rig.engine.addVirtualSink(room, QStringLiteral("Call"));
        const uint32_t call = rig.backend.addRecorder(TEAMS);
        const uint32_t obs = rig.backend.addRecorder(QStringLiteral("OBS"));
        rig.engine.connectNodes(room, vout(pa), rec(call));
        rig.engine.connectNodes(room, mic(USB), rec(obs));
        rig.engine.connectNodes(room, mic(USB), vin(pa));

        rig.engine.disconnectNode(room, mic(USB));
        QVERIFY(!rig.hasWire(room, mic(USB), rec(obs)));
        QVERIFY(!rig.hasWire(room, mic(USB), vin(pa)));
        QVERIFY(rig.hasWire(room, vout(pa), rec(call)));

        rig.engine.connectNodes(room, mic(USB), vin(pa));
        rig.engine.disconnectNode(room, vin(pa));
        QVERIFY(rig.groups.groupAt(room)->virtualSink(pa)->inputSources.isEmpty());

        rig.engine.disconnectNode(room, rec(call));
        QVERIFY(rig.groups.groupAt(room)->captures.isEmpty());

        rig.engine.connectNodes(room, vout(pa), rec(call));
        rig.engine.removeVirtualSink(room, pa);
        QVERIFY(rig.groups.groupAt(room)->captures.isEmpty());
        QVERIFY(rig.engine.connections(room).isEmpty());
    }

    void recordingWiresSurviveARestart()
    {
        QTemporaryDir dir;
        Settings::instance().setFilePath(dir.filePath(QStringLiteral("groups.json")));
        QString pa;
        {
            FakeBackend backend;
            GroupListModel groups;
            groups.load();
            RoutingEngine engine(&backend, &groups);
            backend.addSink(SPK);
            backend.addSource(MIC);
            groups.addGroup(QStringLiteral("Calls"), QStringLiteral("#50c878"));
            pa = engine.addVirtualSink(0, QStringLiteral("Call"));
            const uint32_t call = backend.addRecorder(TEAMS);
            engine.connectNodes(0, mic(MIC), vin(pa));
            engine.connectNodes(0, vout(pa), rec(call));
            engine.activateRoom(0);
        }
        {
            FakeBackend backend;
            backend.nextIndex = 700;
            GroupListModel groups;
            groups.load();
            RoutingEngine engine(&backend, &groups);
            backend.addSink(SPK);
            backend.addSource(MIC);
            emit backend.serverConnected();

            QCOMPARE(backend.micInputs.value(pa), QStringList({ MIC }));
            QCOMPARE(engine.offlineRecorders(0).size(), 1);
            const uint32_t call = backend.addRecorder(TEAMS);
            QTRY_COMPARE(backend.sourceOf(call), virtualMicName(pa));
        }
        Settings::instance().setFilePath({});
    }

    void aRecorderThatIsMovedBackIsLeftAloneEventually()
    {
        Rig rig;
        const int room = rig.addRoom(QStringLiteral("Calls"));
        rig.engine.activateRoom(room);
        const uint32_t call = rig.backend.addRecorder(TEAMS);
        rig.backend.refuseMoves = true;
        rig.engine.connectNodes(room, mic(USB), rec(call));
        for (int i = 0; i < 20; ++i)
            rig.engine.reconcile();
        QVERIFY(rig.backend.recorderMoveCalls <= 6);
    }
};

QTEST_GUILESS_MAIN(TestRoutingEngine)
#include "tst_routingengine.moc"
