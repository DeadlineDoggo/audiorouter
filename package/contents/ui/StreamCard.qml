import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.plasmoid

/** A playing application stream: name, what it plays, volume, and its port. */
NodeCard {
    id: card

    required property int inputIndex
    required property string appName
    required property string mediaName
    required property string iconName
    required property int volumePercent
    property Item viewport: null

    nodeKey: "input:" + inputIndex
    wireColor: board.nodeColors[nodeKey] || ""

    RowLayout {
        width: parent.width
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            source: card.iconName || "applications-multimedia"
            fallback: "applications-multimedia"
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
                visible: card.mediaName !== "" && card.mediaName !== card.appName
                text: card.mediaName
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                opacity: 0.65
                elide: Text.ElideRight
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                QQC2.Slider {
                    id: volumeSlider
                    Layout.fillWidth: true
                    from: 0; to: 150
                    // Follow the server, except while the user holds the handle
                    property int backendVolume: card.volumePercent
                    onBackendVolumeChanged: if (!pressed) value = backendVolume
                    Component.onCompleted: value = backendVolume
                    onMoved: Plasmoid.setSinkInputVolume(card.inputIndex, Math.round(value))
                }
                PlasmaComponents.Label {
                    text: Math.round(volumeSlider.value) + "%"
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    opacity: 0.7
                    horizontalAlignment: Text.AlignRight
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 2
                }
            }
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
