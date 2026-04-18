import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid

PlasmaExtras.Representation {
    id: fullRoot

    Layout.preferredWidth:  Kirigami.Units.gridUnit * 72
    Layout.preferredHeight: Kirigami.Units.gridUnit * 36
    Layout.minimumWidth:    Kirigami.Units.gridUnit * 52
    Layout.minimumHeight:   Kirigami.Units.gridUnit * 28

    readonly property var groupModel: Plasmoid.groupModel ?? null
    readonly property var routeModel: Plasmoid.routeModel ?? null
    readonly property var sinkModel: Plasmoid.sinkModel ?? null
    readonly property var sinkInputModel: Plasmoid.sinkInputModel ?? null

    property int selectedRoomIndex: -1

    // ── Connection management ───────────────────────────────────────
    // Each link is { from: "type:name", to: "type:name", color: "#hex" }
    // type is "app" or "sink"
    property var connections: []
    property int nextColorIdx: 0

    // Drag state
    property bool isDragging: false
    property string dragFrom: ""     // "app:Firefox" or "sink:sinkName"
    property real dragStartX: 0
    property real dragStartY: 0
    property real dragEndX: 0
    property real dragEndY: 0
    property int hoveredConnectionIdx: -1
    // masterVolume is the raw PA default-sink volume. readonly — never assigned manually.
    readonly property int masterVolume: Plasmoid.masterVolume ?? 100
    property bool scrollSyncing: false

    readonly property var linkColors: ["#4a90d9", "#50c878", "#e74c3c", "#f39c12", "#9b59b6",
                                       "#1abc9c", "#e67e22", "#2ecc71", "#3498db", "#e91e63"]

    function selectRoom(idx) {
        selectedRoomIndex = idx
        if (fullRoot.routeModel && fullRoot.groupModel) {
            fullRoot.routeModel.groupId = fullRoot.groupModel.groupId(idx)
        }
        rebuildConnectionsFromModel()
    }

    function activateRoomAt(idx) {
        if (!fullRoot.groupModel) return
        Plasmoid.applyGroup(idx)   // C++ enforces single-room + sets active state
        fullRoot.selectRoom(idx)   // Rebuilds per-stream connections from saved routes
        applyPerStreamRouting()    // Apply exact per-stream moves
    }

    function selectNextRoom() {
        if (!fullRoot.groupModel || fullRoot.groupModel.count === 0) return
        var n = fullRoot.groupModel.count
        var next = selectedRoomIndex < 0 ? 0 : (selectedRoomIndex + 1) % n
        activateRoomAt(next)
    }

    function selectPrevRoom() {
        if (!fullRoot.groupModel || fullRoot.groupModel.count === 0) return
        var n = fullRoot.groupModel.count
        var prev = selectedRoomIndex < 0 ? n - 1 : (selectedRoomIndex - 1 + n) % n
        activateRoomAt(prev)
    }

    function getNodeKey(type, name) {
        return type + ":" + name
    }

    function pickColor() {
        var c = linkColors[nextColorIdx % linkColors.length]
        nextColorIdx++
        return c
    }

    // Hit-test: find which node card is at position (mx, my) in connectionArea
    function findNodeAt(mx, my) {
        for (var i = 0; i < appListView.count; i++) {
            var appItem = appListView.itemAtIndex(i)
            if (!appItem) continue
            var ap = appItem.mapToItem(connectionArea, 0, 0)
            if (mx >= ap.x && mx <= ap.x + appItem.width &&
                my >= ap.y && my <= ap.y + appItem.height)
                return appItem.nodeKey
        }
        for (var j = 0; j < outListView.count; j++) {
            var sinkItem = outListView.itemAtIndex(j)
            if (!sinkItem) continue
            var sp = sinkItem.mapToItem(connectionArea, 0, 0)
            if (mx >= sp.x && mx <= sp.x + sinkItem.width &&
                my >= sp.y && my <= sp.y + sinkItem.height)
                return sinkItem.nodeKey
        }
        return ""
    }

    // Find which "group color" a node belongs to (follow connection chain)
    function findGroupColor(nodeKey) {
        for (var i = 0; i < connections.length; i++) {
            if (connections[i].from === nodeKey || connections[i].to === nodeKey)
                return connections[i].color
        }
        return ""
    }

    // Check if two nodes are already in the same connection chain
    function areConnected(a, b) {
        // BFS through connections
        var visited = {}
        var queue = [a]
        visited[a] = true
        while (queue.length > 0) {
            var cur = queue.shift()
            if (cur === b) return true
            for (var i = 0; i < connections.length; i++) {
                var c = connections[i]
                var next = ""
                if (c.from === cur) next = c.to
                else if (c.to === cur) next = c.from
                if (next !== "" && !visited[next]) {
                    visited[next] = true
                    queue.push(next)
                }
            }
        }
        return false
    }

    // Get the group color for a connected component (or pick new one)
    function getGroupColorForLink(fromKey, toKey) {
        var c1 = findGroupColor(fromKey)
        var c2 = findGroupColor(toKey)
        if (c1 !== "") return c1
        if (c2 !== "") return c2
        return pickColor()
    }

    // Look up appName + mediaName for a live input: node key
    function streamMetaForKey(nodeKey) {
        if (nodeKey.indexOf("input:") !== 0) return {appName: "", mediaName: ""}
        var inputIdx = parseInt(nodeKey.substring(6))
        if (!fullRoot.sinkInputModel) return {appName: "", mediaName: ""}
        for (var i = 0; i < fullRoot.sinkInputModel.count; i++) {
            var iidx = fullRoot.sinkInputModel.index(i, 0)
            if (fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 1) === inputIdx)
                return {
                    appName:   fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 2),
                    mediaName: fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 3)
                }
        }
        return {appName: "", mediaName: ""}
    }

    function addConnection(fromKey, toKey) {
        if (fromKey === toKey) return
        // Don't duplicate
        for (var i = 0; i < connections.length; i++) {
            var c = connections[i]
            if ((c.from === fromKey && c.to === toKey) ||
                (c.from === toKey && c.to === fromKey))
                return
        }
        // Merge colors: if they're in different groups, unify to same color
        var color = getGroupColorForLink(fromKey, toKey)
        // Unify: set all connections in both components to same color
        var oldColors = {}
        var c1 = findGroupColor(fromKey)
        var c2 = findGroupColor(toKey)
        if (c1 !== "" && c1 !== color) oldColors[c1] = true
        if (c2 !== "" && c2 !== color) oldColors[c2] = true

        var newConns = connections.slice()
        for (var j = 0; j < newConns.length; j++) {
            if (oldColors[newConns[j].color])
                newConns[j] = { from: newConns[j].from, to: newConns[j].to, color: color,
                                appName: newConns[j].appName || "", mediaName: newConns[j].mediaName || "" }
        }
        // Determine which key is the input side and attach stream metadata
        var inputKey = (fromKey.indexOf("input:") === 0) ? fromKey : toKey
        var meta = streamMetaForKey(inputKey)
        newConns.push({ from: fromKey, to: toKey, color: color,
                        appName: meta.appName, mediaName: meta.mediaName })
        connections = newConns
        syncConnectionsToModel()
        connectionCanvas.requestPaint()
    }

    function removeConnection(idx) {
        var newConns = connections.slice()
        newConns.splice(idx, 1)
        connections = newConns
        syncConnectionsToModel()
        connectionCanvas.requestPaint()
    }

    function removeAllConnectionsFor(nodeKey) {
        var newConns = []
        for (var i = 0; i < connections.length; i++) {
            if (connections[i].from !== nodeKey && connections[i].to !== nodeKey)
                newConns.push(connections[i])
        }
        connections = newConns
        syncConnectionsToModel()
        connectionCanvas.requestPaint()
    }

    // Sync QML connections array → C++ routeModel only.
    // Live PA routing only happens when the room is explicitly activated.
    function syncConnectionsToModel() {
        if (!fullRoot.routeModel) return

        // Build ordinal map: inputIndex → { appName, ordinal }
        // Ordinal = order of appearance among same-app streams
        var appOrdCounters = {}
        var streamInfo = {} // inputIndex → { appName, ordinal }
        if (fullRoot.sinkInputModel) {
            for (var s = 0; s < fullRoot.sinkInputModel.count; s++) {
                var sidx = fullRoot.sinkInputModel.index(s, 0)
                var sInputIdx = fullRoot.sinkInputModel.data(sidx, Qt.UserRole + 1)
                var sAppName  = fullRoot.sinkInputModel.data(sidx, Qt.UserRole + 2)
                if (!(sAppName in appOrdCounters)) appOrdCounters[sAppName] = 0
                streamInfo[sInputIdx] = { appName: sAppName, ordinal: appOrdCounters[sAppName] }
                appOrdCounters[sAppName]++
            }
        }

        // Build per-stream routes: "appName\tordinal" → [sinkNames]
        var streamRoutes = {}

        for (var i = 0; i < connections.length; i++) {
            var c = connections[i]
            var inputKey = ""
            var sinkKey = ""

            if (c.from.indexOf("input:") === 0 && c.to.indexOf("sink:") === 0) {
                inputKey = c.from; sinkKey = c.to
            } else if (c.from.indexOf("sink:") === 0 && c.to.indexOf("input:") === 0) {
                sinkKey = c.from; inputKey = c.to
            } else if (c.from.indexOf("app:") === 0 && c.to.indexOf("sink:") === 0) {
                var legKey = c.from.substring(4) + "\t0"
                var legSink = c.to.substring(5)
                if (!streamRoutes[legKey]) streamRoutes[legKey] = []
                if (streamRoutes[legKey].indexOf(legSink) < 0) streamRoutes[legKey].push(legSink)
                continue
            } else if (c.from.indexOf("sink:") === 0 && c.to.indexOf("app:") === 0) {
                var legKey2 = c.to.substring(4) + "\t0"
                var legSink2 = c.from.substring(5)
                if (!streamRoutes[legKey2]) streamRoutes[legKey2] = []
                if (streamRoutes[legKey2].indexOf(legSink2) < 0) streamRoutes[legKey2].push(legSink2)
                continue
            } else {
                continue
            }

            var inputIdx = parseInt(inputKey.substring(6))
            var sinkName = sinkKey.substring(5)
            var info = streamInfo[inputIdx]
            if (!info) continue

            var routeKey = info.appName + "\t" + info.ordinal
            if (!streamRoutes[routeKey]) streamRoutes[routeKey] = []
            if (streamRoutes[routeKey].indexOf(sinkName) < 0) streamRoutes[routeKey].push(sinkName)
        }

        // Persist routes to model
        while (fullRoot.routeModel.count > 0)
            fullRoot.routeModel.removeRoute(0)
        for (var key in streamRoutes)
            fullRoot.routeModel.addRoute(key, streamRoutes[key])

        // If this room is already active, do per-stream routing directly
        if (fullRoot.selectedRoomIndex >= 0 && fullRoot.groupModel &&
                fullRoot.groupModel.isGroupActive(fullRoot.selectedRoomIndex)) {
            applyPerStreamRouting()
        }
    }

    // Apply per-stream routing from current connections array.
    // Collects all sinks per input, then uses routeStreamToSinks which
    // creates combine-sinks when a stream targets multiple outputs.
    function applyPerStreamRouting() {
        var inputSinks = {} // inputIdx → [sinkName, ...]
        for (var k = 0; k < connections.length; k++) {
            var conn = connections[k]
            var iKey = "", sKey = ""
            if (conn.from.indexOf("input:") === 0 && conn.to.indexOf("sink:") === 0) {
                iKey = conn.from; sKey = conn.to
            } else if (conn.from.indexOf("sink:") === 0 && conn.to.indexOf("input:") === 0) {
                sKey = conn.from; iKey = conn.to
            }
            if (iKey !== "" && sKey !== "") {
                var idx = parseInt(iKey.substring(6))
                var sink = sKey.substring(5)
                if (!inputSinks[idx]) inputSinks[idx] = []
                if (inputSinks[idx].indexOf(sink) < 0) inputSinks[idx].push(sink)
            }
        }

        for (var inputIdx in inputSinks) {
            Plasmoid.routeStreamToSinks(parseInt(inputIdx), inputSinks[inputIdx])
        }
    }

    // Rebuild connections from current routeModel, matching saved per-stream
    // routes (appName\tordinal) to currently running streams by ordinal.
    function rebuildConnectionsFromModel() {
        if (!fullRoot.routeModel) return
        var newConns = []
        nextColorIdx = 0

        // Build ordinal map for current live streams
        var appStreams = {} // appName → [inputIdx, inputIdx, ...]  in model order
        if (fullRoot.sinkInputModel) {
            for (var j = 0; j < fullRoot.sinkInputModel.count; j++) {
                var iidx = fullRoot.sinkInputModel.index(j, 0)
                var inputIdx = fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 1)
                var inputApp = fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 2)
                if (!appStreams[inputApp]) appStreams[inputApp] = []
                appStreams[inputApp].push(inputIdx)
            }
        }

        for (var i = 0; i < fullRoot.routeModel.count; i++) {
            var ridx = fullRoot.routeModel.index(i, 0)
            var sourceKey   = fullRoot.routeModel.data(ridx, Qt.UserRole + 1) // SourceAppRole
            var outputNames = fullRoot.routeModel.data(ridx, Qt.UserRole + 2) // OutputNamesRole
            if (!outputNames || !outputNames.length) continue
            var color = pickColor()

            // Parse "appName\tordinal" or legacy plain "appName"
            var tabPos = sourceKey.indexOf("\t")
            var baseAppName = tabPos >= 0 ? sourceKey.substring(0, tabPos) : sourceKey
            var ordinal     = tabPos >= 0 ? parseInt(sourceKey.substring(tabPos + 1)) : -1

            var streams = appStreams[baseAppName] || []

            if (ordinal >= 0 && ordinal < streams.length) {
                // Per-stream route: connect this specific stream
                var fromKey = "input:" + streams[ordinal]
                var meta = streamMetaForKey(fromKey)
                for (var k = 0; k < outputNames.length; k++)
                    newConns.push({ from: fromKey, to: "sink:" + outputNames[k], color: color,
                                    appName: meta.appName, mediaName: meta.mediaName })
            } else if (ordinal < 0 && streams.length > 0) {
                // Legacy route (no ordinal): connect ALL streams of this app
                for (var j2 = 0; j2 < streams.length; j2++) {
                    var fk2 = "input:" + streams[j2]
                    var meta2 = streamMetaForKey(fk2)
                    for (var k2 = 0; k2 < outputNames.length; k2++)
                        newConns.push({ from: fk2, to: "sink:" + outputNames[k2], color: color,
                                        appName: meta2.appName, mediaName: meta2.mediaName })
                }
            } else {
                // Stream not currently available — store as app: placeholder
                for (var k3 = 0; k3 < outputNames.length; k3++)
                    newConns.push({ from: "app:" + baseAppName, to: "sink:" + outputNames[k3], color: color,
                                    appName: baseAppName, mediaName: "" })
            }
        }
        connections = newConns
        connectionCanvas.requestPaint()
    }

    // Called on reapplyRequested (new stream appeared while room is active).
    // Preserves all live connections unchanged. Patches dead connections
    // (whose input PA index no longer exists) by matching the new stream's
    // appName+mediaName — avoids ordinal-position swap when tabs pause/resume.
    function handleNewStreams() {
        if (!fullRoot.sinkInputModel) return

        // Build map: inputIdx → {appName, mediaName} for all live streams
        var liveStreams = {}
        for (var j = 0; j < fullRoot.sinkInputModel.count; j++) {
            var iidx = fullRoot.sinkInputModel.index(j, 0)
            var idx       = fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 1)
            var appName   = fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 2)
            var mediaName = fullRoot.sinkInputModel.data(iidx, Qt.UserRole + 3)
            liveStreams[idx] = { appName: appName, mediaName: mediaName }
        }

        // From current connections, collect which live input indices are already wired
        var wiredIndices = {}
        for (var k = 0; k < connections.length; k++) {
            var c = connections[k]
            if (c.from.indexOf("input:") === 0) {
                var cidx = parseInt(c.from.substring(6))
                if (liveStreams[cidx]) wiredIndices[cidx] = true
            }
        }

        // Collect brand-new (unwired) live streams as candidates to fill dead slots
        var newStreamCandidates = [] // [{inputIdx, appName, mediaName}]
        for (var nidx in liveStreams) {
            if (!wiredIndices[nidx])
                newStreamCandidates.push({ inputIdx: parseInt(nidx),
                                           appName:   liveStreams[nidx].appName,
                                           mediaName: liveStreams[nidx].mediaName })
        }

        if (newStreamCandidates.length === 0) {
            // No new streams — just re-apply existing connections for any that drifted
            applyPerStreamRouting()
            return
        }

        // Patch dead connections: replace dead input:X with a matching new stream
        var usedCandidates = {}
        var newConns = connections.slice()
        for (var i = 0; i < newConns.length; i++) {
            var conn = newConns[i]
            if (conn.from.indexOf("input:") !== 0) continue
            var connIdx = parseInt(conn.from.substring(6))
            if (liveStreams[connIdx]) continue  // Still live — leave alone

            // Dead connection — find best matching new stream
            // Priority: same appName AND same mediaName (tab title stable across pause/resume)
            // Fallback: same appName only (if mediaName blank or changed)
            var bestMatch = -1
            for (var s = 0; s < newStreamCandidates.length; s++) {
                var cand = newStreamCandidates[s]
                if (usedCandidates[cand.inputIdx]) continue
                if (cand.appName !== (conn.appName || "")) continue
                var mediaMatch = (conn.mediaName && conn.mediaName !== "" &&
                                  cand.mediaName && cand.mediaName !== "" &&
                                  cand.mediaName === conn.mediaName)
                if (mediaMatch) { bestMatch = s; break }
                if (bestMatch < 0) bestMatch = s  // appName-only fallback
            }

            if (bestMatch >= 0) {
                var matched = newStreamCandidates[bestMatch]
                usedCandidates[matched.inputIdx] = true
                newConns[i] = { from: "input:" + matched.inputIdx, to: conn.to,
                                color: conn.color,
                                appName: matched.appName, mediaName: matched.mediaName }
            }
            // If no match found, leave the dead connection (won't be routed)
        }

        connections = newConns
        connectionCanvas.requestPaint()
        applyPerStreamRouting()
    }

    // Get wire endpoint: right-center of app card, left-center of sink card
    function getNodeEdge(nodeKey) {
        var item = null
        if (nodeKey.indexOf("input:") === 0) {
            var inputIdx = parseInt(nodeKey.substring(6))
            for (var i = 0; i < appListView.count; i++) {
                var appItem = appListView.itemAtIndex(i)
                if (appItem && appItem.inputIndex === inputIdx) {
                    item = appItem; break
                }
            }
            if (!item) return null
            return item.mapToItem(connectionArea, item.width, item.height / 2)
        } else if (nodeKey.indexOf("app:") === 0) {
            var appName = nodeKey.substring(4)
            for (var i2 = 0; i2 < appListView.count; i2++) {
                var appItem2 = appListView.itemAtIndex(i2)
                if (appItem2 && appItem2.nodeName === appName) {
                    item = appItem2; break
                }
            }
            if (!item) return null
            return item.mapToItem(connectionArea, item.width, item.height / 2)
        } else if (nodeKey.indexOf("sink:") === 0) {
            var sinkName = nodeKey.substring(5)
            for (var k = 0; k < outListView.count; k++) {
                var sinkItem = outListView.itemAtIndex(k)
                if (sinkItem && sinkItem.nodeName === sinkName) {
                    item = sinkItem; break
                }
            }
            if (!item) return null
            return item.mapToItem(connectionArea, 0, item.height / 2)
        }
        return null
    }

    // Find the connection index nearest to point (mx, my), or -1
    function findConnectionNear(mx, my) {
        var threshold = 10
        for (var i = 0; i < connections.length; i++) {
            var conn = connections[i]
            var p1 = getNodeEdge(conn.from)
            var p2 = getNodeEdge(conn.to)
            if (!p1 || !p2) continue
            // Normalize: app/input left, sink right
            if (conn.from.indexOf("sink:") === 0) {
                var _tmp = p1; p1 = p2; p2 = _tmp
            }
            var cpOff = Math.abs(p2.x - p1.x) * 0.4
            for (var s = 0; s <= 1.0; s += 0.02) {
                var it = 1 - s
                var bx = it*it*it*p1.x + 3*it*it*s*(p1.x+cpOff) + 3*it*s*s*(p2.x-cpOff) + s*s*s*p2.x
                var by = it*it*it*p1.y + 3*it*it*s*p1.y + 3*it*s*s*p2.y + s*s*s*p2.y
                var dx = mx - bx
                var dy = my - by
                if (dx*dx + dy*dy < threshold*threshold)
                    return i
            }
        }
        return -1
    }

    // ════════════════════════════════════════════════════════════════════
    //  MAIN LAYOUT
    // ════════════════════════════════════════════════════════════════════
    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ─────────────────────────────────────────────────────────────
        //  LEFT SIDEBAR — Rooms
        // ─────────────────────────────────────────────────────────────
        Rectangle {
            id: sidebar
            Layout.fillHeight: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 18
            Layout.minimumWidth:   Kirigami.Units.gridUnit * 14
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.6)

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.smallSpacing
                spacing: Kirigami.Units.smallSpacing

                // ── Logo + Title ────────────────────────────────────
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing

                    Image {
                        source: "icon.png"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                        sourceSize: Qt.size(Kirigami.Units.iconSizes.medium,
                                            Kirigami.Units.iconSizes.medium)
                        smooth: true
                    }

                    PlasmaExtras.Heading {
                        level: 3
                        text: "SoundRoot"
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                }

                // ── + New Room button ───────────────────────────────
                PlasmaComponents.Button {
                    Layout.fillWidth: true
                    text: "  + New Room"
                    icon.name: "list-add"
                    onClicked: addRoomOverlay.visible = true

                    background: Rectangle {
                        radius: Kirigami.Units.smallSpacing
                        color: "#4a90d9"
                        opacity: parent.hovered ? 1.0 : 0.85
                    }
                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Icon {
                            source: "list-add"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                            isMask: true
                            Kirigami.Theme.textColor: "white"
                        }
                        PlasmaComponents.Label {
                            text: "New Room"
                            font.bold: true
                            color: "white"
                            Layout.fillWidth: true
                        }
                    }
                }

                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.3 }

                // ── Room list ────────────────────────────────────────
                QQC2.ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ListView {
                        id: roomListView
                        model: fullRoot.groupModel
                        spacing: 2
                        clip: true

                        delegate: Rectangle {
                            id: roomDelegate
                            width: roomListView.width
                            height: roomDelegateRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                            radius: Kirigami.Units.smallSpacing
                            color: fullRoot.selectedRoomIndex === index
                                   ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                             Kirigami.Theme.highlightColor.g,
                                             Kirigami.Theme.highlightColor.b, 0.18)
                                   : (roomMa.containsMouse
                                      ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                                Kirigami.Theme.highlightColor.g,
                                                Kirigami.Theme.highlightColor.b, 0.08)
                                      : "transparent")

                            RowLayout {
                                id: roomDelegateRow
                                anchors.fill: parent
                                anchors.margins: Kirigami.Units.smallSpacing
                                spacing: Kirigami.Units.smallSpacing

                                // Color bar
                                Rectangle {
                                    width: 4
                                    Layout.fillHeight: true
                                    radius: 2
                                    color: model.groupColor || "#4a90d9"
                                    opacity: model.groupActive ? 1.0 : 0.25
                                }

                                // Room icon
                                Kirigami.Icon {
                                    source: "audio-card"
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                                    opacity: model.groupActive ? 1.0 : 0.5
                                }

                                // Name + route count
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    PlasmaComponents.Label {
                                        text: model.groupName
                                        font.bold: true
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                        opacity: model.groupActive ? 1.0 : 0.6
                                    }
                                    PlasmaComponents.Label {
                                        text: model.routeCount + " link" + (model.routeCount !== 1 ? "s" : "")
                                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                                        opacity: 0.5
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }

                                // Active toggle
                                PlasmaComponents.Switch {
                                    checked: model.groupActive
                                    onToggled: {
                                        if (checked) {
                                            fullRoot.activateRoomAt(index)
                                        } else {
                                            Plasmoid.deactivateGroup(index)
                                        }
                                        // Restore binding — user toggle breaks it
                                        checked = Qt.binding(function() { return model.groupActive })
                                    }
                                }

                                // Three-dot menu
                                PlasmaComponents.ToolButton {
                                    icon.name: "overflow-menu"
                                    icon.width: Kirigami.Units.iconSizes.small
                                    icon.height: Kirigami.Units.iconSizes.small
                                    onClicked: roomContextMenu.open()
                                    visible: roomMa.containsMouse || fullRoot.selectedRoomIndex === index

                                    QQC2.Menu {
                                        id: roomContextMenu
                                        QQC2.MenuItem {
                                            text: "Remove"
                                            icon.name: "edit-delete"
                                            onTriggered: {
                                                if (fullRoot.groupModel) {
                                                    fullRoot.groupModel.removeGroup(index)
                                                    if (fullRoot.selectedRoomIndex === index)
                                                        fullRoot.selectedRoomIndex = -1
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                id: roomMa
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton
                                onClicked: fullRoot.selectRoom(index)
                                z: -1
                            }
                        }
                    }
                }

                // ── Settings / Save buttons at bottom ───────────────
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.3 }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    PlasmaComponents.ToolButton {
                        icon.name: "document-save"
                        text: "Save"
                        display: QQC2.AbstractButton.TextBesideIcon
                        onClicked: {
                            if (fullRoot.groupModel) fullRoot.groupModel.save()
                        }
                    }

                    Item { Layout.fillWidth: true }

                    PlasmaComponents.ToolButton {
                        icon.name: "configure"
                        onClicked: Plasmoid.internalAction("configure").trigger()
                    }
                }
            }
        }

        // Sidebar / main separator
        Rectangle {
            Layout.fillHeight: true
            width: 1
            color: Qt.rgba(Kirigami.Theme.textColor.r,
                           Kirigami.Theme.textColor.g,
                           Kirigami.Theme.textColor.b, 0.1)
        }

        // ─────────────────────────────────────────────────────────────
        //  MAIN CONTENT — Connection Board
        // ─────────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // ── Header bar ────────────────────────────────────
                Rectangle {
                    Layout.fillWidth: true
                    height: headerCol.implicitHeight + Kirigami.Units.smallSpacing * 2
                    color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                                   Kirigami.Theme.backgroundColor.g,
                                   Kirigami.Theme.backgroundColor.b, 0.4)

                    ColumnLayout {
                        id: headerCol
                        anchors.fill: parent
                        anchors.margins: Kirigami.Units.smallSpacing
                        spacing: 2

                        RowLayout {
                            id: headerRow
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            PlasmaExtras.Heading {
                                level: 4
                                text: {
                                    if (fullRoot.selectedRoomIndex < 0) return "Select a Room"
                                    var gm = fullRoot.groupModel
                                    if (!gm) return "Room"
                                    var idx = gm.index(fullRoot.selectedRoomIndex, 0)
                                    var n = gm.data(idx, Qt.DisplayRole) || "Room"
                                    return n + "  —  drag to connect"
                                }
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.9
                            }

                            PlasmaComponents.Button {
                                visible: fullRoot.selectedRoomIndex >= 0 && fullRoot.connections.length > 0
                                text: "Clear All"
                                icon.name: "edit-clear-all"
                                onClicked: {
                                    fullRoot.connections = []
                                    fullRoot.syncConnectionsToModel()
                                    connectionCanvas.requestPaint()
                                }
                            }
                        }

                        // Master volume moved to bottom of the popup (see below)
                    }
                }

                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.2 }

                // ── No room selected state ──────────────────────────
                PlasmaExtras.PlaceholderMessage {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: fullRoot.selectedRoomIndex < 0
                    text: "Select a Room"
                    explanation: "Choose a room from the sidebar\nor create a new one.\n\nDrag from an app to an output device\nto create audio connections."
                    iconName: "audio-volume-high"
                }

                // ── Connection board: APPS | wires | OUTPUTS ────────
                Item {
                    id: connectionArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: fullRoot.selectedRoomIndex >= 0

                    // ── Canvas for drawing connection lines ─────────
                    Canvas {
                        id: connectionCanvas
                        anchors.fill: parent
                        z: 5
                        // We must not block mouse events for items below
                        // We'll use an overlay MouseArea for drag only

                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)

                            // Draw existing connections
                            for (var i = 0; i < fullRoot.connections.length; i++) {
                                var conn = fullRoot.connections[i]
                                var p1 = fullRoot.getNodeEdge(conn.from)
                                var p2 = fullRoot.getNodeEdge(conn.to)
                                if (!p1 || !p2) continue
                                // Normalize: app/input is always left (p1), sink always right (p2)
                                // so the bezier curve bows inward naturally
                                if (conn.from.indexOf("sink:") === 0) {
                                    var _tmp = p1; p1 = p2; p2 = _tmp
                                }

                                var isHovered = (i === fullRoot.hoveredConnectionIdx)
                                ctx.strokeStyle = isHovered ? "#ff4444" : conn.color
                                ctx.lineWidth = isHovered ? 4 : 2.5
                                ctx.globalAlpha = isHovered ? 0.95 : 0.7
                                ctx.beginPath()
                                ctx.moveTo(p1.x, p1.y)
                                var cpOffset = Math.abs(p2.x - p1.x) * 0.4
                                ctx.bezierCurveTo(p1.x + cpOffset, p1.y,
                                                  p2.x - cpOffset, p2.y,
                                                  p2.x, p2.y)
                                ctx.stroke()

                                // Draw small circles at endpoints
                                ctx.globalAlpha = isHovered ? 1.0 : 0.9
                                ctx.fillStyle = isHovered ? "#ff4444" : conn.color
                                ctx.beginPath()
                                ctx.arc(p1.x, p1.y, isHovered ? 5 : 4, 0, 2 * Math.PI)
                                ctx.fill()
                                ctx.beginPath()
                                ctx.arc(p2.x, p2.y, isHovered ? 5 : 4, 0, 2 * Math.PI)
                                ctx.fill()
                            }

                            // Draw drag line
                            if (fullRoot.isDragging) {
                                ctx.strokeStyle = "#ffffff"
                                ctx.globalAlpha = 0.6
                                ctx.lineWidth = 2
                                ctx.setLineDash([6, 4])
                                ctx.beginPath()
                                ctx.moveTo(fullRoot.dragStartX, fullRoot.dragStartY)
                                var dx = Math.abs(fullRoot.dragEndX - fullRoot.dragStartX) * 0.4
                                ctx.bezierCurveTo(fullRoot.dragStartX + dx, fullRoot.dragStartY,
                                                  fullRoot.dragEndX - dx, fullRoot.dragEndY,
                                                  fullRoot.dragEndX, fullRoot.dragEndY)
                                ctx.stroke()
                                ctx.setLineDash([])
                            }
                            ctx.globalAlpha = 1.0
                        }
                    }

                    // ── Three-column layout: Apps | Center | Outputs ──
                    RowLayout {
                        anchors.fill: parent
                        spacing: 0
                        z: 1

                        // ════ APPLICATIONS COLUMN ════════════════════
                        ColumnLayout {
                            Layout.fillHeight: true
                            Layout.preferredWidth: parent.width * 0.35
                            Layout.maximumWidth: parent.width * 0.4
                            spacing: 0

                            Rectangle {
                                Layout.fillWidth: true
                                height: appHeaderLbl.implicitHeight + Kirigami.Units.smallSpacing * 2
                                color: "transparent"

                                PlasmaComponents.Label {
                                    id: appHeaderLbl
                                    anchors.centerIn: parent
                                    text: "APPLICATIONS"
                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 1.5
                                    opacity: 0.5
                                }
                            }

                            Kirigami.Separator { Layout.fillWidth: true; opacity: 0.15 }

                            QQC2.ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                ListView {
                                    id: appListView
                                    model: fullRoot.sinkInputModel
                                    clip: true
                                    spacing: 4
                                    topMargin: Kirigami.Units.smallSpacing
                                    leftMargin: Kirigami.Units.smallSpacing
                                    rightMargin: Kirigami.Units.smallSpacing

                                    onContentYChanged: {
                                        if (!fullRoot.scrollSyncing) {
                                            fullRoot.scrollSyncing = true
                                            outListView.contentY = contentY
                                            fullRoot.scrollSyncing = false
                                        }
                                        connectionCanvas.requestPaint()
                                    }

                                    delegate: Rectangle {
                                        id: appCard
                                        property string nodeName: model.appName || ""
                                        property int inputIndex: model.inputIndex || 0
                                        property string nodeKey: "input:" + inputIndex
                                        width: appListView.width - appListView.leftMargin - appListView.rightMargin
                                        height: appCardRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                                        radius: Kirigami.Units.smallSpacing
                                        color: {
                                            var gc = fullRoot.findGroupColor(nodeKey)
                                            if (gc !== "")
                                                return Qt.rgba(Qt.color(gc).r, Qt.color(gc).g, Qt.color(gc).b, 0.15)
                                            return Qt.rgba(Kirigami.Theme.backgroundColor.r,
                                                           Kirigami.Theme.backgroundColor.g,
                                                           Kirigami.Theme.backgroundColor.b, 0.35)
                                        }
                                        border.width: fullRoot.isDragging && fullRoot.dragFrom !== nodeKey ? 2 : 1
                                        border.color: {
                                            if (fullRoot.isDragging && fullRoot.dragFrom !== nodeKey)
                                                return Qt.rgba(1, 1, 1, 0.3)
                                            var gc = fullRoot.findGroupColor(nodeKey)
                                            if (gc !== "") return gc
                                            return Qt.rgba(Kirigami.Theme.textColor.r,
                                                           Kirigami.Theme.textColor.g,
                                                           Kirigami.Theme.textColor.b, 0.06)
                                        }

                                        RowLayout {
                                            id: appCardRow
                                            anchors.fill: parent
                                            anchors.margins: Kirigami.Units.smallSpacing
                                            spacing: Kirigami.Units.smallSpacing

                                            Kirigami.Icon {
                                                source: model.iconName || "applications-multimedia"
                                                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 0
                                                PlasmaComponents.Label {
                                                    Layout.fillWidth: true
                                                    text: model.appName || "Unknown"
                                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                                    font.bold: true
                                                    elide: Text.ElideRight
                                                }
                                                PlasmaComponents.Label {
                                                    Layout.fillWidth: true
                                                    visible: model.mediaName && model.mediaName !== model.appName
                                                    text: model.mediaName || ""
                                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                                    opacity: 0.65
                                                    elide: Text.ElideRight
                                                }
                                            }

                                            // Connection dot / drag handle
                                            Rectangle {
                                                id: appDragDot
                                                width: 14; height: 14; radius: 7
                                                color: {
                                                    var gc = fullRoot.findGroupColor(appCard.nodeKey)
                                                    return gc !== "" ? gc : "#888"
                                                }
                                                border.width: 2
                                                border.color: Qt.lighter(color, 1.4)

                                                MouseArea {
                                                    anchors.fill: parent
                                                    anchors.margins: -8
                                                    cursorShape: Qt.CrossCursor
                                                    preventStealing: true
                                                    onPressed: function(mouse) {
                                                        var mapped = appDragDot.mapToItem(connectionArea, appDragDot.width / 2, appDragDot.height / 2)
                                                        fullRoot.isDragging = true
                                                        fullRoot.dragFrom = appCard.nodeKey
                                                        fullRoot.dragStartX = mapped.x
                                                        fullRoot.dragStartY = mapped.y
                                                        fullRoot.dragEndX = mapped.x
                                                        fullRoot.dragEndY = mapped.y
                                                        connectionCanvas.requestPaint()
                                                    }
                                                    onPositionChanged: function(mouse) {
                                                        if (fullRoot.isDragging) {
                                                            var pos = mapToItem(connectionArea, mouse.x, mouse.y)
                                                            fullRoot.dragEndX = pos.x
                                                            fullRoot.dragEndY = pos.y
                                                            connectionCanvas.requestPaint()
                                                        }
                                                    }
                                                    onReleased: function(mouse) {
                                                        if (fullRoot.isDragging) {
                                                            var pos = mapToItem(connectionArea, mouse.x, mouse.y)
                                                            var target = fullRoot.findNodeAt(pos.x, pos.y)
                                                            if (target !== "" && target !== fullRoot.dragFrom) {
                                                                fullRoot.addConnection(fullRoot.dragFrom, target)
                                                                fullRoot.isDragging = false
                                                                fullRoot.dragFrom = ""
                                                                connectionCanvas.requestPaint()
                                                            }
                                                            // else: keep isDragging=true → click-click mode via dragTracker
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // Drop target
                                        DropArea {
                                            anchors.fill: parent
                                            keys: ["audio-node"]
                                            onDropped: {
                                                if (fullRoot.isDragging && fullRoot.dragFrom !== appCard.nodeKey) {
                                                    fullRoot.addConnection(fullRoot.dragFrom, appCard.nodeKey)
                                                }
                                            }
                                        }

                                        // Right-click to remove connections
                                        MouseArea {
                                            anchors.fill: parent
                                            acceptedButtons: Qt.RightButton
                                            onClicked: {
                                                fullRoot.removeAllConnectionsFor(appCard.nodeKey)
                                            }
                                            z: -1
                                        }
                                    }

                                    PlasmaExtras.PlaceholderMessage {
                                        anchors.centerIn: parent
                                        width: parent.width - Kirigami.Units.gridUnit * 2
                                        visible: appListView.count === 0
                                        text: "No audio streams"
                                        explanation: "Play something to see\nactive applications."
                                        iconName: "applications-multimedia"
                                    }
                                }
                            }
                        }

                        // ════ CENTER SPACER (wires drawn on canvas) ══
                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: Kirigami.Units.gridUnit * 6

                            // Help text when no connections
                            PlasmaComponents.Label {
                                anchors.centerIn: parent
                                visible: fullRoot.connections.length === 0
                                text: "← Drag from dot →\nto connect"
                                opacity: 0.3
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                horizontalAlignment: Text.AlignHCenter
                            }

                            // Relay scroll events to the app list so the center area also scrolls
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                onWheel: function(wheel) {
                                    if (!fullRoot.scrollSyncing) {
                                        appListView.flick(0, wheel.angleDelta.y * 5)
                                    }
                                }
                                z: -1
                            }
                        }

                        // ════ OUTPUT DEVICES COLUMN ══════════════════
                        ColumnLayout {
                            Layout.fillHeight: true
                            Layout.preferredWidth: parent.width * 0.35
                            Layout.maximumWidth: parent.width * 0.4
                            spacing: 0

                            Rectangle {
                                Layout.fillWidth: true
                                height: outHeaderLbl.implicitHeight + Kirigami.Units.smallSpacing * 2
                                color: "transparent"

                                PlasmaComponents.Label {
                                    id: outHeaderLbl
                                    anchors.centerIn: parent
                                    text: "OUTPUT DEVICES"
                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 1.5
                                    opacity: 0.5
                                }
                            }

                            Kirigami.Separator { Layout.fillWidth: true; opacity: 0.15 }

                            QQC2.ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                ListView {
                                    id: outListView
                                    model: fullRoot.sinkModel
                                    clip: true
                                    spacing: 4
                                    topMargin: Kirigami.Units.smallSpacing
                                    leftMargin: Kirigami.Units.smallSpacing
                                    rightMargin: Kirigami.Units.smallSpacing

                                        onContentYChanged: {
                                            if (!fullRoot.scrollSyncing) {
                                                fullRoot.scrollSyncing = true
                                                appListView.contentY = contentY
                                                fullRoot.scrollSyncing = false
                                            }
                                            connectionCanvas.requestPaint()
                                        }

                                    delegate: Rectangle {
                                        id: sinkCard
                                        property string nodeName: model.sinkName || ""
                                        property string nodeKey: "sink:" + nodeName
                                        width: outListView.width - outListView.leftMargin - outListView.rightMargin
                                        height: sinkCardCol.implicitHeight + Kirigami.Units.smallSpacing * 2
                                        radius: Kirigami.Units.smallSpacing
                                        color: {
                                            var gc = fullRoot.findGroupColor(nodeKey)
                                            if (gc !== "")
                                                return Qt.rgba(Qt.color(gc).r, Qt.color(gc).g, Qt.color(gc).b, 0.15)
                                            return Qt.rgba(Kirigami.Theme.backgroundColor.r,
                                                           Kirigami.Theme.backgroundColor.g,
                                                           Kirigami.Theme.backgroundColor.b, 0.35)
                                        }
                                        border.width: fullRoot.isDragging && fullRoot.dragFrom !== nodeKey ? 2 : 1
                                        border.color: {
                                            if (fullRoot.isDragging && fullRoot.dragFrom !== nodeKey)
                                                return Qt.rgba(1, 1, 1, 0.3)
                                            var gc = fullRoot.findGroupColor(nodeKey)
                                            if (gc !== "") return gc
                                            return Qt.rgba(Kirigami.Theme.textColor.r,
                                                           Kirigami.Theme.textColor.g,
                                                           Kirigami.Theme.textColor.b, 0.06)
                                        }

                                        ColumnLayout {
                                            id: sinkCardCol
                                            anchors.fill: parent
                                            anchors.margins: Kirigami.Units.smallSpacing
                                            spacing: Kirigami.Units.smallSpacing

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: Kirigami.Units.smallSpacing

                                                // Drag dot on left side of output
                                                Rectangle {
                                                    id: sinkDragDot
                                                    width: 14; height: 14; radius: 7
                                                    color: {
                                                        var gc = fullRoot.findGroupColor(sinkCard.nodeKey)
                                                        return gc !== "" ? gc : "#888"
                                                    }
                                                    border.width: 2
                                                    border.color: Qt.lighter(color, 1.4)

                                                    MouseArea {
                                                        anchors.fill: parent
                                                        anchors.margins: -8
                                                        cursorShape: Qt.CrossCursor
                                                        preventStealing: true
                                                        onPressed: function(mouse) {
                                                            var mapped = sinkDragDot.mapToItem(connectionArea, sinkDragDot.width / 2, sinkDragDot.height / 2)
                                                            fullRoot.isDragging = true
                                                            fullRoot.dragFrom = sinkCard.nodeKey
                                                            fullRoot.dragStartX = mapped.x
                                                            fullRoot.dragStartY = mapped.y
                                                            fullRoot.dragEndX = mapped.x
                                                            fullRoot.dragEndY = mapped.y
                                                            connectionCanvas.requestPaint()
                                                        }
                                                        onPositionChanged: function(mouse) {
                                                            if (fullRoot.isDragging) {
                                                                var pos = mapToItem(connectionArea, mouse.x, mouse.y)
                                                                fullRoot.dragEndX = pos.x
                                                                fullRoot.dragEndY = pos.y
                                                                connectionCanvas.requestPaint()
                                                            }
                                                        }
                                                        onReleased: function(mouse) {
                                                            if (fullRoot.isDragging) {
                                                                var pos = mapToItem(connectionArea, mouse.x, mouse.y)
                                                                var target = fullRoot.findNodeAt(pos.x, pos.y)
                                                                if (target !== "" && target !== fullRoot.dragFrom) {
                                                                    fullRoot.addConnection(fullRoot.dragFrom, target)
                                                                    fullRoot.isDragging = false
                                                                    fullRoot.dragFrom = ""
                                                                    connectionCanvas.requestPaint()
                                                                }
                                                                // else: keep isDragging=true → click-click mode via dragTracker
                                                            }
                                                        }
                                                    }
                                                }

                                                Kirigami.Icon {
                                                    source: {
                                                        var desc = (model.sinkDescription || "").toLowerCase()
                                                        if (desc.indexOf("headphone") >= 0 || desc.indexOf("headset") >= 0)
                                                            return "audio-headphones"
                                                        if (desc.indexOf("speaker") >= 0)
                                                            return "audio-speakers"
                                                        if (desc.indexOf("hdmi") >= 0 || desc.indexOf("monitor") >= 0)
                                                            return "monitor"
                                                        return "audio-card"
                                                    }
                                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                                                }

                                                PlasmaComponents.Label {
                                                    Layout.fillWidth: true
                                                    text: model.sinkDescription || model.sinkName
                                                    font.bold: true
                                                    elide: Text.ElideRight
                                                }
                                            }

                                            // ── Per-device volume slider ───────────
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: Kirigami.Units.smallSpacing

                                                // Mute toggle button
                                                PlasmaComponents.ToolButton {
                                                    checkable: true
                                                    checked: model.sinkMuted
                                                    icon.name: model.sinkMuted ? "audio-volume-muted"
                                                               : sinkSlider.value === 0 ? "audio-volume-muted"
                                                               : sinkSlider.value < 40  ? "audio-volume-low"
                                                                                        : "audio-volume-medium"
                                                    Layout.preferredWidth:  Kirigami.Units.iconSizes.small + Kirigami.Units.smallSpacing * 2
                                                    Layout.preferredHeight: Kirigami.Units.iconSizes.small + Kirigami.Units.smallSpacing * 2
                                                    onClicked: Plasmoid.toggleSinkMute(sinkCard.nodeName)
                                                }
                                                QQC2.Slider {
                                                    id: sinkSlider
                                                    Layout.fillWidth: true
                                                    from: 0; to: 150
                                                    enabled: !model.sinkMuted
                                                    opacity: model.sinkMuted ? 0.4 : 1.0
                                                    property int backendVol: model.sinkVolumePercent ?? 100
                                                    onBackendVolChanged: if (!pressed) value = backendVol
                                                    Component.onCompleted: value = backendVol
                                                    onMoved: Plasmoid.setSinkVolumeByName(sinkCard.nodeName, Math.round(value))
                                                }
                                                PlasmaComponents.Label {
                                                    text: model.sinkMuted ? "muted" : Math.round(sinkSlider.value) + "%"
                                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                                    opacity: model.sinkMuted ? 0.5 : 0.65
                                                    Layout.minimumWidth: Kirigami.Units.gridUnit * 2.5
                                                }
                                            }
                                        }

                                        // Drop target
                                        DropArea {
                                            anchors.fill: parent
                                            keys: ["audio-node"]
                                            onDropped: {
                                                if (fullRoot.isDragging && fullRoot.dragFrom !== sinkCard.nodeKey) {
                                                    fullRoot.addConnection(fullRoot.dragFrom, sinkCard.nodeKey)
                                                }
                                            }
                                        }

                                        // Right-click to remove connections
                                        MouseArea {
                                            anchors.fill: parent
                                            acceptedButtons: Qt.RightButton
                                            onClicked: {
                                                fullRoot.removeAllConnectionsFor(sinkCard.nodeKey)
                                            }
                                            z: -1
                                        }
                                    }

                                    PlasmaExtras.PlaceholderMessage {
                                        anchors.centerIn: parent
                                        width: parent.width - Kirigami.Units.gridUnit * 2
                                        visible: outListView.count === 0
                                        text: "No output devices"
                                        explanation: "No PulseAudio sinks detected."
                                        iconName: "audio-speakers"
                                    }
                                }
                            }
                        }
                    }

                    // ── Connection hover detection (click to delete) ──
                    MouseArea {
                        id: connectionHoverArea
                        anchors.fill: parent
                        z: 6
                        enabled: !fullRoot.isDragging
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        cursorShape: fullRoot.hoveredConnectionIdx >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor

                        onPositionChanged: function(mouse) {
                            var idx = fullRoot.findConnectionNear(mouse.x, mouse.y)
                            if (idx !== fullRoot.hoveredConnectionIdx) {
                                fullRoot.hoveredConnectionIdx = idx
                                connectionCanvas.requestPaint()
                            }
                        }

                        onExited: {
                            if (fullRoot.hoveredConnectionIdx >= 0) {
                                fullRoot.hoveredConnectionIdx = -1
                                connectionCanvas.requestPaint()
                            }
                        }

                        onPressed: function(mouse) {
                            if (fullRoot.hoveredConnectionIdx >= 0) {
                                fullRoot.removeConnection(fullRoot.hoveredConnectionIdx)
                                fullRoot.hoveredConnectionIdx = -1
                            } else {
                                mouse.accepted = false
                            }
                        }
                    }

                    // ── Drag tracker: follows cursor in click-click mode ──
                    MouseArea {
                        id: dragTracker
                        anchors.fill: parent
                        z: 10
                        visible: fullRoot.isDragging
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        cursorShape: Qt.CrossCursor

                        onPositionChanged: function(mouse) {
                            if (fullRoot.isDragging) {
                                var pos = mapToItem(connectionArea, mouse.x, mouse.y)
                                fullRoot.dragEndX = pos.x
                                fullRoot.dragEndY = pos.y
                                connectionCanvas.requestPaint()
                            }
                        }
                        onClicked: function(mouse) {
                            if (!fullRoot.isDragging) return
                            if (mouse.button === Qt.RightButton) {
                                // Cancel drag
                                fullRoot.isDragging = false
                                fullRoot.dragFrom = ""
                                connectionCanvas.requestPaint()
                                return
                            }
                            var pos = mapToItem(connectionArea, mouse.x, mouse.y)
                            var target = fullRoot.findNodeAt(pos.x, pos.y)
                            if (target !== "" && target !== fullRoot.dragFrom) {
                                fullRoot.addConnection(fullRoot.dragFrom, target)
                            }
                            fullRoot.isDragging = false
                            fullRoot.dragFrom = ""
                            connectionCanvas.requestPaint()
                        }
                    }
                }

                // ── Master volume ── bottom of room content area ──────────────
                Kirigami.Separator {
                    Layout.fillWidth: true
                    opacity: 0.2
                    visible: fullRoot.selectedRoomIndex >= 0
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin:  Kirigami.Units.smallSpacing
                    Layout.rightMargin: Kirigami.Units.smallSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    visible: fullRoot.selectedRoomIndex >= 0
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        source: fullRoot.masterVolume === 0 ? "audio-volume-muted"
                              : fullRoot.masterVolume < 40  ? "audio-volume-low"
                              : fullRoot.masterVolume < 75  ? "audio-volume-medium"
                              : "audio-volume-high"
                        Layout.preferredWidth:  Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        opacity: 0.75
                    }
                    PlasmaComponents.Label {
                        text: "Master"
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        opacity: 0.6
                    }
                    QQC2.Slider {
                        id: masterSlider
                        Layout.fillWidth: true
                        from: 0; to: 150
                        property int backendVol: fullRoot.masterVolume
                        onBackendVolChanged: if (!pressed) value = backendVol
                        Component.onCompleted: value = backendVol
                        onMoved: Plasmoid.setMasterVolume(Math.round(value))
                    }
                    PlasmaComponents.Label {
                        text: Math.round(masterSlider.value) + "%"
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        opacity: 0.7
                        Layout.minimumWidth: Kirigami.Units.gridUnit * 2
                    }
                }
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  ADD ROOM OVERLAY
    // ════════════════════════════════════════════════════════════════════
    Rectangle {
        id: addRoomOverlay
        visible: false
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.55)
        z: 100

        MouseArea {
            anchors.fill: parent
            onClicked: addRoomOverlay.visible = false
        }

        Rectangle {
            id: addRoomDialog
            anchors.centerIn: parent
            width: Math.min(parent.width * 0.6, Kirigami.Units.gridUnit * 22)
            height: addRoomCol.implicitHeight + Kirigami.Units.largeSpacing * 2
            radius: Kirigami.Units.largeSpacing
            color: Kirigami.Theme.backgroundColor
            border.width: 1
            border.color: Qt.rgba(Kirigami.Theme.textColor.r,
                                  Kirigami.Theme.textColor.g,
                                  Kirigami.Theme.textColor.b, 0.15)

            MouseArea { anchors.fill: parent }

            ColumnLayout {
                id: addRoomCol
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing * 2

                property color selectedColor: "#4a90d9"

                function createRoomAction() {
                    if (!fullRoot.groupModel) {
                        addRoomOverlay.visible = false
                        return
                    }
                    var name = roomNameField.text.trim()
                    if (name.length === 0) return
                    var colorStr = "" + addRoomCol.selectedColor
                    fullRoot.groupModel.addGroup(name, colorStr)
                    roomNameField.text = ""
                    addRoomCol.selectedColor = "#4a90d9"
                    addRoomOverlay.visible = false
                }

                PlasmaExtras.Heading {
                    level: 3
                    text: "New Room"
                }

                PlasmaComponents.TextField {
                    id: roomNameField
                    Layout.fillWidth: true
                    placeholderText: "Room name (e.g. Gaming, Streaming, Music)"
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.05
                    onAccepted: if (text.trim().length > 0) addRoomCol.createRoomAction()
                    focus: addRoomOverlay.visible
                }

                // Color picker row
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing * 2

                    PlasmaComponents.Label {
                        text: "Color:"
                        opacity: 0.8
                        font.bold: true
                    }

                    Repeater {
                        model: ["#4a90d9", "#50c878", "#e74c3c", "#f39c12", "#9b59b6", "#1abc9c"]
                        delegate: Rectangle {
                            width: 28; height: 28; radius: 14
                            color: modelData
                            border.width: ("" + addRoomCol.selectedColor) === modelData ? 3 : 1
                            border.color: ("" + addRoomCol.selectedColor) === modelData
                                          ? "white"
                                          : Qt.rgba(Kirigami.Theme.textColor.r,
                                                    Kirigami.Theme.textColor.g,
                                                    Kirigami.Theme.textColor.b, 0.2)

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: addRoomCol.selectedColor = modelData
                            }
                        }
                    }
                }

                Item { height: Kirigami.Units.smallSpacing }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    PlasmaComponents.Button {
                        text: "Cancel"
                        onClicked: {
                            addRoomOverlay.visible = false
                            roomNameField.text = ""
                        }
                    }

                    Item { Layout.fillWidth: true }

                    PlasmaComponents.Button {
                        text: "  + Create"
                        icon.name: "list-add"
                        enabled: roomNameField.text.trim().length > 0
                        onClicked: addRoomCol.createRoomAction()

                        background: Rectangle {
                            radius: Kirigami.Units.smallSpacing
                            color: addRoomCol.selectedColor
                            opacity: parent.enabled ? (parent.hovered ? 1.0 : 0.85) : 0.3
                        }
                        contentItem: RowLayout {
                            spacing: Kirigami.Units.smallSpacing
                            Kirigami.Icon {
                                source: "list-add"
                                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                isMask: true
                                Kirigami.Theme.textColor: "white"
                            }
                            PlasmaComponents.Label {
                                text: "Create"
                                font.bold: true
                                color: "white"
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Sync selectedRoomIndex when active room changes externally ────
    Connections {
        target: fullRoot.groupModel
        function onDataChanged(topLeft, bottomRight, roles) {
            var gm = fullRoot.groupModel
            if (!gm) return
            for (var i = 0; i < gm.count; i++) {
                if (gm.isGroupActive(i) && fullRoot.selectedRoomIndex !== i) {
                    fullRoot.selectedRoomIndex = i
                    fullRoot.rebuildConnectionsFromModel()
                    break
                }
            }
        }
    }

    // ── Re-apply routing when new streams appear (debounced from C++) ──
    Connections {
        target: Plasmoid
        function onReapplyRequested() {
            fullRoot.handleNewStreams()
        }
    }

    // ── Keyboard shortcuts (active while popup is open) ───────────
    Shortcut {
        sequence: "Ctrl+Right"
        onActivated: fullRoot.selectNextRoom()
    }
    Shortcut {
        sequence: "Ctrl+Left"
        onActivated: fullRoot.selectPrevRoom()
    }

    // Repaint connections when layout changes
    Timer {
        id: repaintTimer
        interval: 100
        onTriggered: connectionCanvas.requestPaint()
    }

    onWidthChanged: repaintTimer.restart()
    onHeightChanged: repaintTimer.restart()

    // Auto-select the first active room when popup opens
    Component.onCompleted: {
        if (fullRoot.groupModel) {
            for (var i = 0; i < fullRoot.groupModel.count; i++) {
                if (fullRoot.groupModel.isGroupActive(i)) {
                    fullRoot.selectRoom(i)
                    return
                }
            }
        }
    }
}
