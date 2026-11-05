import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

Rectangle {
    id: bar
    signal exportRequested()
    signal customizeRadial()
    implicitHeight: Theme.barHeight
    color: Theme.panel
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.unit * 2
        anchors.rightMargin: Theme.unit
        spacing: Theme.unit

        Text {
            text: "ANVIL"
            color: Theme.accent
            font.family: Theme.font
            font.pixelSize: Theme.fontLarge
            font.bold: true
            font.letterSpacing: 3
        }
        Item { width: Theme.unit * 2 }

        // Mode bar. Modes become available with their milestone.
        Repeater {
            model: [
                { name: "Design", ms: "" }, { name: "Sculpt", ms: "M3" }, { name: "Mini", ms: "M4" },
                { name: "Lab", ms: "M5" }, { name: "Print", ms: "M5" }, { name: "AI", ms: "M6" }
            ]
            delegate: AButton {
                objectName: "mode_" + modelData.name
                text: modelData.name
                checkable: true
                checked: App.mode === modelData.name
                enabled: modelData.ms === ""
                tip: modelData.ms === "" ? "" : modelData.name + " mode arrives with milestone " + modelData.ms
                onClicked: App.mode = modelData.name
            }
        }

        Item { Layout.fillWidth: true }

        AButton { objectName: "btnOpen"; text: "Open"; tip: "Open an .anvil document"; onClicked: App.runCommand("file.open") }
        AButton { objectName: "btnSave"; text: "Save"; tip: "Save as .anvil (a Python script)"; onClicked: App.runCommand("file.save") }
        AButton { objectName: "btnExport"; text: "Export STL"; primary: true; onClicked: bar.exportRequested() }
        Rectangle { width: 1; height: 22; color: Theme.border }
        AButton { objectName: "btnUndo"; text: "↶ Undo"; enabled: App.canUndo; onClicked: App.undo() }
        AButton { objectName: "btnRedo"; text: "Redo ↷"; enabled: App.canRedo; onClicked: App.redo() }
        Rectangle { width: 1; height: 22; color: Theme.border }

        // Global unit switch: changes the display only, never stored values.
        Row {
            spacing: 0
            ALabel { text: "Unit "; dim: true; anchors.verticalCenter: parent.verticalCenter }
            Repeater {
                model: ["mm", "cm", "m", "in"]
                delegate: AButton {
                    objectName: "unit_" + modelData
                    text: modelData
                    checkable: true
                    checked: App.unit === modelData
                    leftPadding: 6
                    rightPadding: 6
                    implicitWidth: 40
                    onClicked: App.requestUnit(modelData)
                }
            }
        }
        AButton {
            objectName: "btnTheme"
            text: Theme.dark ? "☾ Dark" : "☀ Light"
            tip: "Switch theme"
            onClicked: App.requestTheme(Theme.dark ? "light" : "dark")
        }
        AButton { text: "◎"; tip: "Customize the radial menu"; onClicked: bar.customizeRadial() }
    }
}
