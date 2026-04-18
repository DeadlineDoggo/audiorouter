#include "models/grouplistmodel.h"
#include "data/settings.h"

GroupListModel::GroupListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

// ═══════════════════════════════════════════════════════════════════════════
// QAbstractListModel interface
// ═══════════════════════════════════════════════════════════════════════════

int GroupListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_groups.size());
}

QVariant GroupListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_groups.size()) return {};

    const auto &group = m_groups[index.row()];
    switch (role) {
    case IdRole:         return group.id;
    case NameRole:       return group.name;
    case ColorRole:      return group.color;
    case IconRole:       return group.icon;
    case ActiveRole:     return group.active;
    case RouteCountRole: return static_cast<int>(group.routes.size());
    case Qt::DisplayRole: return group.name;
    }
    return {};
}

QHash<int, QByteArray> GroupListModel::roleNames() const
{
    return {
        { IdRole,         "groupId"    },
        { NameRole,       "groupName"  },
        { ColorRole,      "groupColor" },
        { IconRole,       "groupIcon"  },
        { ActiveRole,     "groupActive"},
        { RouteCountRole, "routeCount" }
    };
}

// ═══════════════════════════════════════════════════════════════════════════
// QML-callable operations
// ═══════════════════════════════════════════════════════════════════════════

void GroupListModel::addGroup(const QString &name, const QString &color, const QString &icon)
{
    const int n = static_cast<int>(m_groups.size());
    beginInsertRows({}, n, n);
    m_groups.append(AudioGroup(name, color, icon));
    endInsertRows();
    emit countChanged();
    save();
}

void GroupListModel::removeGroup(int row)
{
    if (row < 0 || row >= m_groups.size()) return;
    beginRemoveRows({}, row, row);
    m_groups.removeAt(row);
    endRemoveRows();
    emit countChanged();
    save();
}

void GroupListModel::updateGroup(int row, const QString &name, const QString &color, const QString &icon)
{
    if (row < 0 || row >= m_groups.size()) return;
    m_groups[row].name  = name;
    m_groups[row].color = color;
    m_groups[row].icon  = icon.isEmpty() ? QStringLiteral("audio-card") : icon;
    emit dataChanged(index(row), index(row), { NameRole, ColorRole, IconRole });
    emit groupChanged(row);
    save();
}

void GroupListModel::moveGroup(int from, int to)
{
    if (from < 0 || from >= m_groups.size()) return;
    if (to < 0 || to >= m_groups.size()) return;
    if (from == to) return;
    // beginMoveRows destinationChild: row *before* which items are inserted
    const int dest = to > from ? to + 1 : to;
    if (!beginMoveRows({}, from, from, {}, dest)) return;
    m_groups.move(from, to);
    endMoveRows();
    save();
}

void GroupListModel::toggleActive(int row)
{
    if (row < 0 || row >= m_groups.size()) return;
    m_groups[row].active = !m_groups[row].active;
    emit dataChanged(index(row), index(row), { ActiveRole });
    emit groupChanged(row);
    save();
}

void GroupListModel::setGroupActive(int row, bool active)
{
    if (row < 0 || row >= m_groups.size()) return;
    if (m_groups[row].active == active) return;
    m_groups[row].active = active;
    emit dataChanged(index(row), index(row), { ActiveRole });
    save();
}

QString GroupListModel::groupId(int row) const
{
    if (row < 0 || row >= m_groups.size()) return {};
    return m_groups[row].id;
}

bool GroupListModel::isGroupActive(int row) const
{
    if (row < 0 || row >= m_groups.size()) return false;
    return m_groups[row].active;
}

// ═══════════════════════════════════════════════════════════════════════════
// C++ helpers
// ═══════════════════════════════════════════════════════════════════════════

AudioGroup *GroupListModel::groupAt(int row)
{
    if (row < 0 || row >= m_groups.size()) return nullptr;
    return &m_groups[row];
}

const AudioGroup *GroupListModel::groupAt(int row) const
{
    if (row < 0 || row >= m_groups.size()) return nullptr;
    return &m_groups[row];
}

AudioGroup *GroupListModel::groupById(const QString &id)
{
    for (auto &group : m_groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

void GroupListModel::load()
{
    beginResetModel();
    m_groups = Settings::instance().loadGroups();
    endResetModel();
    emit countChanged();
}

void GroupListModel::save()
{
    Settings::instance().saveGroups(m_groups);
}
