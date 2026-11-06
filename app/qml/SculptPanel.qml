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
        SliderField {
            label: "Size"
            sliderName: "sculptRadius"; fieldName: "sculptRadiusField"
            kind: "length"; log: true; from: 0.2; to: 20             // brush radius, mm
            value: root.view.sculptRadius
            tip: "Brush radius (type e.g. 1.5 or 1.5 mm)"
            onEdited: function (v) { root.view.sculptRadius = v }
        }
        SliderField {
            label: "Intensity"
            sliderName: "sculptStrength"; fieldName: "sculptStrengthField"
            kind: "percent"; from: 0.05; to: 1
            value: root.view.sculptStrength
            tip: "At 100 % one stroke raises the surface by about half the brush size"
            onEdited: function (v) { root.view.sculptStrength = v }
        }
        SliderField {
            label: "Falloff"
            sliderName: "sculptFalloff"; fieldName: "sculptFalloffField"
            kind: "number"; decimals: 2; log: true; from: 0.4; to: 3  // exponent: < 0.8 soft, > 1.4 sharp
            value: root.view.sculptFalloff
            tip: "Edge profile exponent: 0.4 very soft … 1 medium … 3 sharp"
            onEdited: function (v) { root.view.sculptFalloff = Math.round(v * 100) / 100 }
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
        SliderField {
            visible: root.view.sculptBrush === "texture"
            label: "Cell"
            fieldName: "sculptCellField"
            kind: "length"; log: true; from: 0.2; to: 3
            value: root.view.sculptPatternSize
            tip: "Size of one scale / ring / rivet / stone"
            onEdited: function (v) { root.view.sculptPatternSize = Math.round(v * 100) / 100 }
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
