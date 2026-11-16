import QtQuick
import org.kde.kirigami as Kirigami

/**
 * The dot on a card that wires start and end on.
 *
 * It registers itself with the routing board under its node key, so the
 * board always knows where every wire end is, and it hands press / move /
 * release to the board, which owns the drag.
 */
Item {
    id: port

    /// Node key, e.g. "input:42", "sink:alsa_output…", "vsinkin:soundroot_virt_…"
    required property string nodeKey
    /// The board this port belongs to
    required property RoutingBoard board
    /// Card the port sits on: dropping a wire anywhere on it connects here
    property Item hostCard: null
    /// Scrolling area of the port's column; wires are clipped to it
    property Item viewport: null
    /// Colour when the port has wires; empty when it has none
    property string wireColor: ""
    property color idleColor: "#888888"

    readonly property bool isDragSource: board.dragging && board.dragFromKey === nodeKey
    readonly property bool isValidTarget: board.dragging && board.validTargets[nodeKey] === true
    /// Releasing the wire now would connect it here
    readonly property bool isDropCandidate: board.dragging && board.dropCandidate === nodeKey

    implicitWidth: 14
    implicitHeight: 14

    Rectangle {
        id: dot
        anchors.centerIn: parent
        width: port.isDropCandidate ? 22
             : port.isValidTarget || portMouse.containsMouse || port.isDragSource ? 18 : 14
        height: width
        radius: width / 2
        color: port.wireColor !== "" ? port.wireColor : port.idleColor
        border.width: 2
        border.color: port.isValidTarget ? Kirigami.Theme.highlightColor : Qt.lighter(color, 1.4)
        // Ports that cannot take the wire being dragged step back
        opacity: port.board.dragging && !port.isValidTarget && !port.isDragSource ? 0.3 : 1

        Behavior on width { NumberAnimation { duration: Kirigami.Units.shortDuration } }
    }

    MouseArea {
        id: portMouse
        anchors.fill: parent
        anchors.margins: -8   // comfortable to hit
        hoverEnabled: true
        cursorShape: Qt.CrossCursor
        preventStealing: true   // the scrolling column must not take the drag
        acceptedButtons: Qt.LeftButton

        onPressed: port.board.beginDrag(port)
        onPositionChanged: function(mouse) {
            if (pressed) port.board.moveDrag(portMouse, mouse.x, mouse.y)
        }
        onReleased: function(mouse) { port.board.releaseDrag(portMouse, mouse.x, mouse.y) }
        onCanceled: port.board.cancelDrag()
    }

    // Keep the board's registry in step with this port's life and key
    property string registeredKey: ""
    function register() {
        if (registeredKey !== "") board.unregisterPort(registeredKey, port)
        registeredKey = nodeKey
        if (registeredKey !== "") board.registerPort(registeredKey, port)
    }
    onNodeKeyChanged: register()
    Component.onCompleted: register()
    Component.onDestruction: if (registeredKey !== "") board.unregisterPort(registeredKey, port)
}
