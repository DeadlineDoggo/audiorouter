#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

Settings &Settings::instance()
{
    static Settings s;
    return s;
}

QString Settings::configFilePath() const
{
    const QString configDir =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/audiorouter");
    QDir().mkpath(configDir);
    return configDir + QStringLiteral("/groups.json");
}

QVector<AudioGroup> Settings::loadGroups() const
{
    QVector<AudioGroup> groups;

    QFile file(configFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return groups;

    const auto doc   = QJsonDocument::fromJson(file.readAll());
    const auto array = doc.object()[QStringLiteral("groups")].toArray();

    for (const auto &val : array)
        groups.append(AudioGroup::fromJson(val.toObject()));

    return groups;
}

void Settings::saveGroups(const QVector<AudioGroup> &groups) const
{
    QJsonArray array;
    for (const auto &group : groups)
        array.append(group.toJson());

    QJsonObject root;
    root[QStringLiteral("groups")] = array;

    QFile file(configFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}
