import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents

/** An application that records (a call, a recorder): its port, its name,
 *  and the source it records right now. */
NodeCard {
    id: card

    required property int recorderIndex
    required property string appName
    required property string iconName
    required property string currentSource
    property Item viewport: null

    nodeKey: "rec:" + recorderIndex
    wireColor: board.nodeColors[nodeKey] || ""

    RowLayout {
        width: parent.width
        spacing: Kirigami.Units.smallSpacing

        ConnectionPort {
            nodeKey: card.nodeKey
            board: card.board
            hostCard: card
            viewport: card.viewport
            wireColor: card.wireColor
            Layout.alignment: Qt.AlignVCenter
        }

        Kirigami.Icon {
            source: card.iconName || "audio-input-microphone"
            fallback: "audio-input-microphone"
            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
            Layout.alignment: Qt.AlignVCenter
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            PlasmaComponents.Label {
                Layout.fillWidth: true
                text: card.appName || "Unknown"
                font.bold: true
                elide: Text.ElideRight
            }
            PlasmaComponents.Label {
                Layout.fillWidth: true
                text: card.currentSource !== "" ? "Hears: " + card.currentSource : "Recording"
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                opacity: 0.65
                elide: Text.ElideRight
            }
        }
    }
}
