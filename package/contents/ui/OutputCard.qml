import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.plasmoid

/** An output device: its port, name, mute and volume. */
NodeCard {
    id: card

    required property string sinkName
    required property string sinkDescription
    required property int sinkVolumePercent
    required property bool sinkMuted
    required property bool isDefault
    property Item viewport: null

    nodeKey: "sink:" + sinkName
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
            source: {
                var desc = (card.sinkDescription || "").toLowerCase()
                if (desc.indexOf("headphone") >= 0 || desc.indexOf("headset") >= 0)
                    return "audio-headphones"
                if (desc.indexOf("speaker") >= 0)
                    return "audio-speakers"
                if (desc.indexOf("hdmi") >= 0 || desc.indexOf("displayport") >= 0
                        || desc.indexOf("monitor") >= 0)
                    return "video-display"
                return "audio-card"
            }
            fallback: "audio-card"
            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
            Layout.alignment: Qt.AlignVCenter
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    text: card.sinkDescription || card.sinkName
                    font.bold: true
                    elide: Text.ElideRight
                }
                // Where unwired applications play
                PlasmaComponents.Label {
                    visible: card.isDefault
                    text: "default"
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    opacity: 0.55
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                PlasmaComponents.ToolButton {
                    checkable: true
                    checked: card.sinkMuted
                    icon.name: card.sinkMuted || volumeSlider.value === 0 ? "audio-volume-muted"
                             : volumeSlider.value < 40 ? "audio-volume-low"
                             : volumeSlider.value < 75 ? "audio-volume-medium"
                                                       : "audio-volume-high"
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    onClicked: Plasmoid.toggleSinkMute(card.sinkName)
                    PlasmaComponents.ToolTip { text: card.sinkMuted ? "Unmute" : "Mute" }
                }
                QQC2.Slider {
                    id: volumeSlider
                    Layout.fillWidth: true
                    from: 0; to: 150
                    enabled: !card.sinkMuted
                    opacity: card.sinkMuted ? 0.4 : 1.0
                    property int backendVolume: card.sinkVolumePercent
                    onBackendVolumeChanged: if (!pressed) value = backendVolume
                    Component.onCompleted: value = backendVolume
                    onMoved: Plasmoid.setSinkVolumeByName(card.sinkName, Math.round(value))
                }
                PlasmaComponents.Label {
                    text: card.sinkMuted ? "muted" : Math.round(volumeSlider.value) + "%"
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    opacity: card.sinkMuted ? 0.5 : 0.7
                    horizontalAlignment: Text.AlignRight
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 2.5
                }
            }
        }
    }
}
