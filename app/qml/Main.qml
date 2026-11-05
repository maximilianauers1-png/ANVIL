import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

ApplicationWindow {
    id: win
    visible: true
    width: 1600
    height: 960
    minimumWidth: 1000
    minimumHeight: 640
    title: "ANVIL " + App.version + (App.documentPath !== "" ? "  -  " + App.documentPath : "")
    color: Theme.bg
    font.family: Theme.font
    font.pixelSize: Theme.fontSize

    readonly property bool typing: activeFocusItem !== null && activeFocusItem.hasOwnProperty("cursorPosition")

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TopBar {
            Layout.fillWidth: true
            onExportRequested: exportDialog.openFor(App.selection)
            onCustomizeRadial: radialEditor.open()
        }
        CommandBar {
            id: commandBar
            Layout.fillWidth: true
            z: 10
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Item {
                id: viewArea
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Viewport {
                    id: viewport
                    objectName: "viewport"
                    anchors.fill: parent
                    focus: true
                    backgroundColor: Theme.viewportBg
                    gridColor: Theme.grid
                    edgeColor: Theme.edge
                    objectColors: Theme.objectColors

                    // Radial menu key (default Space): hold, flick, release.
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Space && !e.isAutoRepeat) {
                            viewport.radialBegin(App.selection.length > 0 ? "selection" : "empty")
                            e.accepted = true
                        } else if (e.key === Qt.Key_Escape) {
                            viewport.radialCancel()
                            viewport.cancelTool()
                            App.clearPreview()
                            e.accepted = true
                        } else if (e.key === Qt.Key_Delete && App.selection.length > 0) {
                            App.runCommand("edit.delete")
                            e.accepted = true
                        }
                    }
                    Keys.onReleased: (e) => {
                        if (e.key === Qt.Key_Space && !e.isAutoRepeat) {
                            viewport.radialEnd()
                            e.accepted = true
                        }
                    }
                }
                SnapOverlay { anchors.fill: parent; view: viewport }
                RadialMenu { anchors.fill: parent; view: viewport }
                ViewCube {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: Theme.unit * 2
                    view: viewport
                }
                AxisTriad {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.unit * 2
                    view: viewport
                }
                // tool hint (move tool)
                Rectangle {
                    visible: viewport.toolHint !== ""
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: Theme.unit * 2
                    radius: Theme.radius
                    color: Theme.panelAlt
                    border.color: Theme.accent
                    width: hintRow.implicitWidth + 24
                    height: 34
                    Row {
                        id: hintRow
                        anchors.centerIn: parent
                        spacing: 12
                        ALabel { text: viewport.toolHint; anchors.verticalCenter: parent.verticalCenter }
                        AButton { text: "Cancel"; onClicked: viewport.cancelTool() }
                    }
                }
                // empty-state hint
                Column {
                    visible: App.objects.length === 0
                    anchors.centerIn: parent
                    spacing: 6
                    opacity: 0.85
                    ALabel { anchors.horizontalCenter: parent.horizontalCenter; text: "Right-click and drag (or hold " + App.radialKey + ") for the radial menu"; dim: true }
                    ALabel { anchors.horizontalCenter: parent.horizontalCenter; text: "Type in the command bar:  name to search  ·  > Python  ·  ? local AI"; dim: true }
                    ALabel { anchors.horizontalCenter: parent.horizontalCenter; text: "Drop STL files here"; dim: true }
                }
                DropArea {
                    anchors.fill: parent
                    onDropped: (drop) => { if (drop.hasUrls) App.dropFiles(drop.urls) }
                }
            }
            PropertiesPanel {
                Layout.preferredWidth: Theme.panelWidth
                Layout.fillHeight: true
                view: viewport
                onExportRequested: exportDialog.openFor(App.selection)
            }
        }
        BottomPanel { Layout.fillWidth: true }
        StatusBar { Layout.fillWidth: true; view: viewport }
    }

    ExportDialog { id: exportDialog }
    RecoveryDialog {}
    RadialEditor { id: radialEditor }

    Connections {
        target: App
        function onUiToolRequested(tool) {
            if (tool === "export_dialog") exportDialog.openFor(App.selection)
            else if (tool === "import_dialog") fileDialogs.importStl()
            else if (tool === "save_dialog") fileDialogs.save()
            else if (tool === "open_dialog") fileDialogs.open()
        }
    }
    FileDialogs { id: fileDialogs }

    Shortcut { sequences: [StandardKey.Undo]; enabled: !win.typing; onActivated: App.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Y"]; enabled: !win.typing; onActivated: App.redo() }
    Shortcut { sequence: StandardKey.Save; onActivated: App.documentPath !== "" ? App.saveDocument(App.documentPath) : fileDialogs.save() }
    Shortcut { sequence: "Ctrl+K"; onActivated: commandBar.focusBar() }
}
