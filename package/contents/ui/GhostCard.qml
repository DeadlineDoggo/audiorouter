import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents

/**
 * A node the room has wires for but that is not there right now: an
 * application that is not playing, or a device that is unplugged.  Showing
 * it keeps the saved wiring visible - and removable.
 */
NodeCard {
    id: card

    property string title: ""
    property string subtitle: ""
    property string iconSource: "audio-card"
    /// true: port on the left (an output); false: port on the right (a source)
    property bool portOnLeft: false
    property Item viewport: null

    ghost: true
    wireColor: board.nodeColors[nodeKey] || ""

    RowLayout {
        width: parent.width
        spacing: Kirigami.Units.smallSpacing
        layoutDirection: card.portOnLeft ? Qt.RightToLeft : Qt.LeftToRight

        Kirigami.Icon {
            source: card.iconSource
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            Layout.alignment: Qt.AlignVCenter
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            PlasmaComponents.Label {
                Layout.fillWidth: true
                text: card.title
                font.bold: true
                elide: Text.ElideRight
            }
            PlasmaComponents.Label {
                Layout.fillWidth: true
                text: card.subtitle
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                font.italic: true
                opacity: 0.75
                elide: Text.ElideRight
            }
        }

        PlasmaComponents.ToolButton {
            icon.name: "edit-delete-remove"
            icon.width: Kirigami.Units.iconSizes.small
            icon.height: Kirigami.Units.iconSizes.small
            Layout.alignment: Qt.AlignVCenter
            onClicked: card.board.disconnectNode(card.nodeKey)
            PlasmaComponents.ToolTip { text: "Forget these wires" }
        }

        ConnectionPort {
            nodeKey: card.nodeKey
            board: card.board
            hostCard: card
            viewport: card.viewport
            wireColor: card.wireColor
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
