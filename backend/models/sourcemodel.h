#pragma once

#include <QAbstractListModel>
#include "audiotypes.h"

/**
 * @brief Exposes the microphones (sources) to QML.
 *
 * Monitors are never listed (the backend leaves them out), and the virtual
 * nodes' microphones are not either: a node is wired by its OUT side.
 */
class SourceModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IndexRole       = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        IsDefaultRole
    };

    explicit SourceModel(AudioBackend *backend, QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

public slots:
    void refresh();

private:
    AudioBackend         *m_backend;
    QVector<PASourceInfo> m_sources;
    QString               m_defaultSource;
};
