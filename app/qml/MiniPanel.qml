import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Mini Forge: import (Tripo, Meshy, ...) -> pose -> weapons ->
// base -> merge -> sculpt details -> print.
// Every button is one Python command; pose angles and proportions are normal
// parameters in the panel on the right (type or drag their names).
Rectangle {
    id: root
    readonly property string sel: App.selection.length === 1 ? App.selection[0] : ""
    function kindOf(n) {
        for (var i = 0; i < App.objects.length; ++i) if (App.objects[i].name === n) return App.objects[i].kind
        return ""
    }
    readonly property bool selModel: sel !== "" && kindOf(sel) === "Model"
    readonly property bool selFigure: sel !== "" && (kindOf(sel) === "Figure" || selModel)
    readonly property bool selAttach: sel !== "" && kindOf(sel) === "Attach"
    property string hand: "r"
    function modelAttr(k) { App.objects; return root.sel !== "" ? App.objectAttr(root.sel, k) : "" }
    readonly property var printable: App.objects.filter(function (o) { return o.visible && o.kind !== "Sketch" }).map(function (o) { return o.name })
    readonly property var figures: App.objects.filter(function (o) { return o.visible && (o.kind === "Model" || o.kind === "Figure" || o.kind === "Sculpt") }).map(function (o) { return o.name })
    readonly property var bases: App.objects.filter(function (o) { return o.kind === "MiniBase" }).map(function (o) { return o.name })
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.border
    width: col.implicitWidth + 16
    height: col.implicitHeight + 16

    component Section: ALabel { dim: true; font.pixelSize: Theme.fontSmall; topPadding: 4 }

    ColumnLayout {
        id: col
        anchors.centerIn: parent
        spacing: 5
        ALabel { text: "MINI FORGE"; font.bold: true }

        Section { text: "1  FIGURE" }
        AButton {
            objectName: "miniImport"
            Layout.fillWidth: true
            text: "Import mini (Tripo, Meshy: GLB / OBJ)…"
            onClicked: App.runCommand("mini.import")
        }
        RowLayout {
            visible: root.selFigure
            spacing: 4
            ALabel { text: "Height" }
            Repeater {
                model: [28, 32, 54, 75]
                delegate: AButton {
                    text: modelData + " mm"
                    onClicked: App.execute("edit(\"" + root.sel + "\", height=" + modelData + "*mm)", "ui")
                }
            }
        }

        Section { text: "2  POSE"; visible: root.selFigure }
        GridLayout {
            visible: root.selFigure
            columns: 5
            columnSpacing: 3
            rowSpacing: 3
            Repeater {
                model: ["stand", "walk", "attack", "cast", "guard"]
                delegate: AButton {
                    objectName: "pose_" + modelData
                    text: modelData.charAt(0).toUpperCase() + modelData.slice(1)
                    onClicked: App.runCommand("mini.pose_" + modelData)
                }
            }
        }
        RowLayout {
            visible: root.selModel
            spacing: 4
            ALabel { text: "Rest" }
            AButton {
                objectName: "restNormalize"; text: "Arms down"
                tip: "Pose 0 = arms hanging (T- and A-pose files are lowered)"
                primary: root.modelAttr("rest") !== "file"
                onClicked: App.execute("model_options(\"" + root.sel + "\", rest=\"normalize\")", "ui")
            }
            AButton {
                objectName: "restFile"; text: "As in file"
                tip: "Pose 0 = the pose stored in the file (less deformation for posed statues)"
                primary: root.modelAttr("rest") === "file"
                onClicked: App.execute("model_options(\"" + root.sel + "\", rest=\"file\")", "ui")
            }
        }
        RowLayout {
            visible: root.selModel
            spacing: 4
            ALabel { text: "Weights" }
            AButton {
                objectName: "weightsFile"; text: "From file"
                tip: "Skin weights of the file's rig (Tripo, Meshy, Mixamo)"
                primary: root.modelAttr("weights") !== "auto"
                onClicked: App.execute("model_options(\"" + root.sel + "\", weights=\"file\")", "ui")
            }
            AButton {
                objectName: "weightsAuto"; text: "ANVIL (geodesic)"
                tip: "Recompute the weights through the solid: use it when posing stretches or tears the mesh"
                primary: root.modelAttr("weights") === "auto"
                onClicked: App.execute("model_options(\"" + root.sel + "\", weights=\"auto\")", "ui")
            }
        }
        ALabel {
            visible: root.selFigure
            Layout.maximumWidth: 330
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: "Fine-tune every joint on the right: drag a name (e.g. l_elbow, r_shoulder_fwd, spine) sideways or type a value."
        }

        Section { text: "3  WEAPONS"; visible: root.selModel || root.selAttach }
        RowLayout {
            visible: root.selModel
            spacing: 4
            ALabel { text: "Hand" }
            AButton { objectName: "handR"; text: "Right"; primary: root.hand === "r"; onClicked: root.hand = "r" }
            AButton { objectName: "handL"; text: "Left"; primary: root.hand === "l"; onClicked: root.hand = "l" }
        }
        GridLayout {
            visible: root.selModel
            columns: 3
            columnSpacing: 3
            rowSpacing: 3
            Repeater {
                model: [["longsword", "Longsword"], ["zweihander", "Zweihänder"], ["buster", "Buster sword"],
                        ["dagger", "Dagger"], ["axe", "Axe"], ["spear", "Spear"]]
                delegate: AButton {
                    objectName: "equip_" + modelData[0]
                    text: modelData[1]
                    onClicked: App.runCommand("mini.equip_" + modelData[0] + "_" + root.hand)
                }
            }
        }
        ALabel {
            visible: root.selModel || root.selAttach
            Layout.maximumWidth: 330
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: root.selAttach
                  ? "Adjust the grip on the right: pitch (blade up/down), yaw, roll, slide. Length, width, thickness: open the item (src)."
                  : "The weapon follows every pose change. Select it to adjust pitch, yaw, roll and slide."
        }

        Section { text: "4  BASE" }
        RowLayout {
            spacing: 4
            Repeater {
                model: [25, 32, 40, 50]
                delegate: AButton {
                    objectName: "base" + modelData
                    text: modelData + " mm"
                    onClicked: App.runCommand("mini.base" + modelData)
                }
            }
        }
        AButton {
            objectName: "miniOnBase"
            visible: root.figures.length > 0 && root.bases.length > 0
            Layout.fillWidth: true
            text: "Stand " + (root.figures.length ? root.figures[0] : "") + " on " + (root.bases.length ? root.bases[0] : "")
            onClicked: {
                App.selection = [root.figures[0], root.bases[0]]
                App.runCommand("mini.on_base")
            }
        }

        Section { text: "5  PRINT" }
        AButton {
            objectName: "miniMerge"
            Layout.fillWidth: true
            enabled: App.selection.length >= 2 || root.printable.length >= 2
            text: App.selection.length >= 2 ? "Merge " + App.selection.length + " selected for print"
                                            : "Merge all visible (" + root.printable.length + ") for print"
            onClicked: {
                if (App.selection.length < 2) App.selection = root.printable
                App.runCommand("mini.merge")
            }
        }

        Section { text: "6  DETAILS" }
        AButton {
            objectName: "miniSculpt"
            Layout.fillWidth: true
            text: "Sculpt details (scales, chainmail, …)"
            enabled: App.objects.length > 0
            onClicked: App.mode = "Sculpt"
        }
        AButton {
            Layout.fillWidth: true
            text: "Check & export STL…"
            enabled: App.objects.length > 0
            onClicked: App.runCommand("file.export_stl")
        }
    }
}
