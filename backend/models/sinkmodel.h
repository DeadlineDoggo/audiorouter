#pragma once

#include <QAbstractListModel>
#include "audiotypes.h"

/**
 * @brief Exposes the output devices (sinks) to QML.
 *
 * Sinks SoundRoot creates itself (virtual nodes, combine sinks) are left out:
 * virtual nodes have their own column, combine sinks are an implementation
 * detail.
 */
class SinkModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IndexRole       = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        VolumeRole,
        MutedRole,
        IsDefaultRole
    };

    explicit SinkModel(AudioBackend *backend, QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

public slots:
    void refresh();

private:
    AudioBackend        *m_backend;
    QVector<PASinkInfo>  m_sinks;
    QString              m_defaultSink;
};
