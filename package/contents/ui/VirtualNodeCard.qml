import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.plasmoid

/**
 * A virtual node: an input port on the left, an output port on the right.
 *
 * A plain node passes what is played into it on to the outputs wired to its
 * OUT side.  A network node sends what is played into it to the other
 * devices on its channel, and plays what they send on its OUT side.
 */
Rectangle {
    id: card

    required property RoutingBoard board
    /// {paName, displayName, networkEnabled, channel, ...}
    required property var node
    property Item viewport: null
    /// The room this node belongs to is switched on
    property bool roomActive: false

    readonly property string paName: node.paName || ""
    readonly property bool isNetwork: node.networkEnabled === true
    readonly property string nodeKeyIn: "vsinkin:" + paName
    readonly property string nodeKeyOut: "vsinkout:" + paName

    // Live state of a network node; refreshed when the engine reports a change
    property var netStatus: ({})
    function refreshStatus() {
        netStatus = isNetwork ? Plasmoid.networkStatus(paName) : ({})
    }
    readonly property var peers: netStatus.peers || []
    Component.onCompleted: refreshStatus()
    onIsNetworkChanged: refreshStatus()
    onRoomActiveChanged: refreshStatus()
    Connections {
        target: Plasmoid
        function onNetworkStatusChanged(name) { if (name === card.paName) card.refreshStatus() }
    }

    signal settingsRequested()

    // Purple for local nodes, teal for network nodes
    readonly property color accent: isNetwork ? "#1abc9c" : "#9b59b6"

    implicitHeight: body.implicitHeight + Kirigami.Units.smallSpacing * 2
    radius: 6
    color: Qt.rgba(accent.r, accent.g, accent.b, 0.13)
    border.width: 1
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.5)

    RowLayout {
        id: body
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing * 2

        // ── IN ──────────────────────────────────────────────────────
        ColumnLayout {
            spacing: 1
            Layout.alignment: Qt.AlignVCenter
            PlasmaComponents.Label {
                text: "IN"
                font.pointSize: Kirigami.Theme.smallFont.pointSize * 0.8
                font.weight: Font.DemiBold
                opacity: 0.55
                Layout.alignment: Qt.AlignHCenter
            }
            ConnectionPort {
                nodeKey: card.nodeKeyIn
                board: card.board
                hostCard: card
                viewport: card.viewport
                wireColor: card.board.nodeColors[card.nodeKeyIn] || ""
                idleColor: card.accent
                Layout.alignment: Qt.AlignHCenter
            }
        }

        // ── Name and state ──────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing / 2

                Kirigami.Icon {
                    visible: card.isNetwork
                    source: "network-wireless"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    text: card.node.displayName || card.paName
                    font.bold: true
                    elide: Text.ElideRight
                }
                PlasmaComponents.ToolButton {
                    icon.name: "configure"
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    onClicked: card.settingsRequested()
                    PlasmaComponents.ToolTip { text: "Name and network settings" }
                }
                PlasmaComponents.ToolButton {
                    icon.name: "edit-delete"
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    onClicked: Plasmoid.removeVirtualSink(card.board.roomIndex, card.paName)
                    PlasmaComponents.ToolTip { text: "Delete this node" }
                }
            }

            PlasmaComponents.Label {
                Layout.fillWidth: true
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                elide: Text.ElideRight
                opacity: 0.75
                color: card.isNetwork && card.netStatus.state === "error"
                       ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                text: {
                    if (!card.isNetwork)
                        return "Passes IN on to OUT"
                    if (!card.roomActive)
                        return "Channel “" + (card.node.channel || "") + "” · room is off"
                    if (card.netStatus.state === "error")
                        return card.netStatus.error || "Network error"
                    if (card.netStatus.state !== "online")
                        return "Connecting…"
                    if (card.peers.length === 0)
                        return "Channel “" + (card.node.channel || "") + "” · no other devices yet"
                    var names = []
                    for (var i = 0; i < card.peers.length; i++) names.push(card.peers[i].name)
                    return "Connected: " + names.join(", ")
                }
            }
        }

        // ── OUT ─────────────────────────────────────────────────────
        ColumnLayout {
            spacing: 1
            Layout.alignment: Qt.AlignVCenter
            PlasmaComponents.Label {
                text: "OUT"
                font.pointSize: Kirigami.Theme.smallFont.pointSize * 0.8
                font.weight: Font.DemiBold
                opacity: 0.55
                Layout.alignment: Qt.AlignHCenter
            }
            ConnectionPort {
                nodeKey: card.nodeKeyOut
                board: card.board
                hostCard: card
                viewport: card.viewport
                wireColor: card.board.nodeColors[card.nodeKeyOut] || ""
                idleColor: card.accent
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }

    // Right-click removes all wires of the node
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: {
            card.board.disconnectNode(card.nodeKeyIn)
            card.board.disconnectNode(card.nodeKeyOut)
        }
    }

    onYChanged: board.schedulePaint()
    onXChanged: board.schedulePaint()
    onWidthChanged: board.schedulePaint()
    onHeightChanged: board.schedulePaint()
}
