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
    case VolumeRole:      return sink.volumePercent;
    case MutedRole:       return sink.isMuted;
    case Qt::DisplayRole: return sink.description;
    }
    return {};
}

QHash<int, QByteArray> SinkModel::roleNames() const
{
    return {
        { IndexRole,       "sinkIndex"         },
        { NameRole,        "sinkName"           },
        { DescriptionRole, "sinkDescription"   },
        { VolumeRole,      "sinkVolumePercent" },
        { MutedRole,       "sinkMuted"         }
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
    auto rawSinks = m_backend->availableSinks();
    // Strip internal combine sinks from the display list; they stay in the
    // backend's m_sinks so routing lookups can still find them.
    QVector<PASinkInfo> newSinks;
    for (const auto &s : rawSinks) {
        if (!s.name.startsWith(QStringLiteral("audiorouter_")))
            newSinks.append(s);
    }

    // Fast path: same count, same sinks → check for property-only changes
    if (newSinks.size() == m_sinks.size()) {
        bool structurallyEqual = true;
        for (int i = 0; i < newSinks.size(); ++i) {
            if (newSinks[i].index != m_sinks[i].index ||
                newSinks[i].name  != m_sinks[i].name) {
                structurallyEqual = false;
                break;
            }
        }
        if (structurallyEqual) {
            for (int i = 0; i < newSinks.size(); ++i) {
                const auto &o = m_sinks[i];
                const auto &n = newSinks[i];
                if (o.volumePercent != n.volumePercent || o.isMuted != n.isMuted ||
                    o.description != n.description) {
                    m_sinks[i] = n;
                    emit dataChanged(index(i), index(i));
                }
            }
            return;
        }
    }

    // Structural change — full reset
    beginResetModel();
    m_sinks = std::move(newSinks);
    endResetModel();
}
