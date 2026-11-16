import QtQuick
import org.kde.kirigami as Kirigami

/**
 * Common frame of the cards on the routing board (streams, outputs, ghosts).
 * Tinted in the colour of its wires; tells the board when it moves so the
 * wires follow.
 */
Rectangle {
    id: card

    required property RoutingBoard board
    /// Node key whose wires a right-click removes
    property string nodeKey: ""
    /// Colour of the node's wires, empty when it has none
    property string wireColor: ""
    /// Saved in the room but not present right now
    property bool ghost: false
    default property alias content: inner.data

    implicitHeight: inner.implicitHeight + Kirigami.Units.smallSpacing * 2
    radius: Kirigami.Units.smallSpacing

    color: wireColor !== ""
           ? Qt.rgba(Qt.color(wireColor).r, Qt.color(wireColor).g, Qt.color(wireColor).b,
                     ghost ? 0.07 : 0.15)
           : Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g,
                     Kirigami.Theme.backgroundColor.b, 0.35)
    border.width: 1
    border.color: wireColor !== ""
                  ? Qt.rgba(Qt.color(wireColor).r, Qt.color(wireColor).g, Qt.color(wireColor).b,
                            ghost ? 0.45 : 1)
                  : Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                            Kirigami.Theme.textColor.b, ghost ? 0.2 : 0.08)

    Item {
        id: inner
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        implicitHeight: childrenRect.height
        opacity: card.ghost ? 0.6 : 1
    }

    // Right-click removes every wire of this node
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: card.nodeKey !== ""
        onTapped: card.board.disconnectNode(card.nodeKey)
    }

    // The wires are painted from the ports' positions: repaint when this
    // card moves or changes size.
    onYChanged: board.schedulePaint()
    onXChanged: board.schedulePaint()
    onWidthChanged: board.schedulePaint()
    onHeightChanged: board.schedulePaint()
}
