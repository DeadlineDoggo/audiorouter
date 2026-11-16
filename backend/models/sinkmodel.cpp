#include "models/sinkmodel.h"
#include "models/listdiff.h"

SinkModel::SinkModel(AudioBackend *backend, QObject *parent)
    : QAbstractListModel(parent)
    , m_backend(backend)
{
    connect(backend, &AudioBackend::sinksChanged,
            this, &SinkModel::refresh);
}

int SinkModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_sinks.size());
}

QVariant SinkModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_sinks.size()) return {};

    const auto &sink = m_sinks[index.row()];
    switch (role) {
    case IndexRole:       return sink.index;
    case NameRole:        return sink.name;
    case DescriptionRole: return sink.description;
    case VolumeRole:      return sink.volumePercent;
    case MutedRole:       return sink.isMuted;
    case IsDefaultRole:   return sink.name == m_defaultSink;
    case Qt::DisplayRole: return sink.description;
    }
    return {};
}

QHash<int, QByteArray> SinkModel::roleNames() const
{
    return {
        { IndexRole,       "sinkIndex"         },
        { NameRole,        "sinkName"          },
        { DescriptionRole, "sinkDescription"   },
        { VolumeRole,      "sinkVolumePercent" },
        { MutedRole,       "sinkMuted"         },
        { IsDefaultRole,   "isDefault"         }
    };
}

void SinkModel::refresh()
{
    QVector<PASinkInfo> updated;
    const auto all = m_backend->availableSinks();
    for (const auto &s : all) {
        if (!isInternalSinkName(s.name))
            updated.append(s);
    }

    const QString newDefault = m_backend->defaultSinkName();
    const bool defaultChanged = newDefault != m_defaultSink;
    m_defaultSink = newDefault;

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
        m_sinks, updated,
        [](const PASinkInfo &a, const PASinkInfo &b) { return a.name == b.name; },
        [](const PASinkInfo &a, const PASinkInfo &b) {
            return a.index == b.index && a.description == b.description &&
                   a.volumePercent == b.volumePercent && a.isMuted == b.isMuted &&
                   a.channels == b.channels;
        },
        ops);
    if (structural)
        emit countChanged();
    if (defaultChanged && !m_sinks.isEmpty())
        emit dataChanged(index(0), index(static_cast<int>(m_sinks.size()) - 1), { IsDefaultRole });
}
