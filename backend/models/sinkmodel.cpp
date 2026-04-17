#include "models/sinkmodel.h"

SinkModel::SinkModel(PulseAudioBackend *backend, QObject *parent)
    : QAbstractListModel(parent)
    , m_backend(backend)
{
    connect(backend, &PulseAudioBackend::sinksChanged,
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
    case Qt::DisplayRole: return sink.description;
    }
    return {};
}

QHash<int, QByteArray> SinkModel::roleNames() const
{
    return {
        { IndexRole,       "sinkIndex"       },
        { NameRole,        "sinkName"        },
        { DescriptionRole, "sinkDescription" }
    };
}

QString SinkModel::sinkNameAt(int row) const
{
    if (row < 0 || row >= m_sinks.size()) return {};
    return m_sinks[row].name;
}

QString SinkModel::sinkDescriptionAt(int row) const
{
    if (row < 0 || row >= m_sinks.size()) return {};
    return m_sinks[row].description;
}

void SinkModel::refresh()
{
    beginResetModel();
    m_sinks = m_backend->availableSinks();
    endResetModel();
}
