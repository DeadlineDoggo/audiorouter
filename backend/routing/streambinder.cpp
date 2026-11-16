#include "streambinder.h"

#include <QSet>
#include <algorithm>

namespace StreamBinder {

Bindings bind(const QVector<AudioRoute> &routes,
              const QVector<PASinkInputInfo> &streams,
              const Bindings &previous)
{
    Bindings result;

    // Applications' own streams only, oldest first
    QVector<PASinkInputInfo> live;
    for (const auto &s : streams) {
        if (s.netNode.isEmpty())
            live.append(s);
    }
    std::sort(live.begin(), live.end(),
              [](const PASinkInputInfo &a, const PASinkInputInfo &b) { return a.index < b.index; });

    QHash<QString, const AudioRoute *> routeByKey;
    for (const auto &r : routes)
        routeByKey.insert(r.sourceAppName, &r);

    QSet<uint32_t> boundStreams;
    QSet<QString> boundRoutes;   // per-stream routes that have their stream

    // 1. Keep existing bindings that are still valid
    for (const auto &s : std::as_const(live)) {
        const auto prev = previous.constFind(s.index);
        if (prev == previous.constEnd()) continue;
        const AudioRoute *route = routeByKey.value(prev.value());
        if (!route || route->appName() != s.appName) continue;
        if (route->ordinal() >= 0) {
            if (boundRoutes.contains(route->sourceAppName)) continue;   // one stream per route
            boundRoutes.insert(route->sourceAppName);
        }
        result.insert(s.index, route->sourceAppName);
        boundStreams.insert(s.index);
    }

    // Per-stream routes still looking for a stream, lowest ordinal first
    QVector<const AudioRoute *> open;
    for (const auto &r : routes) {
        if (r.ordinal() >= 0 && !boundRoutes.contains(r.sourceAppName))
            open.append(&r);
    }
    std::sort(open.begin(), open.end(), [](const AudioRoute *a, const AudioRoute *b) {
        return a->ordinal() < b->ordinal();
    });

    auto take = [&](const AudioRoute *route, bool needMediaMatch) {
        for (const auto &s : std::as_const(live)) {
            if (boundStreams.contains(s.index) || s.appName != route->appName()) continue;
            if (needMediaMatch && s.mediaName != route->mediaHint) continue;
            result.insert(s.index, route->sourceAppName);
            boundStreams.insert(s.index);
            boundRoutes.insert(route->sourceAppName);
            return;
        }
    };

    // 2. Media-name matches
    for (const AudioRoute *route : std::as_const(open)) {
        if (!route->mediaHint.isEmpty())
            take(route, true);
    }
    // 3. In order
    for (const AudioRoute *route : std::as_const(open)) {
        if (!boundRoutes.contains(route->sourceAppName))
            take(route, false);
    }

    // 4. Legacy all-streams routes take what is left of their application
    for (const auto &r : routes) {
        if (r.ordinal() >= 0) continue;
        for (const auto &s : std::as_const(live)) {
            if (!boundStreams.contains(s.index) && s.appName == r.appName()) {
                result.insert(s.index, r.sourceAppName);
                boundStreams.insert(s.index);
            }
        }
    }

    return result;
}

int freeOrdinal(const QVector<AudioRoute> &routes, const QString &appName)
{
    QSet<int> used;
    for (const auto &r : routes) {
        if (r.appName() == appName && r.ordinal() >= 0)
            used.insert(r.ordinal());
    }
    int n = 0;
    while (used.contains(n)) ++n;
    return n;
}

} // namespace StreamBinder
