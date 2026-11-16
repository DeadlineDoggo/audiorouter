#include "settings.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

Settings &Settings::instance()
{
    static Settings s;
    return s;
}

void Settings::setFilePath(const QString &path)
{
    m_overridePath = path;
}

QString Settings::filePath() const
{
    if (!m_overridePath.isEmpty())
        return m_overridePath;
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + QStringLiteral("/audiorouter/groups.json");
}

SettingsData Settings::load() const
{
    SettingsData data;

    const QString path = filePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return data;   // first run

    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Never start from an empty list over an unreadable file: the next
        // save would silently destroy every room.  Keep a copy to recover.
        if (!raw.trimmed().isEmpty()) {
            const QString backup = path + QStringLiteral(".corrupt-")
                + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
            if (QFile::copy(path, backup))
                qWarning() << "SoundRoot: could not parse" << path << "(" << err.errorString()
                           << ") - kept a copy at" << backup;
        }
        return data;
    }

    const QJsonObject root = doc.object();
    const auto array = root[QStringLiteral("groups")].toArray();
    bool activeSeen = false;
    for (const auto &val : array) {
        AudioGroup group = AudioGroup::fromJson(val.toObject());
        // Only one room can be active; keep the first one marked so
        if (group.active) {
            if (activeSeen) group.active = false;
            activeSeen = true;
        }
        data.groups.append(group);
    }

    const QJsonObject labels = root[QStringLiteral("sinkLabels")].toObject();
    for (auto it = labels.constBegin(); it != labels.constEnd(); ++it)
        data.sinkLabels.insert(it.key(), it.value().toString());

    return data;
}

bool Settings::save(const SettingsData &data) const
{
    QJsonArray array;
    for (const auto &group : data.groups)
        array.append(group.toJson());

    QJsonObject labels;
    for (auto it = data.sinkLabels.constBegin(); it != data.sinkLabels.constEnd(); ++it)
        labels.insert(it.key(), it.value());

    QJsonObject root;
    root[QStringLiteral("version")] = 2;
    root[QStringLiteral("groups")] = array;
    root[QStringLiteral("sinkLabels")] = labels;

    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    // QSaveFile writes to a temporary file and renames it over the target,
    // so a crash or a full disk never leaves a truncated configuration.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "SoundRoot: cannot write" << path << ":" << file.errorString();
        return false;
    }
    // The file contains network node passphrases
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        qWarning() << "SoundRoot: cannot write" << path << ":" << file.errorString();
        return false;
    }
    return true;
}
