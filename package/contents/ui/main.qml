import QtQuick
import QtQuick.Layouts
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
    toolTipSubText: "Manage audio routing groups"

    property int currentRoomIndex: -1

    // Handle global shortcuts emitted by the C++ plugin (registered with KGlobalAccel)
    Connections {
        target: Plasmoid
        function onNextRoomActivated() { root.doNextRoom() }
        function onPrevRoomActivated() { root.doPrevRoom() }
    }

    function doNextRoom() {
        var gm = Plasmoid.groupModel
        if (!gm || gm.count === 0) return
        var n = gm.count
        var cur = currentRoomIndex
        if (cur < 0) {
            for (var i = 0; i < n; i++) {
                if (gm.isGroupActive(i)) { cur = i; break }
            }
        }
        var next = cur < 0 ? 0 : (cur + 1) % n
        for (var i = 0; i < n; i++) {
            if (i !== next && gm.isGroupActive(i)) gm.toggleActive(i)
        }
        if (!gm.isGroupActive(next)) gm.toggleActive(next)
        if (Plasmoid.routeModel) Plasmoid.routeModel.groupId = gm.groupId(next)
        currentRoomIndex = next
    }

    function doPrevRoom() {
        var gm = Plasmoid.groupModel
        if (!gm || gm.count === 0) return
        var n = gm.count
        var cur = currentRoomIndex
        if (cur < 0) {
            for (var i = 0; i < n; i++) {
                if (gm.isGroupActive(i)) { cur = i; break }
            }
        }
        var prev = cur < 0 ? n - 1 : (cur - 1 + n) % n
        for (var i = 0; i < n; i++) {
            if (i !== prev && gm.isGroupActive(i)) gm.toggleActive(i)
        }
        if (!gm.isGroupActive(prev)) gm.toggleActive(prev)
        if (Plasmoid.routeModel) Plasmoid.routeModel.groupId = gm.groupId(prev)
        currentRoomIndex = prev
    }
}
