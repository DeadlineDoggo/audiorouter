#pragma once

#include <QAbstractListModel>
#include "data/audiogroup.h"

/**
 * @brief Qt list model exposing audio groups to QML.
 *
 * Supports add / remove / toggle-active operations and persists changes
 * automatically via Settings.
 */
class GroupListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        ColorRole,
        IconRole,
        ActiveRole,
        RouteCountRole
    };

    explicit GroupListModel(QObject *parent = nullptr);

    // ── QAbstractListModel ──────────────────────────────────────────────
    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ── Invokable from QML ──────────────────────────────────────────────
    Q_INVOKABLE void    addGroup(const QString &name, const QString &color, const QString &icon = QStringLiteral("audio-card"));
    Q_INVOKABLE void    removeGroup(int row);
    Q_INVOKABLE void    moveGroup(int from, int to);
    Q_INVOKABLE void    toggleActive(int row);
    Q_INVOKABLE QString groupId(int row) const;
    Q_INVOKABLE bool    isGroupActive(int row) const;
    Q_INVOKABLE void    updateGroup(int row, const QString &name, const QString &color, const QString &icon);

    /// Set active without toggle; emits dataChanged from within the model.
    void setGroupActive(int row, bool active);

    // ── C++ convenience ─────────────────────────────────────────────────
    AudioGroup       *groupAt(int row);
    const AudioGroup *groupAt(int row) const;
    AudioGroup       *groupById(const QString &id);

    void load();
    void save();

    QVector<AudioGroup> &groups() { return m_groups; }

signals:
    void countChanged();
    void groupChanged(int row);

private:
    QVector<AudioGroup> m_groups;
};
