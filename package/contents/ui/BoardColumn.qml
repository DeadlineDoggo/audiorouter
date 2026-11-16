import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents

/**
 * One column of the routing board: a caption and a list of cards that
 * scrolls on its own when it does not fit.
 */
ColumnLayout {
    id: column

    required property RoutingBoard board
    property string title: ""
    /// Items shown next to the caption (e.g. an add button)
    property alias headerActions: headerActions.data
    /// The cards
    default property alias cards: cardColumn.data
    /// The scrolling area, for the cards' ports to clip their wires to
    readonly property Item viewport: flick
    readonly property real cardWidth: cardColumn.width

    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: Kirigami.Units.gridUnit * 1.8
        Layout.leftMargin: Kirigami.Units.smallSpacing
        Layout.rightMargin: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        PlasmaComponents.Label {
            Layout.fillWidth: true
            text: column.title
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            font.weight: Font.DemiBold
            font.letterSpacing: 1.5
            opacity: 0.5
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
        RowLayout {
            id: headerActions
            spacing: 0
        }
    }

    Kirigami.Separator { Layout.fillWidth: true; opacity: 0.15 }

    Flickable {
        id: flick
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: width
        contentHeight: cardColumn.implicitHeight + Kirigami.Units.smallSpacing * 2
        boundsBehavior: Flickable.StopAtBounds
        // Only scrolls when there is something to scroll to
        interactive: contentHeight > height

        // The wires end on cards in here: repaint as they scroll
        onContentYChanged: column.board.schedulePaint()
        onHeightChanged: column.board.schedulePaint()

        QQC2.ScrollBar.vertical: QQC2.ScrollBar {
            id: scrollBar
            policy: flick.interactive ? QQC2.ScrollBar.AlwaysOn : QQC2.ScrollBar.AlwaysOff
        }

        Column {
            id: cardColumn
            x: Kirigami.Units.smallSpacing
            y: Kirigami.Units.smallSpacing
            // Leave room for the scroll bar so it never covers a port
            width: flick.width - Kirigami.Units.smallSpacing * 2
                   - (flick.interactive ? scrollBar.width : 0)
            spacing: Kirigami.Units.smallSpacing
        }
    }
}
