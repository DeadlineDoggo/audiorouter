#include <QTest>

#include "models/listdiff.h"

// Counts what a model would be told to do.
struct Recorder {
    int inserts = 0, removes = 0, resets = 0, changes = 0;
    ListDiffOps ops()
    {
        return {
            [this](int) { ++inserts; }, []() {},
            [this](int) { ++removes; }, []() {},
            [this]() { ++resets; }, []() {},
            [this](int) { ++changes; },
        };
    }
};

struct Item {
    int id;
    int value;
};

static bool sameItem(const Item &a, const Item &b) { return a.id == b.id; }
static bool sameContent(const Item &a, const Item &b) { return a.value == b.value; }

static QVector<int> ids(const QVector<Item> &list)
{
    QVector<int> out;
    for (const auto &i : list) out.append(i.id);
    return out;
}

class TestListDiff : public QObject {
    Q_OBJECT

private slots:
    void appendKeepsExistingRows()
    {
        QVector<Item> cur { { 1, 0 }, { 2, 0 } };
        Recorder rec;
        const bool structural = applyListDiff(cur, { { 1, 0 }, { 2, 0 }, { 3, 0 } }, sameItem, sameContent, rec.ops());
        QVERIFY(structural);
        QCOMPARE(ids(cur), QVector<int>({ 1, 2, 3 }));
        QCOMPARE(rec.inserts, 1);
        QCOMPARE(rec.removes, 0);
        QCOMPARE(rec.resets, 0);
    }

    void removeFromMiddle()
    {
        QVector<Item> cur { { 1, 0 }, { 2, 0 }, { 3, 0 } };
        Recorder rec;
        applyListDiff(cur, { { 1, 0 }, { 3, 0 } }, sameItem, sameContent, rec.ops());
        QCOMPARE(ids(cur), QVector<int>({ 1, 3 }));
        QCOMPARE(rec.removes, 1);
        QCOMPARE(rec.resets, 0);
    }

    void insertAndRemoveTogether()
    {
        QVector<Item> cur { { 1, 0 }, { 2, 0 }, { 3, 0 } };
        Recorder rec;
        applyListDiff(cur, { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 5, 0 } }, sameItem, sameContent, rec.ops());
        QCOMPARE(ids(cur), QVector<int>({ 0, 2, 4, 5 }));
        QCOMPARE(rec.removes, 2);
        QCOMPARE(rec.inserts, 3);
        QCOMPARE(rec.resets, 0);
    }

    void contentChangeOnly()
    {
        QVector<Item> cur { { 1, 0 }, { 2, 0 } };
        Recorder rec;
        const bool structural = applyListDiff(cur, { { 1, 0 }, { 2, 7 } }, sameItem, sameContent, rec.ops());
        QVERIFY(!structural);
        QCOMPARE(cur[1].value, 7);
        QCOMPARE(rec.changes, 1);
        QCOMPARE(rec.inserts + rec.removes + rec.resets, 0);
    }

    void reorderFallsBackToReset()
    {
        QVector<Item> cur { { 1, 0 }, { 2, 0 }, { 3, 0 } };
        Recorder rec;
        applyListDiff(cur, { { 3, 0 }, { 1, 0 }, { 2, 0 } }, sameItem, sameContent, rec.ops());
        QCOMPARE(ids(cur), QVector<int>({ 3, 1, 2 }));
        QCOMPARE(rec.resets, 1);
    }

    void toEmptyAndBack()
    {
        QVector<Item> cur { { 1, 0 } };
        Recorder rec;
        applyListDiff(cur, {}, sameItem, sameContent, rec.ops());
        QVERIFY(cur.isEmpty());
        applyListDiff(cur, { { 9, 1 } }, sameItem, sameContent, rec.ops());
        QCOMPARE(ids(cur), QVector<int>({ 9 }));
        QCOMPARE(rec.resets, 0);
    }
};

QTEST_GUILESS_MAIN(TestListDiff)
#include "tst_listdiff.moc"
