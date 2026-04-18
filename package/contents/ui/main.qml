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
}
