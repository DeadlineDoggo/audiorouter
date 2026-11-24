import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid

/**
 * Overlay with a virtual node's settings: its name and whether it is shared
 * over the local network (channel, passphrase, optional peer addresses).
 */
Rectangle {
    id: overlay

    property int roomIndex: -1
    property string paName: ""
    property bool roomActive: false

    // Live state of the node while its room is on
    property var netStatus: ({})
    function refreshStatus() {
        netStatus = visible && paName !== "" ? Plasmoid.networkStatus(paName) : ({})
    }
    Connections {
        target: Plasmoid
        function onNetworkStatusChanged(name) { if (name === overlay.paName) overlay.refreshStatus() }
    }

    readonly property bool networkOn: networkSwitch.checked
    readonly property bool passphraseOk: passField.text.trim().length >= 8
    readonly property bool valid: nameField.text.trim() !== ""
        && (!networkOn || (channelField.text.trim() !== "" && passphraseOk))

    function openFor(row, node, active) {
        roomIndex = row
        roomActive = active
        paName = node.paName
        nameField.text = node.displayName || ""
        networkSwitch.checked = node.networkEnabled === true
        channelField.text = node.channel || ""
        passField.text = node.passphrase || ""
        peersField.text = (node.staticPeers || []).join(", ")
        showPass.checked = false
        visible = true
        refreshStatus()
        nameField.forceActiveFocus()
    }

    function accept() {
        if (!valid) return
        var peers = peersField.text.split(/[\s,;]+/).filter(function(p) { return p !== "" })
        Plasmoid.configureVirtualSink(roomIndex, paName, {
            displayName: nameField.text.trim(),
            networkEnabled: networkSwitch.checked,
            channel: channelField.text.trim(),
            passphrase: passField.text.trim(),
            staticPeers: peers
        })
        visible = false
    }

    visible: false
    color: Qt.rgba(0, 0, 0, 0.55)

    MouseArea { anchors.fill: parent; onClicked: overlay.visible = false }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 30)
        height: Math.min(parent.height - Kirigami.Units.gridUnit, form.implicitHeight + Kirigami.Units.largeSpacing * 2)
        radius: Kirigami.Units.largeSpacing
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                              Kirigami.Theme.textColor.b, 0.15)
        clip: true

        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: form
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing * 2

            PlasmaExtras.Heading {
                level: 3
                text: "Virtual Node"
            }

            PlasmaComponents.TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: "Name"
                onAccepted: overlay.accept()
                Keys.onEscapePressed: overlay.visible = false
            }

            Kirigami.Separator { Layout.fillWidth: true; opacity: 0.3 }

            // ── Network ─────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Icon {
                    source: "network-wireless"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                }
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    text: "Share over the local network"
                    font.bold: true
                }
                PlasmaComponents.Switch { id: networkSwitch }
            }

            PlasmaComponents.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                opacity: 0.75
                text: overlay.networkOn
                      ? "Every node with the same channel name and passphrase is connected, on other devices and on this computer: what is played into IN here comes out of OUT there, and the other way round. The audio is encrypted; anyone who has the passphrase can listen and play."
                      : "A local node passes whatever is played into IN on to the outputs wired to OUT. To pair two nodes, turn this on in both and give them the same channel name and passphrase; on one computer that works without a network."
            }

            GridLayout {
                Layout.fillWidth: true
                visible: overlay.networkOn
                columns: 2
                columnSpacing: Kirigami.Units.smallSpacing * 2
                rowSpacing: Kirigami.Units.smallSpacing

                PlasmaComponents.Label {
                    text: "Channel:"
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                }
                PlasmaComponents.TextField {
                    id: channelField
                    Layout.fillWidth: true
                    placeholderText: "The same name on every device (e.g. Living room)"
                    onAccepted: overlay.accept()
                }

                PlasmaComponents.Label {
                    text: "Passphrase:"
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing / 2
                    PlasmaComponents.TextField {
                        id: passField
                        Layout.fillWidth: true
                        echoMode: showPass.checked ? TextInput.Normal : TextInput.Password
                        placeholderText: "At least 8 characters"
                        inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                        onAccepted: overlay.accept()
                    }
                    PlasmaComponents.ToolButton {
                        id: showPass
                        checkable: true
                        icon.name: checked ? "password-show-off" : "password-show-on"
                        PlasmaComponents.ToolTip { text: showPass.checked ? "Hide" : "Show" }
                    }
                    PlasmaComponents.ToolButton {
                        icon.name: "edit-copy"
                        enabled: passField.text !== ""
                        onClicked: {
                            // A password field refuses to copy: reveal it first
                            showPass.checked = true
                            passField.selectAll()
                            passField.copy()
                            passField.deselect()
                        }
                        PlasmaComponents.ToolTip { text: "Copy, to enter it on the other device" }
                    }
                    PlasmaComponents.ToolButton {
                        icon.name: "view-refresh"
                        onClicked: {
                            passField.text = Plasmoid.generatePassphrase()
                            showPass.checked = true
                        }
                        PlasmaComponents.ToolTip { text: "Generate a strong passphrase" }
                    }
                }

                PlasmaComponents.Label {
                    text: "Addresses:"
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                }
                PlasmaComponents.TextField {
                    id: peersField
                    Layout.fillWidth: true
                    placeholderText: "Optional, e.g. 192.168.1.20 — only if devices do not find each other"
                    onAccepted: overlay.accept()
                }
            }

            PlasmaComponents.Label {
                Layout.fillWidth: true
                visible: overlay.networkOn && passField.text !== "" && !overlay.passphraseOk
                text: "The passphrase needs at least 8 characters."
                color: Kirigami.Theme.negativeTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }

            // ── Status ──────────────────────────────────────────────
            PlasmaComponents.Label {
                Layout.fillWidth: true
                visible: overlay.networkOn
                wrapMode: Text.WordWrap
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                opacity: 0.75
                color: overlay.netStatus.state === "error" ? Kirigami.Theme.negativeTextColor
                                                           : Kirigami.Theme.textColor
                text: {
                    var s = overlay.netStatus
                    if (!overlay.roomActive)
                        return "The node goes online when its room is switched on."
                    if (!s.state)
                        return "Not online yet — save to connect."
                    if (s.state === "error")
                        return s.error
                    if (s.state !== "online")
                        return "Connecting…"
                    var peers = s.peers || []
                    var line = "Online on UDP port " + s.port + ". "
                    if (peers.length === 0)
                        return line + "No other device on this channel yet. If one should be there, check that its channel and passphrase match and that the firewalls allow UDP ports 47780 and 47781."
                    var names = []
                    for (var i = 0; i < peers.length; i++)
                        names.push(peers[i].name + " (" + peers[i].address + ")")
                    return line + "Connected: " + names.join(", ")
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
                    text: "Save"
                    icon.name: "document-save"
                    enabled: overlay.valid
                    onClicked: overlay.accept()
                }
            }
        }
    }
}
