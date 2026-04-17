#include "models/sinkinputmodel.h"

SinkInputModel::SinkInputModel(PulseAudioBackend *backend, QObject *parent)
    : QAbstractListModel(parent)
    , m_backend(backend)
{
    connect(backend, &PulseAudioBackend::sinkInputsChanged,
            this, &SinkInputModel::refresh);
}

int SinkInputModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_sinkInputs.size());
}

QVariant SinkInputModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_sinkInputs.size()) return {};

    const auto &si = m_sinkInputs[index.row()];
    switch (role) {
    case IndexRole:     return si.index;
    case AppNameRole:   return si.appName;
    case MediaNameRole: return si.mediaName;
    case SinkIndexRole: return si.sinkIndex;
    case IconNameRole:  return si.iconName;
    case Qt::DisplayRole: return si.appName;
    }
    return {};
}

QHash<int, QByteArray> SinkInputModel::roleNames() const
{
    return {
        { IndexRole,     "inputIndex"       },
        { AppNameRole,   "appName"          },
        { MediaNameRole, "mediaName"        },
        { SinkIndexRole, "currentSinkIndex" },
        { IconNameRole,  "iconName"         }
    };
}

QString SinkInputModel::appNameAt(int row) const
{
    if (row < 0 || row >= m_sinkInputs.size()) return {};
    return m_sinkInputs[row].appName;
}

void SinkInputModel::refresh()
{
    beginResetModel();
    m_sinkInputs = m_backend->availableSinkInputs();
    endResetModel();
}
