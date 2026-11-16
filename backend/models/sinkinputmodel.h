#pragma once

#include <QAbstractListModel>
#include "audiotypes.h"

/**
 * @brief Exposes the application audio streams (sink-inputs) to QML.
 *
 * These are the sources the user can wire to outputs.  Streams SoundRoot
 * creates itself (network node playback) are left out.  Updates are applied
 * row by row, so a stream appearing does not recreate the other cards.
 */
class SinkInputModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IndexRole    = Qt::UserRole + 1,
        AppNameRole,
        MediaNameRole,
        SinkIndexRole,
        IconNameRole,
        VolumePercentRole
    };

    explicit SinkInputModel(AudioBackend *backend, QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

public slots:
    void refresh();

private:
    AudioBackend             *m_backend;
    QVector<PASinkInputInfo>  m_sinkInputs;
};
