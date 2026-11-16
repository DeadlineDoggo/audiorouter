#pragma once

#include <QAbstractListModel>
#include <QHash>
#include "data/audiogroup.h"

/**
 * @brief Qt list model exposing the rooms to QML, and owner of the persisted
 *        configuration.
 *
 * Rooms are edited through this model (name, colour, order) and through the
 * routing engine (wires, virtual nodes), which calls save() and
 * notifyLinksChanged() after changing a room.
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
        LinkCountRole
    };

    explicit GroupListModel(QObject *parent = nullptr);

    // ── QAbstractListModel ──────────────────────────────────────────────
    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ── Invokable from QML ──────────────────────────────────────────────
    Q_INVOKABLE void    addGroup(const QString &name, const QString &color, const QString &icon = QStringLiteral("audio-card"));
    Q_INVOKABLE void    moveGroup(int from, int to);
    Q_INVOKABLE QString groupId(int row) const;
    Q_INVOKABLE QString groupName(int row) const;
    Q_INVOKABLE QString groupColor(int row) const;
    Q_INVOKABLE bool    isGroupActive(int row) const;
    Q_INVOKABLE void    updateGroup(int row, const QString &name, const QString &color, const QString &icon);

    // ── C++ only ────────────────────────────────────────────────────────
    /// Rooms are removed through the applet, which deactivates them first.
    void removeGroup(int row);
    void setGroupActive(int row, bool active);
    /// Row of the active room, or -1.
    int  activeRow() const;
    int  rowOfId(const QString &id) const;

    AudioGroup       *groupAt(int row);
    const AudioGroup *groupAt(int row) const;
    AudioGroup       *groupById(const QString &id);

    /// A room's wires changed: refresh its link count in the views.
    void notifyLinksChanged(int row);

    /// Remember a sink's description so it can be named while unplugged.
    /// Returns true when the stored label changed (the caller saves).
    bool    rememberSinkLabel(const QString &sinkName, const QString &description);
    QString sinkLabel(const QString &sinkName) const;

    void load();
    void save();

signals:
    void countChanged();

private:
    QVector<AudioGroup>     m_groups;
    QHash<QString, QString> m_sinkLabels;
};
