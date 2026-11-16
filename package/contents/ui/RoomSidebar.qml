import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid

/**
 * The list of rooms: select one to look at its wiring, switch one on to
 * apply it, drag the grip to reorder.
 */
Rectangle {
    id: sidebar

    property var groupModel: null
    /// Row whose wiring the board shows
    property int selectedIndex: -1

    signal roomSelected(int row)
    signal newRoomRequested()
    signal editRoomRequested(int row, string name, string color, string icon)

    color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                   Kirigami.Theme.backgroundColor.g,
                   Kirigami.Theme.backgroundColor.b, 0.6)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        // ── Logo + Title ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            Image {
                source: "icon.png"
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                sourceSize: Qt.size(Kirigami.Units.iconSizes.medium,
                                    Kirigami.Units.iconSizes.medium)
                smooth: true
            }

            PlasmaExtras.Heading {
                level: 3
                text: "SoundRoot"
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        Kirigami.Separator { Layout.fillWidth: true; opacity: 0.3 }

        // ── Room list ───────────────────────────────────────────────
        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            ListView {
                id: roomListView
                model: sidebar.groupModel
                spacing: 2
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // Survives delegate recycling during moveGroup reorders
                property int dragIndex: -1

                delegate: Rectangle {
                    id: roomDelegate

                    required property int index
                    required property string groupName
                    required property string groupColor
                    required property string groupIcon
                    required property bool groupActive
                    required property int linkCount

                    readonly property bool selected: sidebar.selectedIndex === index

                    width: roomListView.width
                    height: roomRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                    radius: Kirigami.Units.smallSpacing
                    color: selected
                           ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                     Kirigami.Theme.highlightColor.g,
                                     Kirigami.Theme.highlightColor.b, 0.18)
                           : (roomMouse.containsMouse
                              ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                        Kirigami.Theme.highlightColor.g,
                                        Kirigami.Theme.highlightColor.b, 0.08)
                              : "transparent")

                    MouseArea {
                        id: roomMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: sidebar.roomSelected(roomDelegate.index)
                    }

                    RowLayout {
                        id: roomRow
                        anchors.fill: parent
                        anchors.margins: Kirigami.Units.smallSpacing
                        spacing: Kirigami.Units.smallSpacing

                        // Drag handle (grip icon)
                        Item {
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.fillHeight: true

                            Kirigami.Icon {
                                source: "handle-sort"
                                width: Kirigami.Units.iconSizes.small
                                height: Kirigami.Units.iconSizes.small
                                anchors.centerIn: parent
                                isMask: true
                                opacity: gripMouse.containsMouse || gripMouse.pressed ? 0.8
                                       : roomMouse.containsMouse || roomDelegate.selected ? 0.4 : 0
                            }

                            MouseArea {
                                id: gripMouse
                                anchors.fill: parent
                                cursorShape: Qt.SizeVerCursor
                                hoverEnabled: true
                                // The list must not take the drag for scrolling
                                preventStealing: true

                                onPressed: roomListView.dragIndex = roomDelegate.index
                                onPositionChanged: function(mouse) {
                                    if (!pressed || roomListView.dragIndex < 0) return
                                    var cp = mapToItem(roomListView.contentItem, mouse.x, mouse.y)
                                    // Clamp so indexAt never returns -1 at list edges
                                    var y = Math.max(0, Math.min(cp.y, roomListView.contentHeight - 1))
                                    var target = roomListView.indexAt(roomListView.width / 2, y)
                                    if (target >= 0 && target !== roomListView.dragIndex) {
                                        sidebar.groupModel.moveGroup(roomListView.dragIndex, target)
                                        roomListView.dragIndex = target
                                    }
                                }
                                onReleased: roomListView.dragIndex = -1
                                onCanceled: roomListView.dragIndex = -1
                            }
                        }

                        // Color bar
                        Rectangle {
                            Layout.preferredWidth: 4
                            Layout.fillHeight: true
                            radius: 2
                            color: roomDelegate.groupColor || "#4a90d9"
                            opacity: roomDelegate.groupActive ? 1.0 : 0.25
                        }

                        Kirigami.Icon {
                            source: roomDelegate.groupIcon || "audio-card"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            opacity: roomDelegate.groupActive ? 1.0 : 0.5
                        }

                        // Name + wire count
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            PlasmaComponents.Label {
                                text: roomDelegate.groupName
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                opacity: roomDelegate.groupActive ? 1.0 : 0.6
                            }
                            PlasmaComponents.Label {
                                text: roomDelegate.linkCount + (roomDelegate.linkCount === 1 ? " wire" : " wires")
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                opacity: 0.5
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        // On / off.  The model decides the position: the
                        // switch asks, and shows what the model then says.
                        PlasmaComponents.Switch {
                            checked: roomDelegate.groupActive
                            onToggled: {
                                if (checked) Plasmoid.activateRoom(roomDelegate.index)
                                else Plasmoid.deactivateRoom(roomDelegate.index)
                                checked = Qt.binding(function() { return roomDelegate.groupActive })
                            }
                            PlasmaComponents.ToolTip {
                                text: roomDelegate.groupActive ? "Switch this room off" : "Apply this room's wiring"
                            }
                        }

                        PlasmaComponents.ToolButton {
                            icon.name: "overflow-menu"
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small
                            // Keeps its place when hidden, so the row does not jump on hover
                            opacity: roomMouse.containsMouse || hovered || roomDelegate.selected
                                     || roomMenu.visible ? 1 : 0
                            onClicked: roomMenu.open()

                            QQC2.Menu {
                                id: roomMenu
                                QQC2.MenuItem {
                                    text: "Edit"
                                    icon.name: "document-edit"
                                    onTriggered: sidebar.editRoomRequested(
                                        roomDelegate.index, roomDelegate.groupName,
                                        roomDelegate.groupColor || "#4a90d9",
                                        roomDelegate.groupIcon || "audio-card")
                                }
                                QQC2.MenuItem {
                                    text: "Remove"
                                    icon.name: "edit-delete"
                                    // Switches the room off first, so nothing it set up is left behind
                                    onTriggered: Plasmoid.removeRoom(roomDelegate.index)
                                }
                            }
                        }
                    }
                }

                PlasmaExtras.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit
                    visible: roomListView.count === 0
                    iconName: "list-add"
                    text: "No rooms yet"
                    explanation: "A room is a saved way of wiring applications to outputs."
                }
            }
        }

        // ── Bottom action bar: New Room + Settings ──────────────────
        Kirigami.Separator { Layout.fillWidth: true; opacity: 0.15 }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                radius: Kirigami.Units.smallSpacing
                color: addMouse.pressed
                       ? Qt.rgba(0.29, 0.56, 0.85, 0.35)
                       : addMouse.containsMouse
                         ? Qt.rgba(0.29, 0.56, 0.85, 0.22)
                         : Qt.rgba(0.29, 0.56, 0.85, 0.12)

                Behavior on color { ColorAnimation { duration: 100 } }

                // Left accent bar
                Rectangle {
                    anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 3 }
                    width: 2
                    radius: 1
                    color: "#4a90d9"
                    opacity: addMouse.containsMouse ? 1.0 : 0.6
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                }

                RowLayout {
                    anchors.centerIn: parent
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: "list-add"
                        isMask: true
                        color: "#4a90d9"
                        Layout.preferredWidth:  Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    PlasmaComponents.Label {
                        text: "New Room"
                        font.bold: true
                        color: "#4a90d9"
                    }
                }

                MouseArea {
                    id: addMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.newRoomRequested()
                }
            }

            PlasmaComponents.ToolButton {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                icon.name: "configure"
                opacity: hovered ? 1.0 : 0.5
                Behavior on opacity { NumberAnimation { duration: 100 } }
                onClicked: Plasmoid.internalAction("configure").trigger()
                PlasmaComponents.ToolTip { text: "Shortcuts and settings" }
            }
        }
    }
}
