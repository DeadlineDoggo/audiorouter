#pragma once

#include <QAbstractListModel>
#include "audiotypes.h"

/**
 * @brief Exposes the applications that record (source-outputs) to QML: calls,
 *        recorders, anything that uses a microphone.
 *
 * Each row also names the source the application records right now, so the
 * card can show where its microphone input really comes from.
 */
class SourceOutputModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IndexRole    = Qt::UserRole + 1,
        AppNameRole,
        MediaNameRole,
        IconNameRole,
        CurrentSourceRole
    };

    explicit SourceOutputModel(AudioBackend *backend, QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

public slots:
    void refresh();

private:
    struct Row {
        PASourceOutputInfo info;
        QString            currentSource;   ///< Description of the source it records
    };

    AudioBackend *m_backend;
    QVector<Row>  m_rows;
};
