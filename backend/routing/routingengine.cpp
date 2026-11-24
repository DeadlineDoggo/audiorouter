#include "routingengine.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QUuid>

#include <algorithm>
#include <utility>

#include "data/audiogroup.h"
#include "models/grouplistmodel.h"

namespace {

const QLatin1String kInput("input:");
const QLatin1String kApp("app:");
const QLatin1String kSink("sink:");
const QLatin1String kVsinkIn("vsinkin:");
const QLatin1String kVsinkOut("vsinkout:");
const QLatin1String kMic("mic:");
const QLatin1String kRec("rec:");
const QLatin1String kRecApp("recapp:");

bool isRecKey(const QString &k) { return k.startsWith(kRec) || k.startsWith(kRecApp); }

bool isSourceKey(const QString &k) { return k.startsWith(kInput) || k.startsWith(kApp); }
bool isTargetKey(const QString &k) { return k.startsWith(kSink) || k.startsWith(kVsinkIn); }

/// Sink name behind a "sink:" or "vsinkin:" key.
QString targetSinkName(const QString &k)
{
    if (k.startsWith(kSink)) return k.mid(kSink.size());
    if (k.startsWith(kVsinkIn)) return k.mid(kVsinkIn.size());
    return {};
}

/// Node key of a wire's target sink within a room.
QString targetKey(const AudioGroup &g, const QString &sinkName)
{
    return (g.virtualSink(sinkName) ? QString(kVsinkIn) : QString(kSink)) + sinkName;
}

/// Name of the combine-sink for a set of outputs.  Derived from the set, so
/// every stream with the same outputs shares one module and a changed set
/// simply is another sink.
QString combineName(QStringList sinks)
{
    sinks.sort();
    const QByteArray hash = QCryptographicHash::hash(
        sinks.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha1);
    return combineSinkPrefix() + QStringLiteral("mix_")
           + QString::fromLatin1(hash.toHex().left(12));
}

/// Is the wanted configuration of a network node complete enough to start?
bool networkUsable(const VirtualSinkDef &vs)
{
    return vs.network.enabled && !vs.network.channel.trimmed().isEmpty()
           && !vs.network.passphrase.isEmpty();
}

/// Can audio played into virtual node @p from reach node @p to through the
/// room's wires?  Network nodes do not pass their input to their own output.
bool reaches(const AudioGroup &g, const QString &from, const QString &to, QSet<QString> &seen)
{
    if (from == to) return true;
    if (seen.contains(from)) return false;
    seen.insert(from);
    const VirtualSinkDef *vs = g.virtualSink(from);
    if (!vs || vs->isNetwork()) return false;
    for (const auto &next : vs->loopbackTargets) {
        if (g.virtualSink(next) && reaches(g, next, to, seen))
            return true;
    }
    return false;
}

} // namespace

RoutingEngine::RoutingEngine(AudioBackend *backend, GroupListModel *groups, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_groups(groups)
{
    m_clock.start();

    // Server events arrive in bursts (one per property change); a short
    // delay folds a burst into one pass.
    m_reconcileTimer.setSingleShot(true);
    m_reconcileTimer.setInterval(40);
    connect(&m_reconcileTimer, &QTimer::timeout, this, &RoutingEngine::reconcile);

    // Every change we care about raises an event, so this pass normally
    // finds nothing to do.  It is the safety net for a lost event.
    m_watchdogTimer.setInterval(5000);
    connect(&m_watchdogTimer, &QTimer::timeout, this, [this]() {
        if (activeGroup() || !m_virtualSinks.isEmpty() || !m_combines.isEmpty()
            || !m_managedStreams.isEmpty() || !m_managedRecorders.isEmpty())
            reconcile();
    });
    m_watchdogTimer.start();

    connect(m_backend, &AudioBackend::sinksChanged, this, [this]() {
        scheduleReconcile();
        notifyIfTopologyChanged();
    });
    connect(m_backend, &AudioBackend::sinkInputsChanged, this, [this]() {
        scheduleReconcile();
        notifyIfTopologyChanged();
    });
    connect(m_backend, &AudioBackend::sourcesChanged, this, [this]() {
        scheduleReconcile();
        notifyIfTopologyChanged();
    });
    connect(m_backend, &AudioBackend::sourceOutputsChanged, this, [this]() {
        scheduleReconcile();
        notifyIfTopologyChanged();
    });
    connect(m_backend, &AudioBackend::serverConnected, this, &RoutingEngine::reconcile);
    connect(m_backend, &AudioBackend::serverDisconnected,
            this, &RoutingEngine::onServerDisconnected);
}

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

AudioGroup *RoutingEngine::group(int row) const
{
    return m_groups->groupAt(row);
}

AudioGroup *RoutingEngine::activeGroup() const
{
    return m_groups->groupAt(m_groups->activeRow());
}

const StreamBinder::Bindings &RoutingEngine::bindingsFor(const AudioGroup &g)
{
    auto &bindings = m_bindings[g.id];
    bindings = StreamBinder::bind(g.routes, m_backend->availableSinkInputs(), bindings);
    return bindings;
}

const StreamBinder::Bindings &RoutingEngine::captureBindingsFor(const AudioGroup &g)
{
    auto &bindings = m_captureBindings[g.id];
    bindings = StreamBinder::bind(g.captures, m_backend->availableSourceOutputs(), bindings);
    return bindings;
}

RoutingEngine::Endpoints RoutingEngine::classify(const QString &a, const QString &b) const
{
    Endpoints e;

    // The recording side: a microphone or a node's output, into a node or
    // into a recording application
    for (const auto &[from, to] : { std::pair(&a, &b), std::pair(&b, &a) }) {
        if (from->startsWith(kMic) && to->startsWith(kVsinkIn)) {
            e.kind = Endpoints::MicInput;
            e.source = from->mid(kMic.size());
            e.target = to->mid(kVsinkIn.size());
        } else if ((from->startsWith(kMic) || from->startsWith(kVsinkOut)) && isRecKey(*to)) {
            e.kind = Endpoints::Capture;
            e.source = from->startsWith(kMic) ? from->mid(kMic.size())
                                              : virtualMicName(from->mid(kVsinkOut.size()));
            e.target = *to;
        } else {
            continue;
        }
        if (e.source.isEmpty() || e.target.isEmpty()) return {};
        // "mic:" is a real microphone; a node is reached through its OUT
        if (from->startsWith(kMic) && isInternalSourceName(e.source)) return {};
        return e;
    }

    const QString *src = nullptr;
    const QString *dst = nullptr;
    if ((isSourceKey(a) || a.startsWith(kVsinkOut)) && isTargetKey(b)) { src = &a; dst = &b; }
    else if ((isSourceKey(b) || b.startsWith(kVsinkOut)) && isTargetKey(a)) { src = &b; dst = &a; }
    if (!src) return e;

    e.target = targetSinkName(*dst);
    if (e.target.isEmpty()) return e;
    if (src->startsWith(kVsinkOut)) {
        e.kind = Endpoints::Loopback;
        e.source = src->mid(kVsinkOut.size());
    } else {
        e.kind = Endpoints::Route;
        e.source = *src;
    }
    return e;
}

QString RoutingEngine::routeKeyFor(AudioGroup &g, const QString &sourceKey, bool create)
{
    if (sourceKey.startsWith(kApp)) {
        const QString key = sourceKey.mid(kApp.size());
        return g.route(key) ? key : QString();
    }
    if (!sourceKey.startsWith(kInput)) return {};

    bool ok = false;
    const uint32_t index = sourceKey.mid(kInput.size()).toUInt(&ok);
    if (!ok) return {};

    const auto &bindings = bindingsFor(g);
    const auto bound = bindings.constFind(index);
    if (bound != bindings.constEnd())
        return bound.value();
    if (!create) return {};

    // A live stream without a route yet: give it its own
    const auto inputs = m_backend->availableSinkInputs();
    for (const auto &in : inputs) {
        if (in.index != index || !in.netNode.isEmpty()) continue;
        AudioRoute route;
        route.sourceAppName = AudioRoute::makeKey(
            in.appName, StreamBinder::freeOrdinal(g.routes, in.appName));
        route.mediaHint = in.mediaName;
        g.routes.append(route);
        // Pin this very stream to the new route; without this the binder
        // could hand the route to an older stream of the same application.
        m_bindings[g.id].insert(index, route.sourceAppName);
        return route.sourceAppName;
    }
    return {};
}

QString RoutingEngine::captureKeyFor(AudioGroup &g, const QString &recKey, bool create)
{
    if (recKey.startsWith(kRecApp)) {
        const QString key = recKey.mid(kRecApp.size());
        return g.capture(key) ? key : QString();
    }
    if (!recKey.startsWith(kRec)) return {};

    bool ok = false;
    const uint32_t index = recKey.mid(kRec.size()).toUInt(&ok);
    if (!ok) return {};

    const auto &bindings = captureBindingsFor(g);
    const auto bound = bindings.constFind(index);
    if (bound != bindings.constEnd())
        return bound.value();
    if (!create) return {};

    const auto recorders = m_backend->availableSourceOutputs();
    for (const auto &rec : recorders) {
        if (rec.index != index) continue;
        CaptureRoute cap;
        cap.key = AudioRoute::makeKey(rec.appName, StreamBinder::freeOrdinal(g.captures, rec.appName));
        cap.mediaHint = rec.mediaName;
        g.captures.append(cap);
        m_captureBindings[g.id].insert(index, cap.key);   // this very stream, as in routeKeyFor
        return cap.key;
    }
    return {};
}

void RoutingEngine::dropCapturesFrom(AudioGroup &g, const QString &sourceName)
{
    g.captures.erase(std::remove_if(g.captures.begin(), g.captures.end(),
                                    [&](const CaptureRoute &c) { return c.sourceName == sourceName; }),
                     g.captures.end());
}

bool RoutingEngine::wouldLoop(const AudioGroup &g, const QString &fromPa,
                              const QString &toSink) const
{
    const VirtualSinkDef *from = g.virtualSink(fromPa);
    if (!from || from->isNetwork() || !g.virtualSink(toSink))
        return false;
    QSet<QString> seen;
    return reaches(g, toSink, fromPa, seen);
}

void RoutingEngine::dropUnusedRoutes(AudioGroup &g)
{
    g.routes.erase(std::remove_if(g.routes.begin(), g.routes.end(),
                                  [](const AudioRoute &r) { return r.outputSinkNames.isEmpty(); }),
                   g.routes.end());
}

void RoutingEngine::rememberSinkLabels()
{
    QSet<QString> used;
    for (int i = 0; i < m_groups->rowCount(); ++i) {
        const AudioGroup *g = m_groups->groupAt(i);
        for (const auto &r : g->routes)
            used.unite(QSet<QString>(r.outputSinkNames.cbegin(), r.outputSinkNames.cend()));
        for (const auto &vs : g->virtualSinks) {
            used.unite(QSet<QString>(vs.loopbackTargets.cbegin(), vs.loopbackTargets.cend()));
            used.unite(QSet<QString>(vs.inputSources.cbegin(), vs.inputSources.cend()));
        }
        for (const auto &c : g->captures)
            used.insert(c.sourceName);
    }
    // One store for the labels of outputs and microphones: their names
    // never clash (the server gives each a unique, descriptive name).
    bool changed = false;
    const auto sinks = m_backend->availableSinks();
    for (const auto &s : sinks) {
        if (used.contains(s.name) && !isInternalSinkName(s.name))
            changed |= m_groups->rememberSinkLabel(s.name, s.description);
    }
    const auto sources = m_backend->availableSources();
    for (const auto &s : sources) {
        if (used.contains(s.name) && !isInternalSourceName(s.name))
            changed |= m_groups->rememberSinkLabel(s.name, s.description);
    }
    if (changed)
        m_groups->save();
}

void RoutingEngine::notifyIfTopologyChanged()
{
    // The server reports every volume change as a stream/sink change.  The
    // wires only depend on which streams and devices exist, so only tell the
    // UI when that set changed.
    QStringList parts;
    const auto inputs = m_backend->availableSinkInputs();
    for (const auto &in : inputs) {
        if (in.netNode.isEmpty())
            parts.append(QString::number(in.index) + QLatin1Char('\t') + in.appName
                         + QLatin1Char('\t') + in.mediaName);
    }
    parts.append(QStringLiteral("|"));
    const auto sinks = m_backend->availableSinks();
    for (const auto &s : sinks)
        parts.append(s.name);
    parts.append(QStringLiteral("|"));
    const auto recorders = m_backend->availableSourceOutputs();
    for (const auto &r : recorders)
        parts.append(QString::number(r.index) + QLatin1Char('\t') + r.appName
                     + QLatin1Char('\t') + r.mediaName);
    parts.append(QStringLiteral("|"));
    const auto sources = m_backend->availableSources();
    for (const auto &s : sources)
        parts.append(s.name);
    const QString signature = parts.join(QLatin1Char('\n'));
    if (signature == m_topologySignature) return;
    m_topologySignature = signature;
    emit connectionsChanged();
}

void RoutingEngine::roomEdited(int row)
{
    rememberSinkLabels();
    m_groups->save();
    m_groups->notifyLinksChanged(row);
    emit connectionsChanged();
    if (m_groups->isGroupActive(row))
        reconcile();
}

// ═══════════════════════════════════════════════════════════════════════════
// Room activation
// ═══════════════════════════════════════════════════════════════════════════

void RoutingEngine::activateRoom(int row)
{
    if (!group(row)) return;
    // Exactly one room is active
    for (int i = 0; i < m_groups->rowCount(); ++i) {
        if (i != row)
            m_groups->setGroupActive(i, false);
    }
    m_groups->setGroupActive(row, true);
    m_moveAttempts.clear();
    m_recorderMoveAttempts.clear();
    reconcile();
}

void RoutingEngine::deactivateRoom(int row)
{
    if (!m_groups->isGroupActive(row)) return;
    m_groups->setGroupActive(row, false);
    m_moveAttempts.clear();
    m_recorderMoveAttempts.clear();
    reconcile();   // tears down what the room had set up
}

void RoutingEngine::removeRoom(int row)
{
    const AudioGroup *g = group(row);
    if (!g) return;
    const QString id = g->id;
    deactivateRoom(row);
    m_bindings.remove(id);
    m_captureBindings.remove(id);
    m_groups->removeGroup(row);
    emit connectionsChanged();
    emit virtualSinksChanged();
}

void RoutingEngine::activateRelative(int step)
{
    const int n = m_groups->rowCount();
    if (n == 0 || step == 0) return;
    const int current = m_groups->activeRow();
    int next;
    if (current < 0)
        next = step > 0 ? 0 : n - 1;
    else
        next = ((current + step) % n + n) % n;
    activateRoom(next);
}

// ═══════════════════════════════════════════════════════════════════════════
// Wires
// ═══════════════════════════════════════════════════════════════════════════

bool RoutingEngine::canConnect(int row, const QString &a, const QString &b) const
{
    const AudioGroup *g = group(row);
    if (!g) return false;

    const Endpoints e = classify(a, b);
    if (e.kind == Endpoints::Invalid) return false;

    if (e.kind == Endpoints::MicInput)
        return g->virtualSink(e.target) != nullptr;

    if (e.kind == Endpoints::Capture) {
        // From a node: one of this room's, and a local one.  A network
        // node's OUT is what the other devices send, which its microphone
        // does not carry.
        const QString node = nodeOfVirtualMic(e.source);
        if (!node.isEmpty()) {
            const VirtualSinkDef *vs = g->virtualSink(node);
            if (!vs || vs->isNetwork()) return false;
        }
        if (e.target.startsWith(kRecApp))
            return g->captures.end() != std::find_if(g->captures.cbegin(), g->captures.cend(),
                [&](const CaptureRoute &c) { return c.key == e.target.mid(kRecApp.size()); });
        bool ok = false;
        const uint32_t index = e.target.mid(kRec.size()).toUInt(&ok);
        if (!ok) return false;
        const auto recorders = m_backend->availableSourceOutputs();
        return std::any_of(recorders.cbegin(), recorders.cend(),
                           [index](const PASourceOutputInfo &r) { return r.index == index; });
    }

    // A virtual target must be one of this room's nodes
    const bool virtualTarget = e.target.startsWith(virtualSinkPrefix());
    if (virtualTarget && !g->virtualSink(e.target)) return false;
    if (e.target.startsWith(combineSinkPrefix())) return false;

    if (e.kind == Endpoints::Loopback) {
        if (!g->virtualSink(e.source) || e.source == e.target) return false;
        return !wouldLoop(*g, e.source, e.target);
    }

    if (e.source.startsWith(kApp))
        return std::any_of(g->routes.cbegin(), g->routes.cend(), [&](const AudioRoute &r) {
            return r.sourceAppName == e.source.mid(kApp.size());
        });

    bool ok = false;
    const uint32_t index = e.source.mid(kInput.size()).toUInt(&ok);
    if (!ok) return false;
    const auto inputs = m_backend->availableSinkInputs();
    return std::any_of(inputs.cbegin(), inputs.cend(), [index](const PASinkInputInfo &in) {
        return in.index == index && in.netNode.isEmpty();
    });
}

bool RoutingEngine::connectNodes(int row, const QString &a, const QString &b)
{
    if (!canConnect(row, a, b)) return false;
    AudioGroup *g = group(row);
    const Endpoints e = classify(a, b);

    if (e.kind == Endpoints::Loopback) {
        VirtualSinkDef *vs = g->virtualSink(e.source);
        if (vs->loopbackTargets.contains(e.target)) return true;
        vs->loopbackTargets.append(e.target);
    } else if (e.kind == Endpoints::MicInput) {
        VirtualSinkDef *vs = g->virtualSink(e.target);
        if (vs->inputSources.contains(e.source)) return true;
        vs->inputSources.append(e.source);
    } else if (e.kind == Endpoints::Capture) {
        // An application records one source: the new wire replaces the old
        CaptureRoute *cap = g->capture(captureKeyFor(*g, e.target, true));
        if (!cap) return false;
        if (cap->sourceName == e.source) return true;
        cap->sourceName = e.source;
    } else {
        const QString key = routeKeyFor(*g, e.source, true);
        AudioRoute *route = g->route(key);
        if (!route) return false;
        if (route->outputSinkNames.contains(e.target)) return true;
        route->outputSinkNames.append(e.target);
    }
    roomEdited(row);
    return true;
}

void RoutingEngine::disconnectNodes(int row, const QString &a, const QString &b)
{
    AudioGroup *g = group(row);
    if (!g) return;
    const Endpoints e = classify(a, b);

    if (e.kind == Endpoints::Loopback) {
        if (VirtualSinkDef *vs = g->virtualSink(e.source))
            vs->loopbackTargets.removeAll(e.target);
    } else if (e.kind == Endpoints::Route) {
        if (AudioRoute *route = g->route(routeKeyFor(*g, e.source, false)))
            route->outputSinkNames.removeAll(e.target);
        dropUnusedRoutes(*g);
    } else if (e.kind == Endpoints::MicInput) {
        if (VirtualSinkDef *vs = g->virtualSink(e.target))
            vs->inputSources.removeAll(e.source);
    } else if (e.kind == Endpoints::Capture) {
        const QString key = captureKeyFor(*g, e.target, false);
        g->captures.erase(std::remove_if(g->captures.begin(), g->captures.end(),
                                         [&](const CaptureRoute &c) {
                                             return c.key == key && c.sourceName == e.source;
                                         }),
                          g->captures.end());
    } else {
        return;
    }
    roomEdited(row);
}

void RoutingEngine::disconnectNode(int row, const QString &key)
{
    AudioGroup *g = group(row);
    if (!g) return;

    if (isSourceKey(key)) {
        const QString routeKey = routeKeyFor(*g, key, false);
        if (routeKey.isEmpty()) return;
        g->routes.erase(std::remove_if(g->routes.begin(), g->routes.end(),
                                       [&](const AudioRoute &r) { return r.sourceAppName == routeKey; }),
                        g->routes.end());
    } else if (key.startsWith(kVsinkOut)) {
        VirtualSinkDef *vs = g->virtualSink(key.mid(kVsinkOut.size()));
        if (!vs) return;
        vs->loopbackTargets.clear();
        dropCapturesFrom(*g, virtualMicName(vs->paName));
    } else if (isTargetKey(key)) {
        const QString sink = targetSinkName(key);
        for (auto &r : g->routes)
            r.outputSinkNames.removeAll(sink);
        for (auto &vs : g->virtualSinks)
            vs.loopbackTargets.removeAll(sink);
        if (VirtualSinkDef *vs = g->virtualSink(sink))
            vs->inputSources.clear();
        dropUnusedRoutes(*g);
    } else if (key.startsWith(kMic)) {
        const QString mic = key.mid(kMic.size());
        for (auto &vs : g->virtualSinks)
            vs.inputSources.removeAll(mic);
        dropCapturesFrom(*g, mic);
    } else if (isRecKey(key)) {
        const QString capKey = captureKeyFor(*g, key, false);
        if (capKey.isEmpty()) return;
        g->captures.erase(std::remove_if(g->captures.begin(), g->captures.end(),
                                         [&](const CaptureRoute &c) { return c.key == capKey; }),
                          g->captures.end());
    } else {
        return;
    }
    roomEdited(row);
}

void RoutingEngine::clearRoom(int row)
{
    AudioGroup *g = group(row);
    if (!g) return;
    g->routes.clear();
    g->captures.clear();
    for (auto &vs : g->virtualSinks) {
        vs.loopbackTargets.clear();
        vs.inputSources.clear();
    }
    roomEdited(row);
}

QVariantList RoutingEngine::connections(int row)
{
    QVariantList result;
    const AudioGroup *g = group(row);
    if (!g) return result;

    auto wire = [&result](const QString &from, const QString &to) {
        QVariantMap m;
        m[QStringLiteral("from")] = from;
        m[QStringLiteral("to")] = to;
        result.append(m);
    };

    const auto &bindings = bindingsFor(*g);
    for (const auto &route : g->routes) {
        QVector<uint32_t> streams = bindings.keys(route.sourceAppName);
        std::sort(streams.begin(), streams.end());
        for (const auto &sink : route.outputSinkNames) {
            // A wire into a virtual node that no longer exists has no end
            if (sink.startsWith(virtualSinkPrefix()) && !g->virtualSink(sink)) continue;
            const QString to = targetKey(*g, sink);
            if (streams.isEmpty())
                wire(kApp + route.sourceAppName, to);
            for (uint32_t s : std::as_const(streams))
                wire(kInput + QString::number(s), to);
        }
    }
    for (const auto &vs : g->virtualSinks) {
        for (const auto &sink : vs.loopbackTargets) {
            if (sink.startsWith(virtualSinkPrefix()) && !g->virtualSink(sink)) continue;
            wire(kVsinkOut + vs.paName, targetKey(*g, sink));
        }
        for (const auto &mic : vs.inputSources)
            wire(kMic + mic, kVsinkIn + vs.paName);
    }

    const auto &captureBindings = captureBindingsFor(*g);
    for (const auto &cap : g->captures) {
        QString from;
        const QString node = nodeOfVirtualMic(cap.sourceName);
        if (node.isEmpty())
            from = kMic + cap.sourceName;
        else if (g->virtualSink(node))
            from = kVsinkOut + node;
        else
            continue;   // the node is gone
        QVector<uint32_t> streams = captureBindings.keys(cap.key);
        std::sort(streams.begin(), streams.end());
        if (streams.isEmpty())
            wire(from, kRecApp + cap.key);
        for (uint32_t s : std::as_const(streams))
            wire(from, kRec + QString::number(s));
    }
    return result;
}

QVariantList RoutingEngine::offlineSources(int row)
{
    QVariantList result;
    const AudioGroup *g = group(row);
    if (!g) return result;

    const auto &bindings = bindingsFor(*g);
    const auto boundKeys = bindings.values();
    for (const auto &route : g->routes) {
        if (boundKeys.contains(route.sourceAppName)) continue;
        QVariantMap m;
        m[QStringLiteral("key")] = QString(kApp + route.sourceAppName);
        m[QStringLiteral("appName")] = route.appName();
        m[QStringLiteral("mediaName")] = route.mediaHint;
        result.append(m);
    }
    return result;
}

QVariantList RoutingEngine::offlineSinks(int row)
{
    QVariantList result;
    const AudioGroup *g = group(row);
    if (!g) return result;

    QSet<QString> present;
    const auto sinks = m_backend->availableSinks();
    for (const auto &s : sinks)
        present.insert(s.name);

    QStringList missing;
    auto consider = [&](const QString &sink) {
        if (!present.contains(sink) && !isInternalSinkName(sink) && !missing.contains(sink))
            missing.append(sink);
    };
    for (const auto &r : g->routes)
        for (const auto &sink : r.outputSinkNames) consider(sink);
    for (const auto &vs : g->virtualSinks)
        for (const auto &sink : vs.loopbackTargets) consider(sink);

    for (const auto &sink : std::as_const(missing)) {
        QVariantMap m;
        m[QStringLiteral("name")] = sink;
        const QString label = m_groups->sinkLabel(sink);
        m[QStringLiteral("description")] = label.isEmpty() ? sink : label;
        result.append(m);
    }
    return result;
}

QVariantList RoutingEngine::offlineMics(int row)
{
    QVariantList result;
    const AudioGroup *g = group(row);
    if (!g) return result;

    QSet<QString> present;
    const auto sources = m_backend->availableSources();
    for (const auto &s : sources)
        present.insert(s.name);

    QStringList missing;
    auto consider = [&](const QString &mic) {
        if (!present.contains(mic) && !isInternalSourceName(mic) && !missing.contains(mic))
            missing.append(mic);
    };
    for (const auto &vs : g->virtualSinks)
        for (const auto &mic : vs.inputSources) consider(mic);
    for (const auto &c : g->captures)
        consider(c.sourceName);

    for (const auto &mic : std::as_const(missing)) {
        QVariantMap m;
        m[QStringLiteral("key")] = QString(kMic + mic);
        const QString label = m_groups->sinkLabel(mic);
        m[QStringLiteral("description")] = label.isEmpty() ? mic : label;
        result.append(m);
    }
    return result;
}

QVariantList RoutingEngine::offlineRecorders(int row)
{
    QVariantList result;
    const AudioGroup *g = group(row);
    if (!g) return result;

    const auto boundKeys = captureBindingsFor(*g).values();
    for (const auto &cap : g->captures) {
        if (boundKeys.contains(cap.key)) continue;
        QVariantMap m;
        m[QStringLiteral("key")] = QString(kRecApp + cap.key);
        m[QStringLiteral("appName")] = recorderDisplayName(cap.appName());
        m[QStringLiteral("mediaName")] = cap.mediaHint;
        result.append(m);
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Virtual nodes
// ═══════════════════════════════════════════════════════════════════════════

QString RoutingEngine::addVirtualSink(int row, const QString &displayName)
{
    AudioGroup *g = group(row);
    if (!g) return {};

    VirtualSinkDef def;
    def.displayName = displayName.trimmed();
    def.paName = virtualSinkPrefix() + QUuid::createUuid().toString(QUuid::Id128).left(8);
    g->virtualSinks.append(def);

    m_groups->save();
    emit virtualSinksChanged();
    if (g->active)
        reconcile();
    return def.paName;
}

void RoutingEngine::removeVirtualSink(int row, const QString &paName)
{
    AudioGroup *g = group(row);
    if (!g || !g->virtualSink(paName)) return;

    g->virtualSinks.erase(
        std::remove_if(g->virtualSinks.begin(), g->virtualSinks.end(),
                       [&paName](const VirtualSinkDef &v) { return v.paName == paName; }),
        g->virtualSinks.end());
    // Wires that ended in the node go with it
    for (auto &r : g->routes)
        r.outputSinkNames.removeAll(paName);
    for (auto &vs : g->virtualSinks)
        vs.loopbackTargets.removeAll(paName);
    dropUnusedRoutes(*g);
    dropCapturesFrom(*g, virtualMicName(paName));

    emit virtualSinksChanged();
    roomEdited(row);
}

void RoutingEngine::configureVirtualSink(int row, const QString &paName,
                                         const QVariantMap &config)
{
    AudioGroup *g = group(row);
    VirtualSinkDef *vs = g ? g->virtualSink(paName) : nullptr;
    if (!vs) return;

    if (config.contains(QStringLiteral("displayName"))) {
        const QString name = config.value(QStringLiteral("displayName")).toString().trimmed();
        if (!name.isEmpty()) vs->displayName = name;
    }
    if (config.contains(QStringLiteral("networkEnabled")))
        vs->network.enabled = config.value(QStringLiteral("networkEnabled")).toBool();
    if (config.contains(QStringLiteral("channel")))
        vs->network.channel = config.value(QStringLiteral("channel")).toString().trimmed();
    if (config.contains(QStringLiteral("passphrase")))
        vs->network.passphrase = config.value(QStringLiteral("passphrase")).toString().trimmed();
    if (config.contains(QStringLiteral("staticPeers"))) {
        QStringList peers;
        const auto list = config.value(QStringLiteral("staticPeers")).toStringList();
        for (const auto &p : list) {
            const QString t = p.trimmed();
            if (!t.isEmpty() && !peers.contains(t)) peers.append(t);
        }
        vs->network.staticPeers = peers;
    }

    // A node that stopped being a network node passes its input through
    // again; drop output wires that would now feed back into it.
    if (!vs->isNetwork()) {
        const QStringList targets = vs->loopbackTargets;
        for (const auto &t : targets) {
            QSet<QString> seen;
            if (g->virtualSink(t) && reaches(*g, t, paName, seen))
                vs->loopbackTargets.removeAll(t);
        }
    } else {
        // A network node's OUT cannot feed a recording application
        dropCapturesFrom(*g, virtualMicName(paName));
    }

    emit virtualSinksChanged();
    roomEdited(row);
}

QVariantList RoutingEngine::virtualSinks(int row) const
{
    QVariantList result;
    const AudioGroup *g = group(row);
    if (!g) return result;

    for (const auto &vs : g->virtualSinks) {
        QVariantMap m;
        m[QStringLiteral("displayName")] = vs.displayName;
        m[QStringLiteral("paName")] = vs.paName;
        m[QStringLiteral("loopbackTargets")] = vs.loopbackTargets;
        m[QStringLiteral("inputSources")] = vs.inputSources;
        m[QStringLiteral("networkEnabled")] = vs.network.enabled;
        m[QStringLiteral("channel")] = vs.network.channel;
        m[QStringLiteral("passphrase")] = vs.network.passphrase;
        m[QStringLiteral("staticPeers")] = vs.network.staticPeers;
        result.append(m);
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Reconcile
// ═══════════════════════════════════════════════════════════════════════════

void RoutingEngine::scheduleReconcile()
{
    if (!m_reconcileTimer.isActive())
        m_reconcileTimer.start();
}

void RoutingEngine::onServerDisconnected()
{
    // Everything we created died with the server.  Network nodes keep their
    // sockets; their audio streams are reopened by the next pass.
    m_virtualSinks.clear();
    m_combines.clear();
    m_managedStreams.clear();
    m_managedRecorders.clear();
    m_moveAttempts.clear();
    m_recorderMoveAttempts.clear();
    emit connectionsChanged();
}

void RoutingEngine::stopNetworkNode(const QString &paName)
{
    m_backend->closeNetworkCapture(paName);
    m_backend->closeNetworkPlayback(paName);
    if (m_net)
        m_net->stopNode(paName);
    m_netNodes.remove(paName);
}

bool RoutingEngine::allowMove(QHash<uint32_t, MoveAttempts> &all, uint32_t stream,
                              const QString &appName, const QString &target)
{
    // Something else (a session-manager rule, another tool) may keep moving
    // the stream back.  Do not turn that into an endless tug of war.
    constexpr int kMaxAttempts = 6;
    constexpr qint64 kWindowMs = 10000;

    MoveAttempts &attempts = all[stream];
    const qint64 now = m_clock.elapsed();
    if (attempts.target != target || now - attempts.since > kWindowMs)
        attempts = { target, 0, now };

    if (attempts.count >= kMaxAttempts) {
        if (attempts.count == kMaxAttempts) {
            qWarning() << "SoundRoot: stream" << stream << "(" << appName
                       << ") does not stay on" << target << "- backing off";
            ++attempts.count;
        }
        return false;
    }
    ++attempts.count;
    return true;
}

bool RoutingEngine::tryMove(const PASinkInputInfo &stream, uint32_t sinkIndex,
                            const QString &sinkName)
{
    if (!allowMove(m_moveAttempts, stream.index, stream.appName, sinkName))
        return false;
    m_backend->moveSinkInput(stream.index, sinkIndex);
    return true;
}

void RoutingEngine::reconcileRecorders(const AudioGroup *room)
{
    const auto sources = m_backend->availableSources();
    const auto recorders = m_backend->availableSourceOutputs();

    QHash<QString, uint32_t> sourceIndex;
    for (const auto &s : sources)
        sourceIndex.insert(s.name, s.index);
    QHash<uint32_t, const PASourceOutputInfo *> byIndex;
    for (const auto &r : recorders)
        byIndex.insert(r.index, &r);

    // Recording application → the source it should record
    QHash<uint32_t, QString> desired;
    if (room) {
        const auto &bindings = captureBindingsFor(*room);
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            for (const auto &cap : room->captures) {
                if (cap.key == it.value()) { desired.insert(it.key(), cap.sourceName); break; }
            }
        }
    }

    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const PASourceOutputInfo *rec = byIndex.value(it.key());
        if (!rec) continue;
        m_managedRecorders.insert(rec->index);
        // An unplugged microphone, or a node microphone that is still
        // loading: nothing to do now; its arrival triggers the next pass.
        const auto target = sourceIndex.constFind(it.value());
        if (target == sourceIndex.constEnd()) continue;
        if (rec->sourceIndex == target.value())
            m_recorderMoveAttempts.remove(rec->index);
        else if (allowMove(m_recorderMoveAttempts, rec->index, rec->appName, it.value()))
            m_backend->moveSourceOutput(rec->index, target.value());
    }

    // Applications that lost their wire record the default microphone again
    QString fallback = m_backend->defaultSourceName();
    if (!sourceIndex.contains(fallback) || isInternalSourceName(fallback)) {
        fallback.clear();
        for (const auto &s : sources) {
            if (!isInternalSourceName(s.name)) { fallback = s.name; break; }
        }
    }
    for (auto it = m_managedRecorders.begin(); it != m_managedRecorders.end();) {
        if (desired.contains(*it)) { ++it; continue; }
        const PASourceOutputInfo *rec = byIndex.value(*it);
        if (rec && !fallback.isEmpty() && rec->sourceIndex != sourceIndex.value(fallback))
            m_backend->moveSourceOutput(rec->index, sourceIndex.value(fallback));
        m_recorderMoveAttempts.remove(*it);
        it = m_managedRecorders.erase(it);
    }

    for (auto it = m_recorderMoveAttempts.begin(); it != m_recorderMoveAttempts.end();) {
        if (byIndex.contains(it.key())) ++it;
        else it = m_recorderMoveAttempts.erase(it);
    }
}

void RoutingEngine::reconcile()
{
    m_reconcileTimer.stop();
    if (!m_backend->isConnected()) return;

    const auto sinks = m_backend->availableSinks();
    const auto inputs = m_backend->availableSinkInputs();
    AudioGroup *room = activeGroup();

    QHash<QString, uint32_t> sinkIndex;
    for (const auto &s : sinks)
        sinkIndex.insert(s.name, s.index);
    QHash<uint32_t, const PASinkInputInfo *> inputByIndex;
    for (const auto &in : inputs)
        inputByIndex.insert(in.index, &in);

    // Only sinks that exist right now can be targets.  A saved output that is
    // unplugged stays in the configuration and is used again when it returns.
    auto present = [&sinkIndex](const QStringList &names) {
        QStringList out;
        for (const auto &n : names) {
            if (sinkIndex.contains(n) && !out.contains(n))
                out.append(n);
        }
        return out;
    };

    rememberSinkLabels();

    QSet<QString> sourcePresent;
    const auto sources = m_backend->availableSources();
    for (const auto &s : sources)
        sourcePresent.insert(s.name);

    QHash<uint32_t, QStringList> desired;   // stream → sinks it should play on

    // ── 1. Virtual and network nodes ────────────────────────────────────
    QSet<QString> wantedVirtual;
    QSet<QString> wantedNet;
    if (room) {
        for (const auto &vs : std::as_const(room->virtualSinks)) {
            wantedVirtual.insert(vs.paName);
            m_backend->createVirtualSink(vs.paName, vs.displayName);
            const QStringList targets = present(vs.loopbackTargets);

            // Microphones into IN, for plain and network nodes alike
            QStringList mics;
            for (const auto &mic : vs.inputSources) {
                if (sourcePresent.contains(mic)) mics.append(mic);
            }
            m_backend->setVirtualSinkInputs(vs.paName, mics);

            if (!vs.isNetwork()) {
                m_backend->setVirtualSinkLoopbacks(vs.paName, targets);
                continue;
            }

            // Network node: what is played into it goes to the network, not
            // to its own outputs.
            m_backend->setVirtualSinkLoopbacks(vs.paName, {});
            if (!m_net || !networkUsable(vs)) continue;

            wantedNet.insert(vs.paName);
            const NetworkController::NodeSpec spec { vs.paName, vs.displayName,
                                                     vs.network.channel, vs.network.passphrase,
                                                     vs.network.staticPeers };
            const auto started = m_netNodes.constFind(vs.paName);
            if (started == m_netNodes.constEnd() || started.value() != spec) {
                m_net->startNode(spec);
                m_netNodes.insert(vs.paName, spec);
            }
            if (sinkIndex.contains(vs.paName))
                m_backend->openNetworkCapture(vs.paName);
            if (targets.isEmpty())
                m_backend->closeNetworkPlayback(vs.paName);
            else
                m_backend->openNetworkPlayback(vs.paName, targets.first());
            m_net->setNodeListening(vs.paName, !targets.isEmpty());

            for (const auto &in : inputs) {
                if (in.netNode == vs.paName && !targets.isEmpty())
                    desired.insert(in.index, targets);
            }
        }
    }
    const QStringList startedNet = m_netNodes.keys();
    for (const auto &pa : startedNet) {
        if (!wantedNet.contains(pa))
            stopNetworkNode(pa);
    }
    for (const auto &pa : std::as_const(m_virtualSinks)) {
        if (!wantedVirtual.contains(pa))
            m_backend->destroyVirtualSink(pa);
    }
    m_virtualSinks = wantedVirtual;

    // ── 2. Application streams ──────────────────────────────────────────
    if (room) {
        const auto &bindings = bindingsFor(*room);
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            if (const AudioRoute *route = room->route(it.value()))
                desired.insert(it.key(), route->outputSinkNames);
        }
    }

    // ── 3. Put every stream where it belongs ────────────────────────────
    QSet<QString> neededCombines;
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const PASinkInputInfo *in = inputByIndex.value(it.key());
        if (!in) continue;
        m_managedStreams.insert(in->index);

        const QStringList targets = present(it.value());
        if (targets.isEmpty()) continue;   // nothing to play on right now

        QString targetName = targets.first();
        if (targets.size() > 1) {
            targetName = combineName(targets);
            neededCombines.insert(targetName);
            m_backend->createCombinedSink(targetName, targets);
        }

        const auto target = sinkIndex.constFind(targetName);
        if (target == sinkIndex.constEnd())
            continue;   // combine-sink still loading; its arrival triggers the next pass
        if (in->sinkIndex == target.value())
            m_moveAttempts.remove(in->index);
        else
            tryMove(*in, target.value(), targetName);
    }

    // ── 4. Streams that lost their route return to the default output ───
    QString fallback = m_backend->defaultSinkName();
    if (!sinkIndex.contains(fallback) || isInternalSinkName(fallback)) {
        fallback.clear();
        for (const auto &s : sinks) {
            if (!isInternalSinkName(s.name)) { fallback = s.name; break; }
        }
    }
    for (auto it = m_managedStreams.begin(); it != m_managedStreams.end();) {
        if (desired.contains(*it)) { ++it; continue; }
        const PASinkInputInfo *in = inputByIndex.value(*it);
        if (in && in->netNode.isEmpty() && !fallback.isEmpty()
            && in->sinkIndex != sinkIndex.value(fallback))
            m_backend->moveSinkInput(in->index, sinkIndex.value(fallback));
        m_moveAttempts.remove(*it);
        it = m_managedStreams.erase(it);
    }

    // ── 5. Unload combine-sinks nothing uses any more ───────────────────
    for (auto it = m_combines.begin(); it != m_combines.end();) {
        if (neededCombines.contains(*it)) { ++it; continue; }
        // A stream that is about to move may still sit on it; unloading now
        // would drop that stream onto the default output for a moment.
        const uint32_t idx = sinkIndex.value(*it, kInvalidIndex);
        const bool stillCarrying = idx != kInvalidIndex
            && std::any_of(inputs.cbegin(), inputs.cend(), [&](const PASinkInputInfo &in) {
                   return in.sinkIndex == idx && desired.contains(in.index);
               });
        if (stillCarrying) { ++it; continue; }
        m_backend->destroyCombinedSink(*it);
        it = m_combines.erase(it);
    }
    m_combines.unite(neededCombines);

    // Forget bookkeeping of streams that ended
    for (auto it = m_moveAttempts.begin(); it != m_moveAttempts.end();) {
        if (inputByIndex.contains(it.key())) ++it;
        else it = m_moveAttempts.erase(it);
    }

    // ── 6. Recording applications ───────────────────────────────────────
    reconcileRecorders(room);
}
