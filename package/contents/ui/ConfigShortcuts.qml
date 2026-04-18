import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls
import org.kde.plasma.plasmoid
import org.kde.kcmutils as KCM

KCM.SimpleKCM {
    id: root

    property bool unsavedChanges: false

    function saveConfig() {
        Plasmoid.globalShortcut = activateItem.keySequence
        Plasmoid.nextRoomShortcut = nextItem.keySequence
        Plasmoid.prevRoomShortcut = prevItem.keySequence
        unsavedChanges = false
    }

    Kirigami.FormLayout {
        KQuickControls.KeySequenceItem {
            id: activateItem
            Kirigami.FormData.label: "Activate widget as if clicked:"
            keySequence: Plasmoid.globalShortcut
            onKeySequenceModified: root.unsavedChanges = true
        }

        KQuickControls.KeySequenceItem {
            id: nextItem
            Kirigami.FormData.label: "Next room:"
            keySequence: Plasmoid.nextRoomShortcut
            onKeySequenceModified: root.unsavedChanges = true
        }

        KQuickControls.KeySequenceItem {
            id: prevItem
            Kirigami.FormData.label: "Previous room:"
            keySequence: Plasmoid.prevRoomShortcut
            onKeySequenceModified: root.unsavedChanges = true
        }
    }
}
