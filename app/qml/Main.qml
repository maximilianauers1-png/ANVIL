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
                    wireColor: Theme.wire
                    meshView: App.display === "mesh"
                    scenery: Theme.name === "studio" ? App.scenery : "none"
                    sculptMode: App.mode === "Sculpt"
                    objectColors: Theme.objectColors

                    // Radial menu key (default Space): hold, flick, release.
                    Keys.onPressed: (e) => {
                        if (drawOverlay.handleKey(e)) {
                            e.accepted = true
                        } else if (viewport.tool === "pick" && (e.key === Qt.Key_Return || e.key === Qt.Key_Enter)) {
                            viewport.finishPick()
                            e.accepted = true
                        } else if (e.key === Qt.Key_Space && !e.isAutoRepeat) {
                            viewport.radialBegin(App.selection.length > 0 ? "selection" : "empty")
                            e.accepted = true
                        } else if (e.key === Qt.Key_Escape) {
                            if (sketchEditor.active && viewport.tool === "select") sketchEditor.close()
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
                DrawOverlay { id: drawOverlay; anchors.fill: parent; view: viewport }
                SculptPanel {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: Theme.unit * 2
                    view: viewport
                    visible: viewport.sculptMode
                }
                // brush cursor
                Rectangle {
                    visible: viewport.sculptMode && viewport.brushTarget !== ""
                    x: viewport.brushPos.x - width / 2
                    y: viewport.brushPos.y - height / 2
                    width: 2 * viewport.brushScreenRadius
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.width: 1.5
                    border.color: viewport.sculptBrush === "carve" ? Theme.danger : Theme.accent
                    Rectangle { anchors.centerIn: parent; width: 3; height: 3; radius: 1.5; color: parent.border.color }
                }
                SketchEditor { id: sketchEditor; anchors.fill: parent; view: viewport }
                SketchBar {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.unit * 2
                    view: viewport
                    visible: sketch !== "" && viewport.tool === "select" && !sketchEditor.active
                    onEditRequested: (name) => sketchEditor.open(name)
                }
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
                        AButton {
                            visible: viewport.tool === "pick" || (viewport.tool === "draw" && viewport.drawKind === "polyline")
                            enabled: viewport.tool === "pick" ? viewport.pickedTools.length > 0 : viewport.drawPoints.length >= 2
                            text: viewport.tool === "pick" ? "Cut (" + viewport.pickedTools.length + ")" : "Finish"
                            onClicked: viewport.tool === "pick" ? viewport.finishPick() : viewport.finishDraw()
                        }
                        AButton { text: "Cancel"; onClicked: viewport.cancelTool() }
                    }
                }
                // empty-state hint
                Column {
                    visible: App.objects.length === 0
                    anchors.centerIn: parent
                    spacing: 6
                    opacity: 0.85
                    ALabel { anchors.horizontalCenter: parent.horizontalCenter; text: "Right-click and drag (or hold " + App.radialKey + ") for the radial menu"; color: Theme.sceneText }
                    ALabel { anchors.horizontalCenter: parent.horizontalCenter; text: "Type in the command bar:  name to search  ·  > Python  ·  ? local AI"; color: Theme.sceneText }
                    ALabel { anchors.horizontalCenter: parent.horizontalCenter; text: "Drop STL files here"; color: Theme.sceneText }
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
            else if (tool === "import_step_dialog") fileDialogs.importStep()
            else if (tool === "export_step_dialog") fileDialogs.exportStep()
            else if (tool === "sketch_edit" && App.selection.length === 1) sketchEditor.open(App.selection[0])
            else if (tool.startsWith("sculpt_brush:")) viewport.sculptBrush = tool.substring(13)
        }
    }
    FileDialogs { id: fileDialogs }

    Shortcut { sequences: [StandardKey.Undo]; enabled: !win.typing; onActivated: App.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Y"]; enabled: !win.typing; onActivated: App.redo() }
    Shortcut { sequence: StandardKey.Save; onActivated: App.documentPath !== "" ? App.saveDocument(App.documentPath) : fileDialogs.save() }
    Shortcut { sequence: "Ctrl+K"; onActivated: commandBar.focusBar() }
}
