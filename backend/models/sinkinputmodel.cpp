#include "models/sinkinputmodel.h"
#include "models/listdiff.h"

#include <algorithm>

SinkInputModel::SinkInputModel(AudioBackend *backend, QObject *parent)
    : QAbstractListModel(parent)
    , m_backend(backend)
{
    connect(backend, &AudioBackend::sinkInputsChanged,
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
    case IndexRole:        return si.index;
    case AppNameRole:      return si.appName;
    case MediaNameRole:    return si.mediaName;
    case SinkIndexRole:    return si.sinkIndex;
    case IconNameRole:     return si.iconName;
    case VolumePercentRole: return si.volumePercent;
    case Qt::DisplayRole:  return si.appName;
    }
    return {};
}

QHash<int, QByteArray> SinkInputModel::roleNames() const
{
    return {
        { IndexRole,          "inputIndex"       },
        { AppNameRole,        "appName"          },
        { MediaNameRole,      "mediaName"        },
        { SinkIndexRole,      "currentSinkIndex" },
        { IconNameRole,       "iconName"         },
        { VolumePercentRole,  "volumePercent"    }
    };
}

void SinkInputModel::refresh()
{
    QVector<PASinkInputInfo> updated;
    const auto all = m_backend->availableSinkInputs();
    for (const auto &si : all) {
        if (si.netNode.isEmpty())   // our own network playback streams are not apps
            updated.append(si);
    }

    // Streams of one application next to each other, oldest first: a stable
    // order, whatever order the server happens to list them in.
    std::sort(updated.begin(), updated.end(),
              [](const PASinkInputInfo &a, const PASinkInputInfo &b) {
                  const int byName = a.appName.compare(b.appName, Qt::CaseInsensitive);
                  return byName != 0 ? byName < 0 : a.index < b.index;
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
        m_sinkInputs, updated,
        [](const PASinkInputInfo &a, const PASinkInputInfo &b) { return a.index == b.index; },
        [](const PASinkInputInfo &a, const PASinkInputInfo &b) {
            return a.appName == b.appName && a.mediaName == b.mediaName &&
                   a.sinkIndex == b.sinkIndex && a.iconName == b.iconName &&
                   a.volumePercent == b.volumePercent && a.channels == b.channels;
        },
        ops);
    if (structural)
        emit countChanged();
}
