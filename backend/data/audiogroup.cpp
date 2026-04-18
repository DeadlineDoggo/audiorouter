#include "audiogroup.h"
#include <QJsonArray>

// ---------------------------------------------------------------------------
// AudioRoute
// ---------------------------------------------------------------------------

QJsonObject AudioRoute::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("source")] = sourceAppName;
    obj[QStringLiteral("volumePercent")] = volumePercent;

    QJsonArray outputs;
    for (const auto &sink : outputSinkNames)
        outputs.append(sink);
    obj[QStringLiteral("outputs")] = outputs;

    return obj;
}

AudioRoute AudioRoute::fromJson(const QJsonObject &obj)
{
    AudioRoute route;
    route.sourceAppName = obj[QStringLiteral("source")].toString();
    route.volumePercent = obj[QStringLiteral("volumePercent")].toInt(100);

    const auto outputs = obj[QStringLiteral("outputs")].toArray();
    for (const auto &val : outputs)
        route.outputSinkNames.append(val.toString());

    return route;
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

AudioGroup::AudioGroup(const QString &name, const QString &color, const QString &icon)
    : id(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , name(name)
    , color(color.isEmpty() ? QStringLiteral("#4a90d9") : color)
    , icon(icon.isEmpty() ? QStringLiteral("audio-card") : icon)
{
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

    return obj;
}

AudioGroup AudioGroup::fromJson(const QJsonObject &obj)
{
    AudioGroup group;
    group.id    = obj[QStringLiteral("id")].toString();
    group.name  = obj[QStringLiteral("name")].toString();
    group.color = obj[QStringLiteral("color")].toString(QStringLiteral("#4a90d9"));
    group.icon  = obj[QStringLiteral("icon")].toString(QStringLiteral("audio-card"));
    group.active = obj[QStringLiteral("active")].toBool(false);

    const auto routes = obj[QStringLiteral("routes")].toArray();
    for (const auto &val : routes)
        group.routes.append(AudioRoute::fromJson(val.toObject()));

    return group;
}
