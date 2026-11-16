#pragma once

#include <QVector>
#include <functional>

/// The model operations applyListDiff() needs; a model fills these with
/// lambdas around its (protected) begin/end functions.
struct ListDiffOps {
    std::function<void(int row)> beginInsert;
    std::function<void()>        endInsert;
    std::function<void(int row)> beginRemove;
    std::function<void()>        endRemove;
    std::function<void()>        beginReset;
    std::function<void()>        endReset;
    std::function<void(int row)> changed;
};

/**
 * Bring @p current in line with @p updated using row inserts/removes instead
 * of a model reset, so views keep their delegates (and with them slider
 * positions, hover state and drags in progress).
 *
 * @p sameItem tells whether two entries are the same row; @p sameContent
 * whether a kept row is unchanged.  Falls back to a reset only when the kept
 * rows changed their relative order.
 *
 * @return true when rows were added, removed or reordered.
 */
template <typename T, typename SameItem, typename SameContent>
bool applyListDiff(QVector<T> &current, const QVector<T> &updated,
                   SameItem sameItem, SameContent sameContent, const ListDiffOps &ops)
{
    auto indexIn = [&sameItem](const QVector<T> &list, const T &item) {
        for (int i = 0; i < list.size(); ++i) {
            if (sameItem(list[i], item)) return i;
        }
        return -1;
    };

    bool structural = false;

    // 1. Remove rows that are gone (back to front keeps indices valid)
    for (int i = static_cast<int>(current.size()) - 1; i >= 0; --i) {
        if (indexIn(updated, current[i]) < 0) {
            ops.beginRemove(i);
            current.removeAt(i);
            ops.endRemove();
            structural = true;
        }
    }

    // 2. The survivors must appear in the same order in the new list
    int last = -1;
    for (const auto &item : std::as_const(current)) {
        const int pos = indexIn(updated, item);
        if (pos < last) {
            ops.beginReset();
            current = updated;
            ops.endReset();
            return true;
        }
        last = pos;
    }

    // 3. Insert new rows at their final position, refresh kept ones
    for (int i = 0; i < updated.size(); ++i) {
        if (i < current.size() && sameItem(current[i], updated[i])) {
            if (!sameContent(current[i], updated[i])) {
                current[i] = updated[i];
                ops.changed(i);
            }
            continue;
        }
        ops.beginInsert(i);
        current.insert(i, updated[i]);
        ops.endInsert();
        structural = true;
    }
    return structural;
}
