import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras

/**
 * Overlay for creating a room or editing its name, colour and icon.
 * Emits accepted(); the caller applies it.
 */
Rectangle {
    id: overlay

    /// -1: create a new room; otherwise the row being edited
    property int editingRow: -1
    property color roomColor: "#4a90d9"
    property string roomIcon: "audio-card"
    readonly property string roomName: nameField.text.trim()

    signal accepted()

    readonly property var iconChoices: [
        "audio-card", "audio-headphones", "audio-speakers",
        "audio-volume-high", "multimedia-player", "audio-headset",
        "applications-multimedia", "audio-input-microphone",
        "media-playback-start", "media-record"
    ]

    function openForNew() {
        editingRow = -1
        nameField.text = ""
        roomColor = "#4a90d9"
        roomIcon = "audio-card"
        visible = true
        nameField.forceActiveFocus()
    }

    function openForEdit(row, name, color, icon) {
        editingRow = row
        nameField.text = name
        roomColor = color
        roomIcon = icon
        visible = true
        nameField.forceActiveFocus()
    }

    function accept() {
        if (roomName.length === 0) return
        accepted()
        visible = false
    }

    visible: false
    color: Qt.rgba(0, 0, 0, 0.55)

    // Clicking beside the dialog closes it; the dialog itself swallows clicks
    MouseArea { anchors.fill: parent; onClicked: overlay.visible = false }
    Keys.onEscapePressed: visible = false

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 22)
        height: form.implicitHeight + Kirigami.Units.largeSpacing * 2
        radius: Kirigami.Units.largeSpacing
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                              Kirigami.Theme.textColor.b, 0.15)

        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: form
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing * 2

            PlasmaExtras.Heading {
                level: 3
                text: overlay.editingRow >= 0 ? "Edit Room" : "New Room"
            }

            PlasmaComponents.TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: "Room name (e.g. Gaming, Streaming, Music)"
                onAccepted: overlay.accept()
                Keys.onEscapePressed: overlay.visible = false
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Kirigami.Units.smallSpacing * 2
                rowSpacing: Kirigami.Units.smallSpacing * 2

                PlasmaComponents.Label {
                    text: "Color:"
                    opacity: 0.8
                    font.bold: true
                    Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Repeater {
                        model: ["#4a90d9", "#50c878", "#e74c3c", "#f39c12", "#9b59b6", "#1abc9c"]
                        delegate: Rectangle {
                            required property string modelData
                            readonly property bool current:
                                ("" + overlay.roomColor).toLowerCase() === modelData
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            radius: 12
                            color: modelData
                            border.width: current ? 3 : 1
                            border.color: current ? Kirigami.Theme.textColor
                                                  : Qt.rgba(Kirigami.Theme.textColor.r,
                                                            Kirigami.Theme.textColor.g,
                                                            Kirigami.Theme.textColor.b, 0.2)
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: overlay.roomColor = parent.modelData
                            }
                        }
                    }

                    KQuickControls.ColorButton {
                        color: overlay.roomColor
                        dialogTitle: "Choose Room Color"
                        onAccepted: function(color) { overlay.roomColor = color }
                        Layout.preferredWidth: 24
                        Layout.preferredHeight: 24
                    }

                    Item { Layout.fillWidth: true }
                }

                PlasmaComponents.Label {
                    text: "Icon:"
                    opacity: 0.8
                    font.bold: true
                    Layout.alignment: Qt.AlignTop | Qt.AlignRight
                    topPadding: Kirigami.Units.smallSpacing
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 5
                    columnSpacing: Kirigami.Units.smallSpacing
                    rowSpacing: Kirigami.Units.smallSpacing

                    Repeater {
                        model: overlay.iconChoices
                        delegate: Rectangle {
                            required property string modelData
                            readonly property bool current: overlay.roomIcon === modelData
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 30
                            radius: Kirigami.Units.smallSpacing
                            color: current
                                   ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                             Kirigami.Theme.highlightColor.g,
                                             Kirigami.Theme.highlightColor.b, 0.3)
                                   : (iconMouse.containsMouse
                                      ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                                Kirigami.Theme.highlightColor.g,
                                                Kirigami.Theme.highlightColor.b, 0.12)
                                      : "transparent")
                            border.width: current ? 2 : 0
                            border.color: Kirigami.Theme.highlightColor

                            Kirigami.Icon {
                                anchors.centerIn: parent
                                source: parent.modelData
                                width: Kirigami.Units.iconSizes.small
                                height: Kirigami.Units.iconSizes.small
                            }

                            MouseArea {
                                id: iconMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: overlay.roomIcon = parent.modelData
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
                spacing: Kirigami.Units.smallSpacing

                PlasmaComponents.Button {
                    text: "Cancel"
                    onClicked: overlay.visible = false
                }

                Item { Layout.fillWidth: true }

                PlasmaComponents.Button {
                    id: acceptButton
                    enabled: overlay.roomName.length > 0
                    onClicked: overlay.accept()
                    leftPadding: Kirigami.Units.largeSpacing
                    rightPadding: Kirigami.Units.largeSpacing
                    topPadding: Kirigami.Units.smallSpacing
                    bottomPadding: Kirigami.Units.smallSpacing

                    background: Rectangle {
                        radius: Kirigami.Units.smallSpacing
                        color: overlay.roomColor
                        opacity: acceptButton.enabled ? (acceptButton.hovered ? 1.0 : 0.85) : 0.3
                    }
                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Icon {
                            source: overlay.editingRow >= 0 ? "document-save" : "list-add"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                            isMask: true
                            color: "white"
                        }
                        PlasmaComponents.Label {
                            text: overlay.editingRow >= 0 ? "Save" : "Create"
                            font.bold: true
                            color: "white"
                        }
                    }
                }
            }
        }
    }
}
