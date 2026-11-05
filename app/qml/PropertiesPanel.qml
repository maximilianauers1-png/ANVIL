import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Right panel: scene list, context actions, parameters of the selection, variables.
Rectangle {
    id: panel
    property var view
    signal exportRequested()
    color: Theme.panel
    Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border }

    readonly property string single: App.selection.length === 1 ? App.selection[0] : ""
    property var tree: []
    function reloadTree() { tree = single !== "" ? App.nodeTree(single) : [] }
    onSingleChanged: reloadTree()
    Connections {
        target: App
        function onDocumentChanged() { panel.reloadTree() }
        function onUnitChanged() { panel.reloadTree() }
    }

    component SectionTitle: Text {
        color: Theme.textDim
        font.family: Theme.font
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.letterSpacing: 1.2
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: Theme.unit * 1.5
        contentHeight: col.implicitHeight
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: col
            width: parent.width
            spacing: Theme.unit

            // ---------------------------------------------------------------- scene
            SectionTitle { text: "SCENE  (" + App.objects.length + ")" }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(220, Math.max(36, objList.contentHeight + 4))
                color: Theme.field
                radius: Theme.radius
                border.color: Theme.border
                ListView {
                    id: objList
                    anchors.fill: parent
                    anchors.margins: 2
                    clip: true
                    model: App.objects
                    delegate: Rectangle {
                        objectName: "obj_" + modelData.name
                        width: objList.width
                        height: 26
                        radius: 3
                        readonly property bool sel: App.selection.indexOf(modelData.name) >= 0
                        color: sel ? Qt.rgba(1, 0.6, 0.18, 0.22) : objMouse.containsMouse ? Theme.hover : "transparent"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            anchors.rightMargin: 6
                            spacing: 6
                            Text {
                                text: modelData.visible ? "◉" : "○"
                                color: modelData.visible ? Theme.text : Theme.textFaint
                                font.pixelSize: 14
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    onClicked: App.toggleVisible(modelData.name)
                                }
                            }
                            ALabel {
                                text: modelData.name
                                Layout.fillWidth: true
                                color: (modelData.error || "") !== "" ? Theme.danger : Theme.text
                            }
                            ALabel {
                                text: (modelData.kind || "") + (modelData.triangles ? "  " + modelData.triangles + " △" : "")
                                dim: true
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                        MouseArea {
                            id: objMouse
                            anchors.fill: parent
                            anchors.leftMargin: 22
                            hoverEnabled: true
                            ToolTip.visible: containsMouse && (modelData.error || "") !== ""
                            ToolTip.text: modelData.error || ""
                            onClicked: (m) => App.pick(modelData.name, (m.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0)
                            onDoubleClicked: { renameField.text = modelData.name; renameField.target = modelData.name; renameField.visible = true; renameField.forceActiveFocus() }
                        }
                    }
                }
                AField {
                    id: renameField
                    property string target: ""
                    visible: false
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    onAccepted: { App.renameObject(target, text); visible = false }
                    onActiveFocusChanged: if (!activeFocus) visible = false
                }
            }

            // ---------------------------------------------------------------- actions (mouse-only access)
            SectionTitle { text: App.selection.length === 0 ? "CREATE" : "ACTIONS" }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                visible: App.selection.length === 0
                Repeater {
                    model: [["create.cylinder", "Cylinder"], ["create.box", "Box"], ["create.sphere", "Sphere"],
                            ["create.torus", "Torus"], ["create.cone", "Cone"], ["file.import_stl", "Import STL"]]
                    delegate: AButton { objectName: "btn_" + modelData[0]; text: modelData[1]; onClicked: App.runCommand(modelData[0]) }
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                visible: App.selection.length > 0
                AButton { objectName: "btnMove"; text: "Move"; tip: "Two snapped points: base, then destination"; onClicked: App.runCommand("edit.move") }
                AButton { objectName: "btnUnion"; text: "Union"; enabled: App.selection.length >= 2; onClicked: App.runCommand("boolean.union") }
                AButton { objectName: "btnSubtract"; text: "Subtract"; enabled: App.selection.length >= 2; tip: "First selected minus the others"; onClicked: App.runCommand("boolean.subtract") }
                AButton { objectName: "btnIntersect"; text: "Intersect"; enabled: App.selection.length >= 2; onClicked: App.runCommand("boolean.intersect") }
                AButton { objectName: "btnDuplicate"; text: "Duplicate"; enabled: App.selection.length === 1; onClicked: App.runCommand("edit.duplicate") }
                AButton { objectName: "btnHide"; text: "Hide"; onClicked: App.runCommand("edit.hide") }
                AButton { objectName: "btnDelete"; text: "Delete"; danger: true; onClicked: App.runCommand("edit.delete") }
                AButton { text: "Export STL"; onClicked: panel.exportRequested() }
            }

            // ---------------------------------------------------------------- parameters
            SectionTitle { text: "PARAMETERS"; visible: panel.single !== "" }
            ALabel {
                visible: App.selection.length > 1
                text: App.selection.length + " objects selected (order: " + App.selection.join(", ") + ")"
                dim: true
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Repeater {
                model: panel.tree
                delegate: ColumnLayout {
                    id: nodeBox
                    readonly property var row: modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: row.depth * 12
                    spacing: 4
                    ALabel {
                        text: (row.path === "" ? "" : row.path + ": ") + row.kind
                        font.bold: true
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                    }
                    Repeater {
                        model: nodeBox.row.params
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            ALabel {
                                text: modelData.key
                                dim: true
                                Layout.preferredWidth: 96
                                ToolTip.visible: keyMouse.containsMouse
                                ToolTip.text: modelData.help
                                MouseArea { id: keyMouse; anchors.fill: parent; hoverEnabled: true }
                            }
                            AField {
                                objectName: "param_" + nodeBox.row.path.replace(/\//g, "_") + "_" + modelData.key
                                Layout.fillWidth: true
                                text: modelData.text
                                placeholderText: modelData.key === "d2" ? "= d" : ""
                                font.family: modelData.expr !== "" ? Theme.mono : Theme.font
                                invalid: text !== "" && App.validateField(modelData.dim === "Angle" ? "0 mm" : text) !== "" && modelData.dim !== "Angle"
                                ToolTip.visible: activeFocus && invalid
                                ToolTip.text: App.validateField(text)
                                onAccepted: {
                                    if (text !== modelData.text)
                                        App.setParam(panel.single, nodeBox.row.path, modelData.key, text)
                                }
                            }
                            ALabel {
                                visible: modelData.expr !== ""
                                text: "= " + modelData.value
                                dim: true
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                    }
                }
            }
            ALabel {
                visible: panel.single !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                dim: true
                font.pixelSize: Theme.fontSmall
                text: "Fields accept expressions: 12.5 cm + 3 mm,  2*r,  1/2 in.  A bare number uses the display unit (" + App.unit + ")."
            }

            // ---------------------------------------------------------------- variables
            SectionTitle { text: "VARIABLES" }
            Repeater {
                model: App.variables
                delegate: RowLayout {
                    Layout.fillWidth: true
                    ALabel { text: modelData.name; font.family: Theme.mono; Layout.preferredWidth: 80 }
                    AField {
                        Layout.fillWidth: true
                        text: modelData.expr !== "" ? modelData.expr : modelData.display
                        onAccepted: App.setVariable(modelData.name, text)
                    }
                    ALabel { visible: modelData.expr !== ""; text: "= " + modelData.display; dim: true }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                AField {
                    id: newVar
                    objectName: "newVariable"
                    Layout.fillWidth: true
                    placeholderText: "new variable, e.g.  r = 12.5 mm"
                    onAccepted: {
                        const i = text.indexOf("=")
                        if (i > 0) {
                            App.setVariable(text.substring(0, i).trim(), text.substring(i + 1).trim())
                            text = ""
                        }
                    }
                }
            }
        }
    }
}
