#include "models/routelistmodel.h"
#include "models/grouplistmodel.h"
#include <QtGlobal>

RouteListModel::RouteListModel(GroupListModel *groupModel, QObject *parent)
    : QAbstractListModel(parent)
    , m_groupModel(groupModel)
{
}

// ═══════════════════════════════════════════════════════════════════════════
// QAbstractListModel
// ═══════════════════════════════════════════════════════════════════════════

int RouteListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    auto *group = currentGroup();
    return group ? static_cast<int>(group->routes.size()) : 0;
}

QVariant RouteListModel::data(const QModelIndex &index, int role) const
{
    auto *group = currentGroup();
    if (!group || !index.isValid() || index.row() >= group->routes.size())
        return {};

    const auto &route = group->routes[index.row()];
    switch (role) {
    case SourceAppRole:   return route.sourceAppName;
    case OutputNamesRole: return route.outputSinkNames;
    case OutputCountRole: return static_cast<int>(route.outputSinkNames.size());
    case VolumeRole:      return route.volumePercent;
    case Qt::DisplayRole: return route.sourceAppName;
    }
    return {};
}

QHash<int, QByteArray> RouteListModel::roleNames() const
{
    return {
        { SourceAppRole,   "sourceApp"   },
        { OutputNamesRole, "outputNames" },
        { OutputCountRole, "outputCount" },
        { VolumeRole,      "volumePercent" }
    };
}

// ═══════════════════════════════════════════════════════════════════════════
// Properties
// ═══════════════════════════════════════════════════════════════════════════

void RouteListModel::setGroupId(const QString &id)
{
    if (m_groupId == id) return;
    beginResetModel();
    m_groupId = id;
    endResetModel();
    emit groupIdChanged();
    emit groupNameChanged();
    emit countChanged();
}

QString RouteListModel::groupName() const
{
    auto *group = currentGroup();
    return group ? group->name : QString();
}

// ═══════════════════════════════════════════════════════════════════════════
// Mutations
// ═══════════════════════════════════════════════════════════════════════════

void RouteListModel::addRoute(const QString &sourceApp,
                              const QStringList &outputSinkNames)
{
    auto *group = currentGroup();
    if (!group) return;

    const int n = static_cast<int>(group->routes.size());
    beginInsertRows({}, n, n);

    AudioRoute route;
    route.sourceAppName   = sourceApp;
    route.outputSinkNames = outputSinkNames;
    route.volumePercent   = 100;
    group->routes.append(route);

    endInsertRows();
    emit countChanged();
    m_groupModel->save();
}

void RouteListModel::setRouteVolume(int row, int volumePercent)
{
    auto *group = currentGroup();
    if (!group || row < 0 || row >= group->routes.size()) return;

    const int clamped = qBound(0, volumePercent, 150);
    auto &route = group->routes[row];
    if (route.volumePercent == clamped) return;

    route.volumePercent = clamped;
    emit dataChanged(index(row), index(row), { VolumeRole });
    m_groupModel->save();
}

void RouteListModel::removeRoute(int row)
{
    auto *group = currentGroup();
    if (!group || row < 0 || row >= group->routes.size()) return;

    beginRemoveRows({}, row, row);
    group->routes.removeAt(row);
    endRemoveRows();
    emit countChanged();
    m_groupModel->save();
}

void RouteListModel::addOutputToRoute(int row, const QString &sinkName)
{
    auto *group = currentGroup();
    if (!group || row < 0 || row >= group->routes.size()) return;

    auto &route = group->routes[row];
    if (!route.outputSinkNames.contains(sinkName)) {
        route.outputSinkNames.append(sinkName);
        emit dataChanged(index(row), index(row), { OutputNamesRole, OutputCountRole });
        m_groupModel->save();
    }
}

void RouteListModel::removeOutputFromRoute(int row, int outputIndex)
{
    auto *group = currentGroup();
    if (!group || row < 0 || row >= group->routes.size()) return;

    auto &route = group->routes[row];
    if (outputIndex >= 0 && outputIndex < route.outputSinkNames.size()) {
        route.outputSinkNames.removeAt(outputIndex);
        emit dataChanged(index(row), index(row), { OutputNamesRole, OutputCountRole });
        m_groupModel->save();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Internals
// ═══════════════════════════════════════════════════════════════════════════

AudioGroup *RouteListModel::currentGroup()
{
    return m_groupModel->groupById(m_groupId);
}

const AudioGroup *RouteListModel::currentGroup() const
{
    return const_cast<GroupListModel *>(m_groupModel)->groupById(m_groupId);
}
