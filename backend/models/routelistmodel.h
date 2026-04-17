#pragma once

#include <QAbstractListModel>
#include "data/audiogroup.h"

class GroupListModel;

/**
 * @brief Model for the routes inside one AudioGroup, driven by a groupId property.
 *
 * Setting `groupId` switches which group the model reflects. Any mutations
 * are automatically persisted through the owning GroupListModel.
 */
class RouteListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString groupId   READ groupId   WRITE setGroupId NOTIFY groupIdChanged)
    Q_PROPERTY(QString groupName READ groupName NOTIFY groupNameChanged)
    Q_PROPERTY(int     count     READ rowCount  NOTIFY countChanged)

public:
    enum Roles {
        SourceAppRole = Qt::UserRole + 1,
        OutputNamesRole,
        OutputCountRole,
        VolumeRole
    };

    explicit RouteListModel(GroupListModel *groupModel, QObject *parent = nullptr);

    // ── QAbstractListModel ──────────────────────────────────────────────
    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ── Properties ──────────────────────────────────────────────────────
    QString groupId()   const { return m_groupId; }
    void    setGroupId(const QString &id);
    QString groupName() const;

    // ── Invokable from QML ──────────────────────────────────────────────
    Q_INVOKABLE void addRoute(const QString &sourceApp,
                              const QStringList &outputSinkNames);
    Q_INVOKABLE void removeRoute(int row);
    Q_INVOKABLE void addOutputToRoute(int row, const QString &sinkName);
    Q_INVOKABLE void removeOutputFromRoute(int row, int outputIndex);
    Q_INVOKABLE void setRouteVolume(int row, int volumePercent);

signals:
    void groupIdChanged();
    void groupNameChanged();
    void countChanged();

private:
    AudioGroup       *currentGroup();
    const AudioGroup *currentGroup() const;

    GroupListModel *m_groupModel;
    QString         m_groupId;
};
