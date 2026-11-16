#pragma once

#include <QHash>
#include <QSet>

#include "audiotypes.h"
#include "routing/routingengine.h"

/**
 * An in-memory sound server for the routing tests.  It behaves like the real
 * one where it matters to the engine: moves take effect, a destroyed sink
 * drops its streams onto the default sink, and combine-sinks can be made to
 * appear late, the way module loads do.
 */
class FakeBackend : public AudioBackend {
    Q_OBJECT

public:
    bool connected = true;
    bool asyncCombine = false;   ///< combine-sinks only appear on flush()
    bool refuseMoves = false;    ///< moves are accepted but have no effect

    QVector<PASinkInfo>      sinks;
    QVector<PASinkInputInfo> inputs;
    QString                  defaultSink;
    uint32_t                 nextIndex = 100;

    int moveCalls = 0;
    QHash<QString, QStringList> combines;     // name → slaves
    QHash<QString, QStringList> loopbacks;    // virtual sink → targets
    QSet<QString>               virtualSinks;
    QSet<QString>               captures;
    QHash<QString, QStringList> pendingCombines;

    // ── Test controls ───────────────────────────────────────────────────
    uint32_t addSink(const QString &name, const QString &description = {})
    {
        PASinkInfo s;
        s.index = nextIndex++;
        s.name = name;
        s.description = description.isEmpty() ? name : description;
        sinks.append(s);
        if (defaultSink.isEmpty()) defaultSink = name;
        emit sinksChanged();
        return s.index;
    }

    void removeSink(const QString &name)
    {
        const uint32_t idx = sinkIndexOf(name);
        sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
                                   [&](const PASinkInfo &s) { return s.name == name; }),
                    sinks.end());
        // The server re-homes streams whose sink went away
        for (auto &in : inputs) {
            if (in.sinkIndex == idx) in.sinkIndex = sinkIndexOf(defaultSink);
        }
        emit sinksChanged();
        emit sinkInputsChanged();
    }

    uint32_t addStream(const QString &app, const QString &media = {}, const QString &sink = {})
    {
        PASinkInputInfo in;
        in.index = nextIndex++;
        in.appName = app;
        in.mediaName = media;
        in.sinkIndex = sinkIndexOf(sink.isEmpty() ? defaultSink : sink);
        inputs.append(in);
        emit sinkInputsChanged();
        return in.index;
    }

    void removeStream(uint32_t index)
    {
        inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                    [&](const PASinkInputInfo &in) { return in.index == index; }),
                     inputs.end());
        emit sinkInputsChanged();
    }

    /// Let combine-sinks that were requested finish loading.
    void flush()
    {
        for (auto it = pendingCombines.cbegin(); it != pendingCombines.cend(); ++it) {
            combines.insert(it.key(), it.value());
            addSink(it.key());
        }
        pendingCombines.clear();
    }

    /// The sound server dies: everything is gone.
    void killServer()
    {
        connected = false;
        sinks.clear(); inputs.clear(); combines.clear(); loopbacks.clear();
        virtualSinks.clear(); captures.clear(); pendingCombines.clear();
        defaultSink.clear();
        emit serverDisconnected();
        emit sinksChanged();
        emit sinkInputsChanged();
    }

    uint32_t sinkIndexOf(const QString &name) const
    {
        for (const auto &s : sinks) {
            if (s.name == name) return s.index;
        }
        return kInvalidIndex;
    }

    /// Name of the sink a stream currently plays on.
    QString sinkOf(uint32_t streamIndex) const
    {
        for (const auto &in : inputs) {
            if (in.index != streamIndex) continue;
            for (const auto &s : sinks) {
                if (s.index == in.sinkIndex) return s.name;
            }
        }
        return {};
    }

    /// Index of a network node's playback stream, or kInvalidIndex.
    uint32_t playbackStream(const QString &paName) const
    {
        for (const auto &in : inputs) {
            if (in.netNode == paName) return in.index;
        }
        return kInvalidIndex;
    }

    // ── AudioBackend ────────────────────────────────────────────────────
    bool isConnected() const override { return connected; }
    QVector<PASinkInfo>      availableSinks() const override { return sinks; }
    QVector<PASinkInputInfo> availableSinkInputs() const override { return inputs; }
    QString                  defaultSinkName() const override { return defaultSink; }

    void moveSinkInput(uint32_t sinkInputIndex, uint32_t sinkIndex) override
    {
        ++moveCalls;
        if (refuseMoves) return;
        for (auto &in : inputs) {
            if (in.index == sinkInputIndex) in.sinkIndex = sinkIndex;
        }
        emit sinkInputsChanged();
    }

    void createCombinedSink(const QString &name, const QStringList &sinkNames) override
    {
        if (combines.contains(name) || pendingCombines.contains(name)) return;
        if (asyncCombine) {
            pendingCombines.insert(name, sinkNames);
        } else {
            combines.insert(name, sinkNames);
            addSink(name);
        }
    }

    void destroyCombinedSink(const QString &name) override
    {
        pendingCombines.remove(name);
        if (combines.remove(name)) removeSink(name);
    }

    void createVirtualSink(const QString &paName, const QString &displayName) override
    {
        if (virtualSinks.contains(paName)) return;
        virtualSinks.insert(paName);
        addSink(paName, displayName);
    }

    void destroyVirtualSink(const QString &paName) override
    {
        loopbacks.remove(paName);
        if (virtualSinks.remove(paName)) removeSink(paName);
    }

    void setVirtualSinkLoopbacks(const QString &paName, const QStringList &targetSinks) override
    {
        if (!virtualSinks.contains(paName)) return;
        if (targetSinks.isEmpty()) loopbacks.remove(paName);
        else loopbacks.insert(paName, targetSinks);
    }

    void openNetworkCapture(const QString &paName) override { captures.insert(paName); }
    void closeNetworkCapture(const QString &paName) override { captures.remove(paName); }

    void openNetworkPlayback(const QString &paName, const QString &sinkName) override
    {
        if (playbackStream(paName) != kInvalidIndex) return;
        PASinkInputInfo in;
        in.index = nextIndex++;
        in.appName = QStringLiteral("SoundRoot");
        in.mediaName = netPlaybackPrefix() + paName;
        in.netNode = paName;
        in.sinkIndex = sinkIndexOf(sinkName);
        inputs.append(in);
        emit sinkInputsChanged();
    }

    void closeNetworkPlayback(const QString &paName) override
    {
        const uint32_t idx = playbackStream(paName);
        if (idx != kInvalidIndex) removeStream(idx);
    }
};

/// Records what the routing engine asks of the network layer.
class FakeNetwork : public NetworkController {
public:
    QHash<QString, NodeSpec> nodes;
    QHash<QString, bool>     listening;
    int starts = 0;
    int stops = 0;

    void startNode(const NodeSpec &spec) override { nodes.insert(spec.paName, spec); ++starts; }
    void stopNode(const QString &paName) override { nodes.remove(paName); listening.remove(paName); ++stops; }
    void setNodeListening(const QString &paName, bool on) override { listening.insert(paName, on); }
};
