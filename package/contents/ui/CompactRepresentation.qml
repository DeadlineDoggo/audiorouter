import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

/**
 * Compact representation: the icon shown in the Plasma panel.
 * Clicking it toggles the full representation popup.
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

    property string activeColor: ""

    function refreshActiveColor() {
        var gm = Plasmoid.groupModel
        if (!gm) { activeColor = ""; return }
        for (var i = 0; i < gm.count; i++) {
            if (gm.isGroupActive(i)) {
                activeColor = gm.data(gm.index(i, 0), Qt.UserRole + 3)  // ColorRole
                return
            }
        }
        activeColor = ""
    }

    Component.onCompleted: refreshActiveColor()

    Connections {
        target: Plasmoid.groupModel
        function onDataChanged(topLeft, bottomRight, roles) { compactRoot.refreshActiveColor() }
        function onModelReset() { compactRoot.refreshActiveColor() }
        function onRowsInserted(parent, first, last) { compactRoot.refreshActiveColor() }
        function onRowsRemoved(parent, first, last) { compactRoot.refreshActiveColor() }
    }

    Image {
        id: icon
        anchors.fill: parent
        source: "icon.png"
        sourceSize: Qt.size(parent.width, parent.height)
        smooth: true
    }

    // Active room color indicator dot (top-right corner)
    Rectangle {
        id: activeRoomDot
        width: Math.round(parent.width * 0.38)
        height: Math.round(parent.height * 0.38)
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
