#include "models/sourceoutputmodel.h"
#include "models/listdiff.h"

#include <QHash>
#include <algorithm>

SourceOutputModel::SourceOutputModel(AudioBackend *backend, QObject *parent)
    : QAbstractListModel(parent)
    , m_backend(backend)
{
    connect(backend, &AudioBackend::sourceOutputsChanged, this, &SourceOutputModel::refresh);
    // A renamed or replaced source changes what the cards show
    connect(backend, &AudioBackend::sourcesChanged, this, &SourceOutputModel::refresh);
}

int SourceOutputModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant SourceOutputModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};

    const auto &row = m_rows[index.row()];
    switch (role) {
    case IndexRole:         return row.info.index;
    case AppNameRole:       return recorderDisplayName(row.info.appName);
    case MediaNameRole:     return row.info.mediaName;
    case IconNameRole:      return row.info.iconName;
    case CurrentSourceRole: return row.currentSource;
    case Qt::DisplayRole:   return recorderDisplayName(row.info.appName);
    }
    return {};
}

QHash<int, QByteArray> SourceOutputModel::roleNames() const
{
    return {
        { IndexRole,         "recorderIndex" },
        { AppNameRole,       "appName"       },
        { MediaNameRole,     "mediaName"     },
        { IconNameRole,      "iconName"      },
        { CurrentSourceRole, "currentSource" }
    };
}

void SourceOutputModel::refresh()
{
    QHash<uint32_t, QString> sourceLabel;
    const auto sources = m_backend->availableSources();
    for (const auto &s : sources)
        sourceLabel.insert(s.index, s.description);

    QVector<Row> updated;
    const auto all = m_backend->availableSourceOutputs();
    for (const auto &r : all)
        updated.append({ r, sourceLabel.value(r.sourceIndex) });

    // Same order rules as the application streams
    std::sort(updated.begin(), updated.end(), [](const Row &a, const Row &b) {
        const int byName = a.info.appName.compare(b.info.appName, Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.info.index < b.info.index;
    });

    const ListDiffOps ops {
        [this](int row) { beginInsertRows({}, row, row); },
        [this]() { endInsertRows(); },
        [this](int row) { beginRemoveRows({}, row, row); },
        [this]() { endRemoveRows(); },
        [this]() { beginResetModel(); },
        [this]() { endResetModel(); },
        [this](int row) { emit dataChanged(index(row), index(row)); },
    };
    const bool structural = applyListDiff(
        m_rows, updated,
        [](const Row &a, const Row &b) { return a.info.index == b.info.index; },
        [](const Row &a, const Row &b) {
            return a.info.appName == b.info.appName && a.info.mediaName == b.info.mediaName
                   && a.info.iconName == b.info.iconName && a.info.sourceIndex == b.info.sourceIndex
                   && a.currentSource == b.currentSource;
        },
        ops);
    if (structural)
        emit countChanged();
}
