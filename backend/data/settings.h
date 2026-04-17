#pragma once

#include "audiogroup.h"
#include <QVector>

/**
 * @brief Singleton for loading/saving audio groups to a JSON config file.
 *
 * Config is stored at ~/.config/audiorouter/groups.json
 */
class Settings {
public:
    static Settings &instance();

    QVector<AudioGroup> loadGroups() const;
    void saveGroups(const QVector<AudioGroup> &groups) const;

private:
    Settings() = default;
    QString configFilePath() const;
};
