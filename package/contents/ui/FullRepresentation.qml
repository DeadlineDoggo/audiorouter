import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid

/**
 * The popup: rooms on the left, the selected room's routing board on the
 * right.
 *
 * All routing state lives in the C++ engine.  This file only keeps what is
 * purely a matter of presentation: which room is being looked at, and which
 * colour each wire is drawn in.
 */
PlasmaExtras.Representation {
    id: fullRoot

    Layout.preferredWidth:  Kirigami.Units.gridUnit * 72
    Layout.preferredHeight: Kirigami.Units.gridUnit * 36
    Layout.minimumWidth:    Kirigami.Units.gridUnit * 52
    Layout.minimumHeight:   Kirigami.Units.gridUnit * 28

    readonly property var groupModel: Plasmoid.groupModel ?? null
    readonly property int masterVolume: Plasmoid.masterVolume ?? 100

    /// Plasma loaded this interface but not the compiled plugin behind it
    /// (it is not installed, or its folder is not on QT_PLUGIN_PATH).  Then
    /// `Plasmoid` is a plain applet without any of SoundRoot's properties and
    /// nothing here can work - say so instead of showing a dead interface.
    readonly property bool backendMissing: Plasmoid.groupModel === undefined

    // ── Which room is shown ─────────────────────────────────────────
    // Tracked by id: rows shift when rooms are reordered or removed.
    property string selectedRoomId: ""
    property int selectedRoomIndex: -1
    property bool selectedRoomActive: false
    property int activeRow: -1

    function selectRoom(row) {
        if (!groupModel || row < 0 || row >= groupModel.count) {
            selectedRoomId = ""
            selectedRoomIndex = -1
        } else {
            if (groupModel.groupId(row) !== selectedRoomId)
                wireColors = ({})          // another room: colours start afresh
            selectedRoomId = groupModel.groupId(row)
            selectedRoomIndex = row
        }
        board.cancelDrag()
        board.addingNode = false
        refreshRoomState()
        refreshVirtualNodes()
        refreshWires()
    }

    /// Find the selected room again after rows were added, moved or removed.
    function syncSelection() {
        if (!groupModel) return
        var row = -1
        for (var i = 0; i < groupModel.count; i++) {
            if (groupModel.groupId(i) === selectedRoomId) { row = i; break }
        }
        if (row >= 0 && selectedRoomId !== "") {
            selectedRoomIndex = row
            refreshRoomState()
        } else {
            // It is gone (or nothing was selected): show the active room,
            // or the first one
            var active = Plasmoid.activeRoom()
            selectRoom(active >= 0 ? active : (groupModel.count > 0 ? 0 : -1))
        }
    }

    function refreshRoomState() {
        activeRow = Plasmoid.activeRoom()
        selectedRoomActive = selectedRoomIndex >= 0 && activeRow === selectedRoomIndex
    }

    // ── What the board shows ────────────────────────────────────────
    property var wires: []
    property var nodeColors: ({})
    property var ghostSources: []
    property var ghostSinks: []
    property var virtualNodes: []

    readonly property var wirePalette: ["#4a90d9", "#50c878", "#e74c3c", "#f39c12", "#9b59b6",
                                    "#1abc9c", "#e67e22", "#2ecc71", "#3498db", "#e91e63"]
    /// target node key → colour.  Kept while a room is shown, so adding or
    /// removing one wire does not recolour all the others.
    property var wireColors: ({})

    function sameData(a, b) { return JSON.stringify(a) === JSON.stringify(b) }

    function refreshWires() {
        var raw = selectedRoomIndex >= 0 ? Plasmoid.roomConnections(selectedRoomIndex) : []

        // One colour per target: all wires into the same output look alike
        var colors = wireColors
        var inUse = {}
        for (var i = 0; i < raw.length; i++) inUse[raw[i].to] = true
        for (var key in colors) {
            if (!inUse[key]) delete colors[key]     // free it for reuse
        }
        function pickColor() {
            var taken = {}
            for (var k in colors) taken[colors[k]] = true
            for (var p = 0; p < wirePalette.length; p++) {
                if (!taken[wirePalette[p]]) return wirePalette[p]
            }
            return wirePalette[Object.keys(colors).length % wirePalette.length]
        }

        var list = []
        var perNode = {}
        for (var j = 0; j < raw.length; j++) {
            var to = raw[j].to
            if (!colors[to]) colors[to] = pickColor()
            list.push({ from: raw[j].from, to: to, color: colors[to] })
            perNode[to] = colors[to]
            if (!perNode[raw[j].from]) perNode[raw[j].from] = colors[to]
        }

        // Assign only what changed: an unchanged list keeps its cards
        if (!sameData(wires, list)) wires = list
        if (!sameData(nodeColors, perNode)) nodeColors = perNode

        var sources = selectedRoomIndex >= 0 ? Plasmoid.offlineSources(selectedRoomIndex) : []
        if (!sameData(ghostSources, sources)) ghostSources = sources
        var sinks = selectedRoomIndex >= 0 ? Plasmoid.offlineSinks(selectedRoomIndex) : []
        if (!sameData(ghostSinks, sinks)) ghostSinks = sinks
    }

    function refreshVirtualNodes() {
        var nodes = selectedRoomIndex >= 0 ? Plasmoid.groupVirtualSinks(selectedRoomIndex) : []
        if (!sameData(virtualNodes, nodes)) virtualNodes = nodes
    }

    // ════════════════════════════════════════════════════════════════════
    //  Layout
    // ════════════════════════════════════════════════════════════════════
    RowLayout {
        anchors.fill: parent
        spacing: 0
        visible: !fullRoot.backendMissing

        RoomSidebar {
            Layout.fillHeight: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 16
            Layout.minimumWidth:   Kirigami.Units.gridUnit * 13
            Layout.maximumWidth:   Kirigami.Units.gridUnit * 18

            groupModel: fullRoot.groupModel
            selectedIndex: fullRoot.selectedRoomIndex
            onRoomSelected: function(row) { fullRoot.selectRoom(row) }
            onNewRoomRequested: roomDialog.openForNew()
            onEditRoomRequested: function(row, name, color, icon) {
                roomDialog.openForEdit(row, name, color, icon)
            }
        }

        // Sidebar / main separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Qt.rgba(Kirigami.Theme.textColor.r,
                           Kirigami.Theme.textColor.g,
                           Kirigami.Theme.textColor.b, 0.1)
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ── Header bar ──────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: headerRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                visible: fullRoot.selectedRoomIndex >= 0
                color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                               Kirigami.Theme.backgroundColor.g,
                               Kirigami.Theme.backgroundColor.b, 0.4)

                RowLayout {
                    id: headerRow
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing

                    PlasmaExtras.Heading {
                        id: roomTitle
                        level: 4
                        // Re-read on any change of the room list (a rename)
                        property int revision: 0
                        text: {
                            revision
                            return fullRoot.groupModel && fullRoot.selectedRoomIndex >= 0
                                   ? fullRoot.groupModel.groupName(fullRoot.selectedRoomIndex) : ""
                        }
                        Connections {
                            target: fullRoot.groupModel
                            function onDataChanged() { roomTitle.revision++ }
                        }
                        elide: Text.ElideRight
                        Layout.maximumWidth: headerRow.width * 0.4
                    }

                    // A room that is not switched on is only a plan
                    PlasmaComponents.Label {
                        Layout.fillWidth: true
                        text: fullRoot.selectedRoomActive
                              ? "active — changes apply immediately"
                              : "not active — wires are saved, nothing is rerouted"
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        opacity: 0.6
                        elide: Text.ElideRight
                    }

                    PlasmaComponents.Button {
                        visible: !fullRoot.selectedRoomActive
                        text: "Switch on"
                        icon.name: "media-playback-start"
                        onClicked: Plasmoid.activateRoom(fullRoot.selectedRoomIndex)
                    }

                    PlasmaComponents.Button {
                        visible: fullRoot.wires.length > 0
                        text: "Clear"
                        icon.name: "edit-clear-all"
                        onClicked: Plasmoid.clearRoom(fullRoot.selectedRoomIndex)
                        PlasmaComponents.ToolTip { text: "Remove every wire of this room" }
                    }

                    PlasmaComponents.Button {
                        text: "Virtual node"
                        icon.name: "list-add"
                        onClicked: board.addingNode = true
                        PlasmaComponents.ToolTip {
                            text: "Add a node that mixes applications, or connects this device to others over the network"
                        }
                    }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                opacity: 0.2
                visible: fullRoot.selectedRoomIndex >= 0
            }

            // ── Sound server gone ───────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: serverLabel.implicitHeight + Kirigami.Units.smallSpacing * 2
                visible: !Plasmoid.serverConnected
                color: Qt.rgba(Kirigami.Theme.negativeTextColor.r, Kirigami.Theme.negativeTextColor.g,
                               Kirigami.Theme.negativeTextColor.b, 0.15)
                PlasmaComponents.Label {
                    id: serverLabel
                    anchors.centerIn: parent
                    text: "The sound server is not reachable — reconnecting…"
                }
            }

            // ── Nothing to show ─────────────────────────────────────
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: fullRoot.selectedRoomIndex < 0

                PlasmaExtras.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    iconName: "audio-volume-high"
                    text: "Create a room to start"
                    explanation: "A room remembers which application plays on which output.\nSwitch between rooms to rewire everything at once."
                    helpfulAction: QQC2.Action {
                        text: "New Room"
                        icon.name: "list-add"
                        onTriggered: roomDialog.openForNew()
                    }
                }
            }

            // ── The board ───────────────────────────────────────────
            RoutingBoard {
                id: board
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: fullRoot.selectedRoomIndex >= 0

                roomIndex: fullRoot.selectedRoomIndex
                roomActive: fullRoot.selectedRoomActive
                wires: fullRoot.wires
                nodeColors: fullRoot.nodeColors
                ghostSources: fullRoot.ghostSources
                ghostSinks: fullRoot.ghostSinks
                virtualNodes: fullRoot.virtualNodes
                sinkModel: Plasmoid.sinkModel ?? null
                sinkInputModel: Plasmoid.sinkInputModel ?? null

                onNodeSettingsRequested: function(node) {
                    nodeDialog.openFor(fullRoot.selectedRoomIndex, node, fullRoot.selectedRoomActive)
                }
            }

            // ── Volume of the default output ────────────────────────
            Kirigami.Separator { Layout.fillWidth: true; opacity: 0.2 }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.smallSpacing
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
                    text: "Default Output"
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    opacity: 0.6
                }
                QQC2.Slider {
                    id: masterSlider
                    Layout.fillWidth: true
                    from: 0; to: 150
                    property int backendVolume: fullRoot.masterVolume
                    onBackendVolumeChanged: if (!pressed) value = backendVolume
                    Component.onCompleted: value = backendVolume
                    onMoved: Plasmoid.setMasterVolume(Math.round(value))
                }
                PlasmaComponents.Label {
                    text: Math.round(masterSlider.value) + "%"
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    opacity: 0.7
                    horizontalAlignment: Text.AlignRight
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 2
                }
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  The plugin is not loaded
    // ════════════════════════════════════════════════════════════════════
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
        visible: fullRoot.backendMissing
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Icon {
            source: "dialog-warning"
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Kirigami.Units.iconSizes.huge
            Layout.preferredHeight: Kirigami.Units.iconSizes.huge
        }
        PlasmaExtras.Heading {
            level: 2
            text: "SoundRoot's plugin is not loaded"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
        PlasmaComponents.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: "This is only the interface. The part that routes audio is a compiled plugin, and Plasma did not find it - so rooms cannot be created or applied."
        }
        PlasmaComponents.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            text: "<b>Installed from the KDE Store or a .plasmoid file?</b> Those contain the interface only. Download the <tt>.tar.gz</tt> from the project's releases, unpack it and run <tt>bash install-local.sh</tt>."
        }
        PlasmaComponents.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            text: "<b>Already ran install-local.sh?</b> Plasma was started before it knew where the plugin is. Log out and back in, or run the restart command the script printed at the end."
        }
        // Selectable, so it can be copied
        QQC2.TextField {
            Layout.fillWidth: true
            readOnly: true
            selectByMouse: true
            horizontalAlignment: Text.AlignHCenter
            text: "https://github.com/DeadlineDoggo/audiorouter#troubleshooting"
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  Dialogs
    // ════════════════════════════════════════════════════════════════════
    RoomDialog {
        id: roomDialog
        anchors.fill: parent
        z: 100
        onAccepted: {
            if (!fullRoot.groupModel) return
            if (editingRow >= 0)
                fullRoot.groupModel.updateGroup(editingRow, roomName, "" + roomColor, roomIcon)
            else
                fullRoot.groupModel.addGroup(roomName, "" + roomColor, roomIcon)
        }
    }

    NodeDialog {
        id: nodeDialog
        anchors.fill: parent
        z: 100
    }

    // ════════════════════════════════════════════════════════════════════
    //  Keeping up with the C++ side
    // ════════════════════════════════════════════════════════════════════
    Connections {
        target: fullRoot.groupModel

        function onDataChanged() {
            // If another room was switched on (here, by shortcut, ...), show it
            var active = Plasmoid.activeRoom()
            var changed = active !== fullRoot.activeRow
            fullRoot.refreshRoomState()
            if (changed && active >= 0 && active !== fullRoot.selectedRoomIndex)
                fullRoot.selectRoom(active)
        }
        function onRowsInserted(parent, first, last) {
            fullRoot.selectRoom(last)      // a new room: show it
        }
        function onRowsRemoved() { fullRoot.syncSelection() }
        function onRowsMoved() { fullRoot.syncSelection() }
        function onModelReset() { fullRoot.syncSelection() }
    }

    Connections {
        target: Plasmoid
        function onConnectionsChanged() { fullRoot.refreshWires() }
        function onVirtualSinksChanged() {
            fullRoot.refreshVirtualNodes()
            fullRoot.refreshWires()
        }
    }

    // Room switching while the popup has focus (the global shortcuts work
    // everywhere and are handled in C++)
    Shortcut {
        sequence: "Ctrl+Right"
        enabled: !fullRoot.backendMissing
        onActivated: Plasmoid.activateNextRoom()
    }
    Shortcut {
        sequence: "Ctrl+Left"
        enabled: !fullRoot.backendMissing
        onActivated: Plasmoid.activatePrevRoom()
    }

    Component.onCompleted: syncSelection()
}
