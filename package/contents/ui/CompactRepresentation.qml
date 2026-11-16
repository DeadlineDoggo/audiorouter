import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

/**
 * Compact representation: the icon shown in the Plasma panel.
 * Clicking it toggles the full representation popup.  A dot in the colour of
 * the active room shows at a glance which room is on.
 */
MouseArea {
    id: compactRoot

    Layout.minimumWidth: Kirigami.Units.iconSizes.small
    Layout.minimumHeight: Kirigami.Units.iconSizes.small
    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
    Layout.preferredHeight: Kirigami.Units.iconSizes.medium

    hoverEnabled: true
    acceptedButtons: Qt.LeftButton
    property bool wasExpanded: false
    onPressed: wasExpanded = root.expanded
    onClicked: root.expanded = !wasExpanded

    // root.activeRoomRow changes whenever a room is switched on or off
    readonly property string activeColor: {
        var gm = Plasmoid.groupModel
        return gm && root.activeRoomRow >= 0 ? gm.groupColor(root.activeRoomRow) : ""
    }

    Image {
        anchors.fill: parent
        source: "icon.png"
        sourceSize: Qt.size(parent.width, parent.height)
        fillMode: Image.PreserveAspectFit
        smooth: true
    }

    // Active room color indicator dot (top-right corner)
    Rectangle {
        width: Math.round(Math.min(parent.width, parent.height) * 0.38)
        height: width
        radius: width / 2
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 1
        anchors.rightMargin: 1

        visible: compactRoot.activeColor !== ""
        color: compactRoot.activeColor !== "" ? compactRoot.activeColor : "transparent"
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, 0.5)
    }
}
