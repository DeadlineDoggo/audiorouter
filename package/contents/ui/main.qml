import QtQuick
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

PlasmoidItem {
    id: root

    Plasmoid.icon: "audio-volume-high"

    preferredRepresentation: compactRepresentation
    compactRepresentation: CompactRepresentation {}
    fullRepresentation: FullRepresentation {}

    switchWidth:  Kirigami.Units.gridUnit * 64
    switchHeight: Kirigami.Units.gridUnit * 32

    toolTipMainText: "SoundRoot"
    toolTipSubText: {
        var gm = Plasmoid.groupModel
        if (gm === undefined)
            return "Plugin not loaded - open for details"
        // Re-evaluated when the active room changes (see CompactRepresentation)
        var row = activeRoomRow
        return row >= 0 && gm ? "Active room: " + gm.groupName(row)
                              : "No room active"
    }

    // Row of the active room, kept current for the tooltip and the panel icon
    property int activeRoomRow: -1
    // Without the compiled plugin `Plasmoid` has no activeRoom() (see the
    // notice in FullRepresentation)
    function refreshActiveRoom() {
        activeRoomRow = Plasmoid.groupModel !== undefined ? Plasmoid.activeRoom() : -1
    }
    Component.onCompleted: refreshActiveRoom()
    Connections {
        target: Plasmoid.groupModel
        function onDataChanged() { root.refreshActiveRoom() }
        function onModelReset() { root.refreshActiveRoom() }
        function onRowsInserted() { root.refreshActiveRoom() }
        function onRowsRemoved() { root.refreshActiveRoom() }
        function onRowsMoved() { root.refreshActiveRoom() }
    }
}
