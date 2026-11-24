#include "streambinder.h"

#include <QSet>
#include <algorithm>

namespace StreamBinder {

namespace {

const QString &keyOf(const AudioRoute &r) { return r.sourceAppName; }
const QString &keyOf(const CaptureRoute &r) { return r.key; }

/// Streams SoundRoot plays itself are not applications
bool isOwn(const PASinkInputInfo &s) { return !s.netNode.isEmpty(); }
bool isOwn(const PASourceOutputInfo &) { return false; }

template <typename Route, typename Stream>
Bindings bindImpl(const QVector<Route> &routes, const QVector<Stream> &streams,
                  const Bindings &previous)
{
    Bindings result;

    // Applications' own streams only, oldest first
    QVector<Stream> live;
    for (const auto &s : streams) {
        if (!isOwn(s))
            live.append(s);
    }
    std::sort(live.begin(), live.end(),
              [](const Stream &a, const Stream &b) { return a.index < b.index; });

    QHash<QString, const Route *> routeByKey;
    for (const auto &r : routes)
        routeByKey.insert(keyOf(r), &r);

    QSet<uint32_t> boundStreams;
    QSet<QString> boundRoutes;   // per-stream routes that have their stream

    // 1. Keep existing bindings that are still valid
    for (const auto &s : std::as_const(live)) {
        const auto prev = previous.constFind(s.index);
        if (prev == previous.constEnd()) continue;
        const Route *route = routeByKey.value(prev.value());
        if (!route || route->appName() != s.appName) continue;
        if (route->ordinal() >= 0) {
            if (boundRoutes.contains(keyOf(*route))) continue;   // one stream per route
            boundRoutes.insert(keyOf(*route));
        }
        result.insert(s.index, keyOf(*route));
        boundStreams.insert(s.index);
    }

    // Per-stream routes still looking for a stream, lowest ordinal first
    QVector<const Route *> open;
    for (const auto &r : routes) {
        if (r.ordinal() >= 0 && !boundRoutes.contains(keyOf(r)))
            open.append(&r);
    }
    std::sort(open.begin(), open.end(), [](const Route *a, const Route *b) {
        return a->ordinal() < b->ordinal();
    });

    auto take = [&](const Route *route, bool needMediaMatch) {
        for (const auto &s : std::as_const(live)) {
            if (boundStreams.contains(s.index) || s.appName != route->appName()) continue;
            if (needMediaMatch && s.mediaName != route->mediaHint) continue;
            result.insert(s.index, keyOf(*route));
            boundStreams.insert(s.index);
            boundRoutes.insert(keyOf(*route));
            return;
        }
    };

    // 2. Media-name matches
    for (const Route *route : std::as_const(open)) {
        if (!route->mediaHint.isEmpty())
            take(route, true);
    }
    // 3. In order
    for (const Route *route : std::as_const(open)) {
        if (!boundRoutes.contains(keyOf(*route)))
            take(route, false);
    }

    // 4. Legacy all-streams routes take what is left of their application
    for (const auto &r : routes) {
        if (r.ordinal() >= 0) continue;
        for (const auto &s : std::as_const(live)) {
            if (!boundStreams.contains(s.index) && s.appName == r.appName()) {
                result.insert(s.index, keyOf(r));
                boundStreams.insert(s.index);
            }
        }
    }

    return result;
}

template <typename Route>
int freeOrdinalImpl(const QVector<Route> &routes, const QString &appName)
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

} // namespace

Bindings bind(const QVector<AudioRoute> &routes, const QVector<PASinkInputInfo> &streams,
              const Bindings &previous)
{
    return bindImpl(routes, streams, previous);
}

Bindings bind(const QVector<CaptureRoute> &routes, const QVector<PASourceOutputInfo> &streams,
              const Bindings &previous)
{
    return bindImpl(routes, streams, previous);
}

int freeOrdinal(const QVector<AudioRoute> &routes, const QString &appName)
{
    return freeOrdinalImpl(routes, appName);
}

int freeOrdinal(const QVector<CaptureRoute> &routes, const QString &appName)
{
    return freeOrdinalImpl(routes, appName);
}

} // namespace StreamBinder
