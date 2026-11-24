import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents

/** A microphone: its name and its port.  It can be wired into a virtual
 *  node or straight into an application that records. */
NodeCard {
    id: card

    required property string sourceName
    required property string sourceDescription
    required property bool isDefault
    property Item viewport: null

    nodeKey: "mic:" + sourceName
    wireColor: board.nodeColors[nodeKey] || ""

    RowLayout {
        width: parent.width
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            source: "audio-input-microphone"
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            Layout.alignment: Qt.AlignVCenter
        }

        PlasmaComponents.Label {
            Layout.fillWidth: true
            text: card.sourceDescription || card.sourceName
            font.bold: true
            elide: Text.ElideRight
        }
        // What applications record when nothing else is wired
        PlasmaComponents.Label {
            visible: card.isDefault
            text: "default"
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            opacity: 0.55
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
