#pragma once

#include "audiogroup.h"
#include <QHash>
#include <QVector>

/// Everything SoundRoot persists.
struct SettingsData {
    QVector<AudioGroup> groups;
    /// Sink name → last seen description, for sinks used by a room.  Lets the
    /// UI name a device that is currently unplugged.
    QHash<QString, QString> sinkLabels;
};

/**
 * @brief Loads and saves the configuration as JSON.
 *
 * Stored at ~/.config/audiorouter/groups.json.  Saving is atomic (a crash
 * mid-write keeps the previous file) and the file is private to the user,
 * because it holds network node passphrases.
 */
class Settings {
public:
    static Settings &instance();

    SettingsData load() const;
    /// Returns false when the file could not be written.
    bool save(const SettingsData &data) const;

    /// Use another file (tests).  An empty path restores the default.
    void setFilePath(const QString &path);
    QString filePath() const;

private:
    Settings() = default;
    QString m_overridePath;
};
