#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QUuid>

/**
 * @brief A single audio route: one source application routed to one or more output sinks.
 *
 * Sources are identified by application name (pattern-matched against active PulseAudio
 * sink-inputs). A single source can appear in multiple groups — sources are NOT unique
 * to a group.
 */
struct AudioRoute {
    QString sourceAppName;          ///< Application name (e.g. "Firefox", "Discord")
    QStringList outputSinkNames;    ///< PulseAudio sink names for outputs
    int volumePercent = 100;        ///< Route volume (0-150) applied to matching stream

    QJsonObject toJson() const;
    static AudioRoute fromJson(const QJsonObject &obj);
};

/**
 * @brief A named group of audio routes.
 *
 * Each group is a logical "audio channel" that bundles one or more routes.
 * Groups can be activated/deactivated; when active, their routes are applied
 * to PulseAudio via combined sinks.
 */
struct AudioGroup {
    QString id;
    QString name;
    QString color;
    bool active = false;
    QVector<AudioRoute> routes;

    AudioGroup();
    explicit AudioGroup(const QString &name, const QString &color = QStringLiteral("#4a90d9"));

    QJsonObject toJson() const;
    static AudioGroup fromJson(const QJsonObject &obj);
};
