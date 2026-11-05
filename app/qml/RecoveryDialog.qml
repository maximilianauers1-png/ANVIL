import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

Popup {
    id: dlg
    objectName: "recoveryDialog"
    modal: true
    visible: App.recoveryAvailable
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: Overlay.overlay
    width: 500
    padding: Theme.unit * 2
    background: Rectangle { color: Theme.panelAlt; border.color: Theme.warn; radius: Theme.radius * 2 }
    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.unit
        ALabel { text: "Restore unsaved work"; font.pixelSize: Theme.fontLarge; font.bold: true }
        ALabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: App.recoveryInfo }
        ALabel {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: "Every command was written to the journal before it ran. Restoring replays all completed commands; a command that was still running when the session ended is listed in the console and not replayed."
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AButton { text: "Discard"; danger: true; onClicked: App.discardRecovery() }
            AButton { objectName: "btnRestore"; text: "Restore"; primary: true; onClicked: App.recover() }
        }
    }
}
