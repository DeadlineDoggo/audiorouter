#include "models/sourcemodel.h"
#include "models/listdiff.h"

SourceModel::SourceModel(AudioBackend *backend, QObject *parent)
    : QAbstractListModel(parent)
    , m_backend(backend)
{
    connect(backend, &AudioBackend::sourcesChanged, this, &SourceModel::refresh);
}

int SourceModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_sources.size());
}

QVariant SourceModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_sources.size()) return {};

    const auto &source = m_sources[index.row()];
    switch (role) {
    case IndexRole:       return source.index;
    case NameRole:        return source.name;
    case DescriptionRole: return source.description;
    case IsDefaultRole:   return source.name == m_defaultSource;
    case Qt::DisplayRole: return source.description;
    }
    return {};
}

QHash<int, QByteArray> SourceModel::roleNames() const
{
    return {
        { IndexRole,       "sourceIndex"       },
        { NameRole,        "sourceName"        },
        { DescriptionRole, "sourceDescription" },
        { IsDefaultRole,   "isDefault"         }
    };
}

void SourceModel::refresh()
{
    QVector<PASourceInfo> updated;
    const auto all = m_backend->availableSources();
    for (const auto &s : all) {
        if (!isInternalSourceName(s.name))
            updated.append(s);
    }

    const QString newDefault = m_backend->defaultSourceName();
    const bool defaultChanged = newDefault != m_defaultSource;
    m_defaultSource = newDefault;

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
        m_sources, updated,
        [](const PASourceInfo &a, const PASourceInfo &b) { return a.name == b.name; },
        [](const PASourceInfo &a, const PASourceInfo &b) {
            return a.index == b.index && a.description == b.description;
        },
        ops);
    if (structural)
        emit countChanged();
    if (defaultChanged && !m_sources.isEmpty())
        emit dataChanged(index(0), index(static_cast<int>(m_sources.size()) - 1), { IsDefaultRole });
}
