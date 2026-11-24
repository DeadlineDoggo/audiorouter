import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid

/**
 * The routing routingBoard of one room: applications and microphones on the
 * left, output devices and recording applications on the right, virtual nodes
 * in between, and the wires that connect them.
 *
 * The routingBoard is a view.  Which wires exist is decided by the routing engine
 * (C++); the routingBoard draws what it is given and turns drags and clicks into
 * connect / disconnect requests.
 *
 * Wires are painted between "ports" - the dots on the cards.  Every port
 * registers here under its node key, so a wire end is always exactly where
 * its dot is, whatever the layout does.
 */
Item {
    id: routingBoard

    // ── What to show (set by FullRepresentation) ─────────────────────
    property int roomIndex: -1
    property bool roomActive: false
    /// [{from, to, color}]; from is the source side
    property var wires: []
    /// node key → colour of its wires
    property var nodeColors: ({})
    /// [{key, appName, mediaName}] saved sources that are not playing
    property var ghostSources: []
    /// [{name, description}] wired devices that are not present
    property var ghostSinks: []
    /// [{key, description}] wired microphones that are not present
    property var ghostMics: []
    /// [{key, appName, mediaName}] wired recording applications that are not recording
    property var ghostRecorders: []
    /// [{paName, displayName, networkEnabled, ...}]
    property var virtualNodes: []
    property var sinkModel: null
    property var sinkInputModel: null
    property var sourceModel: null
    property var sourceOutputModel: null
    /// The "new virtual node" name field is open
    property bool addingNode: false

    signal nodeSettingsRequested(var node)

    function disconnectNode(key) {
        Plasmoid.disconnectNode(roomIndex, key)
    }

    // ════════════════════════════════════════════════════════════════════
    //  Ports
    // ════════════════════════════════════════════════════════════════════
    // key → ConnectionPort.  A plain JS object: changes are followed by an
    // explicit repaint rather than by bindings.
    property var ports: ({})

    function registerPort(key, item) {
        ports[key] = item
        schedulePaint()
    }

    function unregisterPort(key, item) {
        if (ports[key] === item) delete ports[key]
        // The card a wire was being dragged from went away (stream ended)
        if (dragging && dragFromKey === key) cancelDrag()
        schedulePaint()
    }

    /// Centre of a port in routingBoard coordinates.  A port scrolled out of its
    /// column is clamped to the column's edge and flagged, so its wire still
    /// leads towards it instead of ending in mid-air.
    function portPoint(key) {
        var item = ports[key]
        if (!item || !item.visible) return null
        var p = item.mapToItem(routingBoard, item.width / 2, item.height / 2)
        var clipped = false
        if (item.viewport) {
            var top = item.viewport.mapToItem(routingBoard, 0, 0).y
            var bottom = top + item.viewport.height
            if (p.y < top) { p.y = top; clipped = true }
            else if (p.y > bottom) { p.y = bottom; clipped = true }
        }
        return { x: p.x, y: p.y, clipped: clipped }
    }

    /// Is routingBoard point (x, y) inside the card of this port (and visible)?
    function onCardOf(item, x, y) {
        var card = item.hostCard
        if (!card || !item.visible) return false
        if (item.viewport) {
            var top = item.viewport.mapToItem(routingBoard, 0, 0).y
            if (y < top || y > top + item.viewport.height) return false
        }
        var q = card.mapFromItem(routingBoard, x, y)
        return q.x >= 0 && q.x <= card.width && q.y >= 0 && q.y <= card.height
    }

    function overAnyCard(x, y) {
        for (var k in ports) {
            if (onCardOf(ports[k], x, y)) return true
        }
        return false
    }

    // ════════════════════════════════════════════════════════════════════
    //  Painting
    // ════════════════════════════════════════════════════════════════════
    property bool paintQueued: false

    /// Repaint the wires once the current burst of changes is over.
    function schedulePaint() {
        if (paintQueued) return
        paintQueued = true
        Qt.callLater(function() {
            routingBoard.paintQueued = false
            wireCanvas.requestPaint()
            // Once more after layouts have settled: a card that is still
            // being laid out reports its old position in the pass above.
            settleTimer.restart()
        })
    }

    Timer {
        id: settleTimer
        interval: 60
        onTriggered: wireCanvas.requestPaint()
    }

    onWiresChanged: { hoveredWire = -1; schedulePaint() }
    onWidthChanged: schedulePaint()
    onHeightChanged: schedulePaint()
    onVisibleChanged: schedulePaint()

    /// Control points of the curve between two wire ends.  It leaves the
    /// source to the right and enters the target from the left.
    function curve(a, b) {
        var d = Math.max(Kirigami.Units.gridUnit * 2, Math.abs(b.x - a.x) * 0.45)
        return { c1x: a.x + d, c1y: a.y, c2x: b.x - d, c2y: b.y }
    }

    // ════════════════════════════════════════════════════════════════════
    //  Dragging a new wire
    // ════════════════════════════════════════════════════════════════════
    property bool dragging: false
    /// After a plain click on a port the wire follows the pointer until the
    /// next click ("click, then click"), for those who prefer it to dragging.
    property bool dragSticky: false
    property string dragFromKey: ""
    property point dragStart: Qt.point(0, 0)
    property point dragEnd: Qt.point(0, 0)
    /// Keys of the ports the wire being dragged may end on
    property var validTargets: ({})
    /// The port a release would connect to right now
    property string dropCandidate: ""

    function beginDrag(port) {
        if (roomIndex < 0) return
        var valid = {}
        for (var k in ports) {
            if (k !== port.nodeKey && Plasmoid.canConnect(roomIndex, port.nodeKey, k))
                valid[k] = true
        }
        var p = port.mapToItem(routingBoard, port.width / 2, port.height / 2)
        validTargets = valid
        dragFromKey = port.nodeKey
        dragStart = Qt.point(p.x, p.y)
        dragEnd = dragStart
        dropCandidate = ""
        dragSticky = false
        hoveredWire = -1
        dragging = true
        schedulePaint()
    }

    function moveDrag(item, x, y) {
        if (!dragging) return
        var p = item.mapToItem(routingBoard, x, y)
        dragEnd = Qt.point(p.x, p.y)
        dropCandidate = targetAt(p.x, p.y)
        schedulePaint()
    }

    function releaseDrag(item, x, y) {
        if (!dragging) return
        var p = item.mapToItem(routingBoard, x, y)
        var target = targetAt(p.x, p.y)
        if (target !== "") {
            Plasmoid.connectNodes(roomIndex, dragFromKey, target)
            cancelDrag()
        } else if (Math.abs(p.x - dragStart.x) + Math.abs(p.y - dragStart.y) < 8) {
            dragSticky = true      // a click, not a drag: keep the wire on the pointer
        } else {
            cancelDrag()           // dropped on nothing
        }
    }

    function cancelDrag() {
        dragging = false
        dragSticky = false
        dragFromKey = ""
        dropCandidate = ""
        validTargets = ({})
        schedulePaint()
    }

    /// Key of the valid port a wire dropped at (x, y) connects to: the
    /// nearest one whose card is under the pointer, or that is very close.
    function targetAt(x, y) {
        var best = ""
        var bestDistance = Infinity
        for (var k in validTargets) {
            var item = ports[k]
            if (!item || !item.visible) continue
            var c = item.mapToItem(routingBoard, item.width / 2, item.height / 2)
            var distance = Math.sqrt((c.x - x) * (c.x - x) + (c.y - y) * (c.y - y))
            if ((onCardOf(item, x, y) || distance < Kirigami.Units.gridUnit * 1.2)
                    && distance < bestDistance) {
                best = k
                bestDistance = distance
            }
        }
        return best
    }

    // ════════════════════════════════════════════════════════════════════
    //  Hovering and removing wires
    // ════════════════════════════════════════════════════════════════════
    property int hoveredWire: -1

    /// Index of the wire under (x, y), or -1.  Only in the open space
    /// between cards, so a click on a card is never taken for a wire.
    function wireAt(x, y) {
        if (overAnyCard(x, y)) return -1
        var reach = 7
        for (var i = 0; i < wires.length; i++) {
            var a = portPoint(wires[i].from)
            var b = portPoint(wires[i].to)
            if (!a || !b) continue
            var cv = curve(a, b)
            for (var t = 0; t <= 1.0; t += 0.02) {
                var u = 1 - t
                var bx = u*u*u*a.x + 3*u*u*t*cv.c1x + 3*u*t*t*cv.c2x + t*t*t*b.x
                var by = u*u*u*a.y + 3*u*u*t*cv.c1y + 3*u*t*t*cv.c2y + t*t*t*b.y
                if ((x - bx) * (x - bx) + (y - by) * (y - by) < reach * reach)
                    return i
            }
        }
        return -1
    }

    function setHoveredWire(index) {
        if (index === hoveredWire) return
        hoveredWire = index
        schedulePaint()
    }

    // Handlers, not a MouseArea: they see the pointer without taking it away
    // from the sliders, buttons and ports underneath.
    HoverHandler {
        id: hover
        cursorShape: routingBoard.hoveredWire >= 0 ? Qt.PointingHandCursor : undefined
        onPointChanged: {
            if (routingBoard.dragging) return
            routingBoard.setHoveredWire(routingBoard.wireAt(point.position.x, point.position.y))
        }
        onHoveredChanged: if (!hovered) routingBoard.setHoveredWire(-1)
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: function(eventPoint) {
            if (routingBoard.dragging) return
            var i = routingBoard.wireAt(eventPoint.position.x, eventPoint.position.y)
            if (i < 0) return
            var wire = routingBoard.wires[i]
            routingBoard.setHoveredWire(-1)
            Plasmoid.disconnectNodes(routingBoard.roomIndex, wire.from, wire.to)
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  Columns
    // ════════════════════════════════════════════════════════════════════

    /// Caption of the second group of cards in a column
    component SectionLabel: PlasmaComponents.Label {
        topPadding: Kirigami.Units.largeSpacing
        horizontalAlignment: Text.AlignHCenter
        font.pointSize: Kirigami.Theme.smallFont.pointSize
        font.weight: Font.DemiBold
        font.letterSpacing: 1.5
        opacity: 0.5
    }
    readonly property bool virtualVisible: virtualNodes.length > 0 || addingNode
    readonly property real gu: Kirigami.Units.gridUnit
    readonly property real virtualWidth: virtualVisible
        ? Math.max(gu * 11, Math.min(gu * 16, width * 0.26)) : 0
    // The two outer columns share what the middle leaves, within limits;
    // the gaps (where the wires run) take the rest.
    readonly property real sideWidth: Math.max(gu * 9, Math.min(gu * 24,
        (width - virtualWidth - (virtualVisible ? 2 : 1) * gu * 5) / 2))

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── Applications ────────────────────────────────────────────
        BoardColumn {
            id: appsColumn
            board: routingBoard
            title: "APPLICATIONS"
            Layout.fillHeight: true
            Layout.preferredWidth: routingBoard.sideWidth
            Layout.maximumWidth: routingBoard.sideWidth

            Repeater {
                model: routingBoard.sinkInputModel
                delegate: StreamCard {
                    board: routingBoard
                    viewport: appsColumn.viewport
                    width: appsColumn.cardWidth
                }
            }

            Repeater {
                model: routingBoard.ghostSources
                delegate: GhostCard {
                    required property var modelData
                    board: routingBoard
                    viewport: appsColumn.viewport
                    width: appsColumn.cardWidth
                    nodeKey: modelData.key
                    title: modelData.appName
                    subtitle: modelData.mediaName ? "Not playing · " + modelData.mediaName
                                                  : "Not playing"
                    iconSource: "media-playback-stopped"
                    portOnLeft: false
                }
            }

            PlasmaExtras.PlaceholderMessage {
                width: appsColumn.cardWidth
                visible: routingBoard.sinkInputModel && routingBoard.sinkInputModel.count === 0
                         && routingBoard.ghostSources.length === 0
                iconName: "applications-multimedia"
                text: "No audio streams"
                explanation: "Play something and it shows up here."
            }

            // ── Microphones ─────────────────────────────────────────
            SectionLabel {
                width: appsColumn.cardWidth
                visible: (routingBoard.sourceModel && routingBoard.sourceModel.count > 0)
                         || routingBoard.ghostMics.length > 0
                text: "MICROPHONES"
            }

            Repeater {
                model: routingBoard.sourceModel
                delegate: MicCard {
                    board: routingBoard
                    viewport: appsColumn.viewport
                    width: appsColumn.cardWidth
                }
            }

            Repeater {
                model: routingBoard.ghostMics
                delegate: GhostCard {
                    required property var modelData
                    board: routingBoard
                    viewport: appsColumn.viewport
                    width: appsColumn.cardWidth
                    nodeKey: modelData.key
                    title: modelData.description
                    subtitle: "Not connected"
                    iconSource: "audio-input-microphone"
                    portOnLeft: false
                }
            }
        }

        // ── Gap: wires only ─────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: routingBoard.gu * 2.5

            PlasmaComponents.Label {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.smallSpacing * 2
                visible: routingBoard.wires.length === 0 && !routingBoard.dragging && !routingBoard.virtualVisible
                text: "Drag from a dot\nto another to connect"
                opacity: 0.35
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }

        // ── Virtual nodes ───────────────────────────────────────────
        BoardColumn {
            id: virtualColumn
            board: routingBoard
            title: "VIRTUAL NODES"
            visible: routingBoard.virtualVisible
            Layout.fillHeight: true
            Layout.preferredWidth: routingBoard.virtualWidth
            Layout.maximumWidth: routingBoard.virtualWidth

            // Name field for a new node
            RowLayout {
                width: virtualColumn.cardWidth
                visible: routingBoard.addingNode
                spacing: Kirigami.Units.smallSpacing
                onVisibleChanged: if (visible) newNodeName.forceActiveFocus()

                function commit() {
                    var name = newNodeName.text.trim()
                    if (name !== "")
                        Plasmoid.addVirtualSink(routingBoard.roomIndex, name)
                    newNodeName.text = ""
                    routingBoard.addingNode = false
                }

                QQC2.TextField {
                    id: newNodeName
                    Layout.fillWidth: true
                    placeholderText: "Name of the new node…"
                    onAccepted: parent.commit()
                    Keys.onEscapePressed: { text = ""; routingBoard.addingNode = false }
                }
                PlasmaComponents.ToolButton {
                    icon.name: "dialog-ok"
                    enabled: newNodeName.text.trim() !== ""
                    onClicked: parent.commit()
                    PlasmaComponents.ToolTip { text: "Add" }
                }
                PlasmaComponents.ToolButton {
                    icon.name: "dialog-cancel"
                    onClicked: { newNodeName.text = ""; routingBoard.addingNode = false }
                    PlasmaComponents.ToolTip { text: "Cancel" }
                }
            }

            Repeater {
                model: routingBoard.virtualNodes
                delegate: VirtualNodeCard {
                    required property var modelData
                    board: routingBoard
                    node: modelData
                    viewport: virtualColumn.viewport
                    roomActive: routingBoard.roomActive
                    width: virtualColumn.cardWidth
                    onSettingsRequested: routingBoard.nodeSettingsRequested(modelData)
                }
            }
        }

        // ── Gap between virtual nodes and outputs ───────────────────
        Item {
            visible: routingBoard.virtualVisible
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: routingBoard.gu * 2.5
        }

        // ── Output devices ──────────────────────────────────────────
        BoardColumn {
            id: outputsColumn
            board: routingBoard
            title: "OUTPUT DEVICES"
            Layout.fillHeight: true
            Layout.preferredWidth: routingBoard.sideWidth
            Layout.maximumWidth: routingBoard.sideWidth

            Repeater {
                model: routingBoard.sinkModel
                delegate: OutputCard {
                    board: routingBoard
                    viewport: outputsColumn.viewport
                    width: outputsColumn.cardWidth
                }
            }

            Repeater {
                model: routingBoard.ghostSinks
                delegate: GhostCard {
                    required property var modelData
                    board: routingBoard
                    viewport: outputsColumn.viewport
                    width: outputsColumn.cardWidth
                    nodeKey: "sink:" + modelData.name
                    title: modelData.description
                    subtitle: "Not connected"
                    iconSource: "network-disconnect"
                    portOnLeft: true
                }
            }

            PlasmaExtras.PlaceholderMessage {
                width: outputsColumn.cardWidth
                visible: routingBoard.sinkModel && routingBoard.sinkModel.count === 0
                         && routingBoard.ghostSinks.length === 0
                iconName: "audio-speakers"
                text: "No output devices"
                explanation: Plasmoid.serverConnected ? "The sound server reports none."
                                                      : "Waiting for the sound server…"
            }

            // ── Applications that record ────────────────────────────
            SectionLabel {
                width: outputsColumn.cardWidth
                text: "RECORDING"
            }

            Repeater {
                model: routingBoard.sourceOutputModel
                delegate: RecorderCard {
                    board: routingBoard
                    viewport: outputsColumn.viewport
                    width: outputsColumn.cardWidth
                }
            }

            Repeater {
                model: routingBoard.ghostRecorders
                delegate: GhostCard {
                    required property var modelData
                    board: routingBoard
                    viewport: outputsColumn.viewport
                    width: outputsColumn.cardWidth
                    nodeKey: modelData.key
                    title: modelData.appName
                    subtitle: "Not recording"
                    iconSource: "media-record"
                    portOnLeft: true
                }
            }

            PlasmaComponents.Label {
                width: outputsColumn.cardWidth
                visible: routingBoard.sourceOutputModel && routingBoard.sourceOutputModel.count === 0
                         && routingBoard.ghostRecorders.length === 0
                text: "Apps that use a microphone (a call, a recorder) show up here while they record."
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                opacity: 0.5
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  Wires
    // ════════════════════════════════════════════════════════════════════
    Canvas {
        id: wireCanvas
        anchors.fill: parent
        z: 5   // above the cards; a Canvas takes no input

        readonly property string dragColor: "" + Kirigami.Theme.textColor

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.lineCap = "round"

            for (var i = 0; i < routingBoard.wires.length; i++) {
                var wire = routingBoard.wires[i]
                var a = routingBoard.portPoint(wire.from)
                var b = routingBoard.portPoint(wire.to)
                if (!a || !b) continue
                if (a.clipped && b.clipped) continue   // both ends scrolled away

                var hovered = (i === routingBoard.hoveredWire)
                var faded = a.clipped || b.clipped
                var cv = routingBoard.curve(a, b)

                ctx.strokeStyle = hovered ? "#ff4444" : wire.color
                ctx.lineWidth = hovered ? 4 : 2.5
                ctx.globalAlpha = hovered ? 0.95 : (faded ? 0.3 : 0.75)
                ctx.beginPath()
                ctx.moveTo(a.x, a.y)
                ctx.bezierCurveTo(cv.c1x, cv.c1y, cv.c2x, cv.c2y, b.x, b.y)
                ctx.stroke()

                // End caps on the ports (not where the wire leaves the view)
                ctx.fillStyle = hovered ? "#ff4444" : wire.color
                ctx.globalAlpha = hovered ? 1.0 : 0.9
                var ends = [a, b]
                for (var e = 0; e < 2; e++) {
                    if (ends[e].clipped) continue
                    ctx.beginPath()
                    ctx.arc(ends[e].x, ends[e].y, hovered ? 5 : 4, 0, 2 * Math.PI)
                    ctx.fill()
                }
            }

            // The wire being dragged
            if (routingBoard.dragging) {
                var from = routingBoard.portPoint(routingBoard.dragFromKey) || routingBoard.dragStart
                var to = routingBoard.dragEnd
                var snap = routingBoard.dropCandidate !== "" ? routingBoard.portPoint(routingBoard.dropCandidate) : null
                if (snap) to = snap
                // Always bow from left to right, whichever end was grabbed
                var left = from.x <= to.x ? from : to
                var right = from.x <= to.x ? to : from
                var dc = routingBoard.curve(left, right)
                ctx.strokeStyle = snap ? "" + Kirigami.Theme.highlightColor : dragColor
                ctx.globalAlpha = snap ? 0.9 : 0.6
                ctx.lineWidth = 2
                ctx.setLineDash(snap ? [] : [6, 4])
                ctx.beginPath()
                ctx.moveTo(left.x, left.y)
                ctx.bezierCurveTo(dc.c1x, dc.c1y, dc.c2x, dc.c2y, right.x, right.y)
                ctx.stroke()
                ctx.setLineDash([])
            }
            ctx.globalAlpha = 1.0
        }
    }

    // ── Click-click mode: the wire follows the pointer until a click ──
    MouseArea {
        anchors.fill: parent
        z: 10
        visible: routingBoard.dragging && routingBoard.dragSticky
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.CrossCursor

        onPositionChanged: function(mouse) { routingBoard.moveDrag(this, mouse.x, mouse.y) }
        onClicked: function(mouse) {
            if (mouse.button === Qt.LeftButton) {
                var target = routingBoard.targetAt(mouse.x, mouse.y)
                if (target !== "")
                    Plasmoid.connectNodes(routingBoard.roomIndex, routingBoard.dragFromKey, target)
            }
            routingBoard.cancelDrag()   // right-click or a click on nothing cancels
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: routingBoard.dragging
        onActivated: routingBoard.cancelDrag()
    }
}
