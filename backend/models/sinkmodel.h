#pragma once

#include <QAbstractListModel>
#include "audiobackend.h"

/**
 * @brief Exposes the list of PulseAudio sinks (output devices) to QML.
 *
 * Refreshes automatically when PulseAudioBackend emits sinksChanged().
 */
class SinkModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        IndexRole       = Qt::UserRole + 1,
        NameRole,
        DescriptionRole
    };

    explicit SinkModel(PulseAudioBackend *backend, QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString sinkNameAt(int row) const;
    Q_INVOKABLE QString sinkDescriptionAt(int row) const;

public slots:
    void refresh();

private:
    PulseAudioBackend   *m_backend;
    QVector<PASinkInfo>  m_sinks;
};
