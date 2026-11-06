import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Sculpt mode tools: brush, radius, strength; one stroke = one sculpt(...)
// command (one undo step). Hold Shift to smooth, Ctrl to invert draw/carve.
Rectangle {
    id: root
    property var view
    readonly property string sel: App.selection.length === 1 ? App.selection[0] : ""
    // derived from App.objects so it re-evaluates on every document change
    readonly property bool selSculptable: sel !== "" && App.objects.some(function (o) { return o.name === sel && o.kind === "Sculpt" })
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.border
    width: col.implicitWidth + 16
    height: col.implicitHeight + 16

    ColumnLayout {
        id: col
        anchors.centerIn: parent
        spacing: 6
        ALabel { text: "SCULPT"; font.bold: true; dim: true }
        GridLayout {
            columns: 5
            columnSpacing: 3
            rowSpacing: 3
            Repeater {
                model: [["draw", "Draw"], ["carve", "Carve"], ["smooth", "Smooth"], ["grab", "Grab"], ["flatten", "Flatten"]]
                delegate: AButton {
                    objectName: "brush_" + modelData[0]
                    text: modelData[1]
                    checkable: true
                    checked: root.view.sculptBrush === modelData[0]
                    onClicked: root.view.sculptBrush = modelData[0]
                }
            }
        }
        RowLayout {
            ALabel { text: "Radius"; Layout.preferredWidth: 64 }
            Slider {
                id: rad
                objectName: "sculptRadius"
                Layout.preferredWidth: 180
                // logarithmic 0.2 .. 20 mm
                from: Math.log(0.2); to: Math.log(20)
                value: Math.log(root.view.sculptRadius)
                onMoved: root.view.sculptRadius = Math.exp(value)
            }
            ALabel { text: App.formatLength(root.view.sculptRadius); Layout.preferredWidth: 70 }
        }
        RowLayout {
            ALabel { text: "Strength"; Layout.preferredWidth: 64 }
            Slider {
                objectName: "sculptStrength"
                Layout.preferredWidth: 180
                from: 0.05; to: 1
                value: root.view.sculptStrength
                onMoved: root.view.sculptStrength = value
            }
            ALabel { text: Math.round(root.view.sculptStrength * 100) + " %"; Layout.preferredWidth: 70 }
        }
        AButton {
            objectName: "makeSculptable"
            visible: root.sel !== "" && !root.selSculptable
            text: "Make " + root.sel + " sculptable"
            onClicked: App.runCommand("sculpt.make")
        }
        ALabel {
            Layout.maximumWidth: 300
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: root.selSculptable || App.objects.some(function (o) { return o.kind === "Sculpt" })
                  ? "Drag on clay to sculpt, on empty space to orbit. Shift: smooth · Ctrl: invert" +
                    (root.view.sculptUpdateMs > 0 ? " · last update " + root.view.sculptUpdateMs.toFixed(0) + " ms" : "")
                  : "Select an object and make it sculptable to start."
        }
    }
}
