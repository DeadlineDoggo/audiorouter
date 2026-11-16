#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QUuid>

/**
 * @brief Network settings of a virtual node.
 *
 * When enabled, the node becomes a shared audio channel: every SoundRoot
 * instance on the local network that has a node with the same channel name
 * and passphrase hears what the others play into it.
 */
struct NetworkNodeConfig {
    bool        enabled = false;
    QString     channel;      ///< Channel name shared by all devices
    QString     passphrase;   ///< Shared secret; the encryption key is derived from it
    QStringList staticPeers;  ///< Optional "host" or "host:port" entries, for networks without multicast

    bool operator==(const NetworkNodeConfig &other) const = default;

    QJsonObject toJson() const;
    static NetworkNodeConfig fromJson(const QJsonObject &obj);
};

/**
 * @brief A per-room virtual node.
 *
 * Each instance maps to one module-null-sink loaded while the room is active.
 * Applications are routed into it like into any output.
 *
 * `loopbackTargets` are the sinks wired to the node's OUT side.  For a plain
 * virtual node they receive what was played into it; for a network node they
 * receive what the other devices on the channel sent.
 */
struct VirtualSinkDef {
    QString displayName;          ///< User-visible label (e.g. "Gaming Mix")
    QString paName;               ///< PA internal name (e.g. "soundroot_virt_abc123")
    QStringList loopbackTargets;  ///< Sink names wired to the OUT side
    NetworkNodeConfig network;

    bool isNetwork() const { return network.enabled; }

    QJsonObject toJson() const;
    static VirtualSinkDef fromJson(const QJsonObject &obj);
};

/**
 * @brief A single audio route: one application stream routed to one or more sinks.
 *
 * `sourceAppName` is the route key.  "Firefox\t1" means the second Firefox
 * stream; a key without the tab part (written by old versions) matches every
 * stream of that application.  Which live stream a route applies to is
 * decided by StreamBinder.
 */
struct AudioRoute {
    QString sourceAppName;          ///< Route key: "<app>\t<ordinal>" or legacy "<app>"
    QString mediaHint;              ///< media.name of the stream when it was wired; used to re-find it
    QStringList outputSinkNames;    ///< Sink names (real or virtual) the stream plays on
    int volumePercent = 100;        ///< Unused; kept so old config files round-trip

    QString appName() const;        ///< Key without the ordinal
    int     ordinal() const;        ///< -1 for a legacy all-streams route
    static QString makeKey(const QString &appName, int ordinal);

    QJsonObject toJson() const;
    static AudioRoute fromJson(const QJsonObject &obj);
};

/**
 * @brief A room: a named set of routes and virtual nodes.
 *
 * At most one room is active at a time; the routing engine applies the active
 * room's routes to the sound server and keeps them applied.
 */
struct AudioGroup {
    QString id;
    QString name;
    QString color;
    QString icon;
    bool active = false;
    QVector<AudioRoute> routes;
    QVector<VirtualSinkDef> virtualSinks;

    AudioGroup();
    explicit AudioGroup(const QString &groupName,
                        const QString &groupColor = QStringLiteral("#4a90d9"),
                        const QString &groupIcon = QStringLiteral("audio-card"));

    /// Number of wires the room has (route outputs + virtual node outputs).
    int linkCount() const;
    VirtualSinkDef       *virtualSink(const QString &paName);
    const VirtualSinkDef *virtualSink(const QString &paName) const;
    AudioRoute           *route(const QString &key);

    QJsonObject toJson() const;
    static AudioGroup fromJson(const QJsonObject &obj);
};
