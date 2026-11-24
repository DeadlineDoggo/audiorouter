#include "audiogroup.h"
#include <QJsonArray>

namespace {
QJsonArray toArray(const QStringList &list)
{
    QJsonArray arr;
    for (const auto &s : list)
        arr.append(s);
    return arr;
}

QStringList toStringList(const QJsonValue &value)
{
    QStringList list;
    const auto arr = value.toArray();
    for (const auto &v : arr) {
        const QString s = v.toString();
        // Skip empty entries and duplicates: both would only confuse routing
        if (!s.isEmpty() && !list.contains(s))
            list.append(s);
    }
    return list;
}
} // namespace

// ---------------------------------------------------------------------------
// NetworkNodeConfig
// ---------------------------------------------------------------------------

QJsonObject NetworkNodeConfig::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("enabled")] = enabled;
    obj[QStringLiteral("channel")] = channel;
    obj[QStringLiteral("passphrase")] = passphrase;
    obj[QStringLiteral("staticPeers")] = toArray(staticPeers);
    return obj;
}

NetworkNodeConfig NetworkNodeConfig::fromJson(const QJsonObject &obj)
{
    NetworkNodeConfig cfg;
    cfg.enabled = obj[QStringLiteral("enabled")].toBool(false);
    cfg.channel = obj[QStringLiteral("channel")].toString();
    cfg.passphrase = obj[QStringLiteral("passphrase")].toString();
    cfg.staticPeers = toStringList(obj[QStringLiteral("staticPeers")]);
    return cfg;
}

// ---------------------------------------------------------------------------
// AudioRoute
// ---------------------------------------------------------------------------

QString AudioRoute::appNameOf(const QString &key)
{
    const auto tab = key.indexOf(QLatin1Char('\t'));
    return tab >= 0 ? key.left(tab) : key;
}

int AudioRoute::ordinalOf(const QString &key)
{
    const auto tab = key.indexOf(QLatin1Char('\t'));
    if (tab < 0) return -1;
    bool ok = false;
    const int n = key.mid(tab + 1).toInt(&ok);
    return ok && n >= 0 ? n : -1;
}

QString AudioRoute::appName() const { return appNameOf(sourceAppName); }
int     AudioRoute::ordinal() const { return ordinalOf(sourceAppName); }

QString AudioRoute::makeKey(const QString &appName, int ordinal)
{
    return appName + QLatin1Char('\t') + QString::number(ordinal);
}

QJsonObject AudioRoute::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("source")] = sourceAppName;
    obj[QStringLiteral("volumePercent")] = volumePercent;
    if (!mediaHint.isEmpty())
        obj[QStringLiteral("media")] = mediaHint;
    obj[QStringLiteral("outputs")] = toArray(outputSinkNames);
    return obj;
}

AudioRoute AudioRoute::fromJson(const QJsonObject &obj)
{
    AudioRoute route;
    route.sourceAppName = obj[QStringLiteral("source")].toString();
    route.volumePercent = obj[QStringLiteral("volumePercent")].toInt(100);
    route.mediaHint = obj[QStringLiteral("media")].toString();
    route.outputSinkNames = toStringList(obj[QStringLiteral("outputs")]);
    return route;
}

// ---------------------------------------------------------------------------
// CaptureRoute
// ---------------------------------------------------------------------------

QJsonObject CaptureRoute::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("app")] = key;
    if (!mediaHint.isEmpty())
        obj[QStringLiteral("media")] = mediaHint;
    obj[QStringLiteral("source")] = sourceName;
    return obj;
}

CaptureRoute CaptureRoute::fromJson(const QJsonObject &obj)
{
    CaptureRoute route;
    route.key = obj[QStringLiteral("app")].toString();
    route.mediaHint = obj[QStringLiteral("media")].toString();
    route.sourceName = obj[QStringLiteral("source")].toString();
    return route;
}

// ---------------------------------------------------------------------------
// VirtualSinkDef
// ---------------------------------------------------------------------------

QJsonObject VirtualSinkDef::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("displayName")] = displayName;
    obj[QStringLiteral("paName")] = paName;
    obj[QStringLiteral("loopbackTargets")] = toArray(loopbackTargets);
    if (!inputSources.isEmpty())
        obj[QStringLiteral("inputSources")] = toArray(inputSources);
    // Written only when it was ever configured, so plain virtual nodes keep
    // the file format old versions wrote.
    if (network != NetworkNodeConfig())
        obj[QStringLiteral("network")] = network.toJson();
    return obj;
}

VirtualSinkDef VirtualSinkDef::fromJson(const QJsonObject &obj)
{
    VirtualSinkDef vs;
    vs.displayName = obj[QStringLiteral("displayName")].toString();
    vs.paName = obj[QStringLiteral("paName")].toString();
    vs.loopbackTargets = toStringList(obj[QStringLiteral("loopbackTargets")]);
    vs.inputSources = toStringList(obj[QStringLiteral("inputSources")]);
    vs.network = NetworkNodeConfig::fromJson(obj[QStringLiteral("network")].toObject());
    return vs;
}

// ---------------------------------------------------------------------------
// AudioGroup
// ---------------------------------------------------------------------------

AudioGroup::AudioGroup()
    : id(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , color(QStringLiteral("#4a90d9"))
    , icon(QStringLiteral("audio-card"))
{
}

AudioGroup::AudioGroup(const QString &groupName, const QString &groupColor,
                       const QString &groupIcon)
    : id(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , name(groupName)
    , color(groupColor.isEmpty() ? QStringLiteral("#4a90d9") : groupColor)
    , icon(groupIcon.isEmpty() ? QStringLiteral("audio-card") : groupIcon)
{
}

int AudioGroup::linkCount() const
{
    qsizetype n = 0;
    for (const auto &r : routes)
        n += r.outputSinkNames.size();
    for (const auto &vs : virtualSinks)
        n += vs.loopbackTargets.size() + vs.inputSources.size();
    n += captures.size();
    return static_cast<int>(n);
}

VirtualSinkDef *AudioGroup::virtualSink(const QString &paName)
{
    for (auto &vs : virtualSinks) {
        if (vs.paName == paName) return &vs;
    }
    return nullptr;
}

const VirtualSinkDef *AudioGroup::virtualSink(const QString &paName) const
{
    for (const auto &vs : virtualSinks) {
        if (vs.paName == paName) return &vs;
    }
    return nullptr;
}

AudioRoute *AudioGroup::route(const QString &key)
{
    for (auto &r : routes) {
        if (r.sourceAppName == key) return &r;
    }
    return nullptr;
}

CaptureRoute *AudioGroup::capture(const QString &key)
{
    for (auto &c : captures) {
        if (c.key == key) return &c;
    }
    return nullptr;
}

QJsonObject AudioGroup::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")]    = id;
    obj[QStringLiteral("name")]  = name;
    obj[QStringLiteral("color")] = color;
    obj[QStringLiteral("icon")]  = icon;
    obj[QStringLiteral("active")] = active;

    QJsonArray routeArray;
    for (const auto &route : routes)
        routeArray.append(route.toJson());
    obj[QStringLiteral("routes")] = routeArray;

    QJsonArray vsinkArray;
    for (const auto &vs : virtualSinks)
        vsinkArray.append(vs.toJson());
    obj[QStringLiteral("virtualSinks")] = vsinkArray;

    // Written only when there are any, so rooms without them keep the format
    // older versions read.
    if (!captures.isEmpty()) {
        QJsonArray captureArray;
        for (const auto &cap : captures)
            captureArray.append(cap.toJson());
        obj[QStringLiteral("captures")] = captureArray;
    }

    return obj;
}

AudioGroup AudioGroup::fromJson(const QJsonObject &obj)
{
    AudioGroup group;   // starts with a fresh UUID
    // Keep the generated id when the file has none: several rooms sharing
    // the id "" could not be told apart.
    const QString storedId = obj[QStringLiteral("id")].toString();
    if (!storedId.isEmpty())
        group.id = storedId;
    group.name  = obj[QStringLiteral("name")].toString();
    group.color = obj[QStringLiteral("color")].toString(QStringLiteral("#4a90d9"));
    group.icon  = obj[QStringLiteral("icon")].toString(QStringLiteral("audio-card"));
    group.active = obj[QStringLiteral("active")].toBool(false);

    const auto routes = obj[QStringLiteral("routes")].toArray();
    for (const auto &val : routes) {
        AudioRoute route = AudioRoute::fromJson(val.toObject());
        if (route.sourceAppName.isEmpty() || route.outputSinkNames.isEmpty())
            continue;   // nothing to route
        // Two routes with one key would fight over the same stream: merge
        if (AudioRoute *existing = group.route(route.sourceAppName)) {
            for (const auto &out : std::as_const(route.outputSinkNames)) {
                if (!existing->outputSinkNames.contains(out))
                    existing->outputSinkNames.append(out);
            }
        } else {
            group.routes.append(route);
        }
    }

    const auto vsinks = obj[QStringLiteral("virtualSinks")].toArray();
    for (const auto &val : vsinks) {
        const VirtualSinkDef vs = VirtualSinkDef::fromJson(val.toObject());
        if (!vs.paName.isEmpty() && !group.virtualSink(vs.paName))
            group.virtualSinks.append(vs);
    }

    const auto captures = obj[QStringLiteral("captures")].toArray();
    for (const auto &val : captures) {
        const CaptureRoute cap = CaptureRoute::fromJson(val.toObject());
        // One application records from one source: the first entry wins
        if (!cap.key.isEmpty() && !cap.sourceName.isEmpty() && !group.capture(cap.key))
            group.captures.append(cap);
    }

    return group;
}
