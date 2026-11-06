import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Sculpt mode tools. One stroke = one sculpt(...) command (one undo step).
// Shift: smooth · Ctrl: invert · drag on empty space: orbit.
Rectangle {
    id: root
    property var view
    readonly property string sel: App.selection.length === 1 ? App.selection[0] : ""
    readonly property bool selIsClay: sel !== "" && App.objects.some(function (o) { return o.name === sel && o.kind === "Sculpt" })
    readonly property bool selReady: selIsClay && view.sculptObjects.indexOf(sel) >= 0
    readonly property bool anyClay: view.sculptObjects.length > 0
    readonly property real voxel: { App.objects; return sel !== "" ? App.sculptVoxel(sel) : 0 }
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
        ALabel { text: "SCULPT"; font.bold: true }

        Section { text: "BRUSH" }
        GridLayout {
            columns: 3
            columnSpacing: 3
            rowSpacing: 3
            Repeater {
                model: [["draw", "Draw", "Build up along the surface normal"],
                        ["carve", "Carve", "Dig in"],
                        ["clay", "Clay", "Build flat layers (best for forms)"],
                        ["inflate", "Inflate", "Swell / shrink along the local normal"],
                        ["crease", "Crease", "Sharp grooves: folds, eyelids, cloth"],
                        ["pinch", "Pinch", "Sharpen ridges and edges"],
                        ["smooth", "Smooth", "Relax the surface (also: hold Shift)"],
                        ["grab", "Grab", "Pull the surface with the pointer"],
                        ["flatten", "Flatten", "Plane off at the first touched point"],
                        ["texture", "Texture", "Paint scales, chainmail, rivets or stone"]]
                delegate: AButton {
                    objectName: "brush_" + modelData[0]
                    text: modelData[1]
                    tip: modelData[2]
                    checkable: true
                    checked: root.view.sculptBrush === modelData[0]
                    Layout.fillWidth: true
                    onClicked: root.view.sculptBrush = modelData[0]
                }
            }
        }

        Section { text: "SETTINGS" }
        GridLayout {
            columns: 3
            columnSpacing: 6
            rowSpacing: 2
            ALabel { text: "Size" }
            Slider {
                objectName: "sculptRadius"
                Layout.preferredWidth: 170
                from: Math.log(0.2); to: Math.log(20)          // logarithmic 0.2 .. 20 mm
                value: Math.log(root.view.sculptRadius)
                onMoved: root.view.sculptRadius = Math.exp(value)
            }
            ALabel { text: App.formatLength(root.view.sculptRadius); Layout.preferredWidth: 64 }
            ALabel { text: "Intensity" }
            Slider {
                objectName: "sculptStrength"
                Layout.preferredWidth: 170
                from: 0.05; to: 1
                value: root.view.sculptStrength
                onMoved: root.view.sculptStrength = value
            }
            ALabel { text: Math.round(root.view.sculptStrength * 100) + " %"; Layout.preferredWidth: 64 }
            ALabel { text: "Falloff" }
            Slider {
                objectName: "sculptFalloff"
                Layout.preferredWidth: 170
                from: Math.log(0.4); to: Math.log(3)            // exponent: soft .. sharp
                value: Math.log(root.view.sculptFalloff)
                onMoved: root.view.sculptFalloff = Math.round(Math.exp(value) * 20) / 20
            }
            ALabel { text: root.view.sculptFalloff < 0.8 ? "soft" : root.view.sculptFalloff > 1.4 ? "sharp" : "medium"; Layout.preferredWidth: 64 }
        }
        RowLayout {
            visible: root.view.sculptBrush === "texture"
            spacing: 4
            ALabel { text: "Pattern" }
            Repeater {
                model: [["scales", "Scales"], ["chainmail", "Mail"], ["rivets", "Rivets"], ["stone", "Stone"]]
                delegate: AButton {
                    objectName: "pattern_" + modelData[0]
                    text: modelData[1]
                    checkable: true
                    checked: root.view.sculptPattern === modelData[0]
                    onClicked: root.view.sculptPattern = modelData[0]
                }
            }
        }
        RowLayout {
            visible: root.view.sculptBrush === "texture"
            spacing: 6
            ALabel { text: "Cell" }
            Slider {
                Layout.preferredWidth: 170
                from: Math.log(0.2); to: Math.log(3)
                value: Math.log(root.view.sculptPatternSize)
                onMoved: root.view.sculptPatternSize = Math.round(Math.exp(value) * 100) / 100
            }
            ALabel { text: App.formatLength(root.view.sculptPatternSize) }
        }
        RowLayout {
            spacing: 4
            ALabel { text: "Mirror" }
            Repeater {
                model: [["", "off"], ["x", "X"], ["y", "Y"], ["z", "Z"]]
                delegate: AButton {
                    objectName: "sym_" + (modelData[0] === "" ? "off" : modelData[0])
                    text: modelData[1]
                    checkable: true
                    checked: root.view.sculptSymmetry === modelData[0]
                    onClicked: root.view.sculptSymmetry = modelData[0]
                }
            }
            AButton {
                objectName: "sculptInvert"
                text: "Invert"
                tip: "Reverse the brush (also: hold Ctrl)"
                checkable: true
                checked: root.view.sculptInvert
                onClicked: root.view.sculptInvert = !root.view.sculptInvert
            }
        }

        Section { text: "CLAY" }
        AButton {
            objectName: "makeSculptable"
            visible: root.sel !== "" && !root.selIsClay
            text: "Make " + root.sel + " sculptable"
            Layout.fillWidth: true
            onClicked: App.runCommand("sculpt.make")
        }
        RowLayout {
            visible: root.selReady
            spacing: 4
            ALabel { text: "Detail " + App.formatLength(root.voxel) }
            AButton {
                objectName: "detailUp"
                text: "Finer"
                tip: "Resample at half the voxel size (more detail, slower)"
                enabled: root.voxel / 2 >= 0.01
                onClicked: App.execute("resculpt(\"" + root.sel + "\", " + (root.voxel / 2).toPrecision(3) + "*mm)", "ui")
            }
            AButton {
                text: "Coarser"
                tip: "Resample at twice the voxel size (faster, smoother)"
                onClicked: App.execute("resculpt(\"" + root.sel + "\", " + (root.voxel * 2).toPrecision(3) + "*mm)", "ui")
            }
        }
        ALabel {
            visible: root.selIsClay && !root.selReady
            text: "Preparing clay for " + root.sel + " …"
            color: Theme.accent
        }
        ALabel {
            Layout.maximumWidth: 320
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: root.anyClay
                  ? "Drag on clay to sculpt, on empty space to orbit. Click a part to turn it into clay. Shift: smooth · Ctrl: invert" +
                    (root.view.sculptUpdateMs > 0 ? " · " + root.view.sculptUpdateMs.toFixed(0) + " ms per update" : "")
                  : "Click a part to turn it into clay, then drag on it."
        }
    }
}
