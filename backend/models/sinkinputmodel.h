#pragma once

#include <QAbstractListModel>
#include "audiobackend.h"

/**
 * @brief Exposes the list of PulseAudio sink-inputs (application audio streams) to QML.
 *
 * These are the "source" applications the user can route to output devices.
 * Refreshes automatically when PulseAudioBackend emits sinkInputsChanged().
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
        IconNameRole
    };

    explicit SinkInputModel(PulseAudioBackend *backend, QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString appNameAt(int row) const;

signals:
    void countChanged();

public slots:
    void refresh();

private:
    PulseAudioBackend        *m_backend;
    QVector<PASinkInputInfo>  m_sinkInputs;
};
