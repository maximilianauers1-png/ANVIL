import QtQuick
import QtQuick.Layouts
import Anvil.Native

// One-click follow-ups for a selected sketch (same commands as the radial
// 'sketch' ring): the shortest path from a drawn shape to a part or a cut.
Rectangle {
    id: root
    property var view
    property string sketch: App.selection.length === 1 && App.objects.some(function (o) { return o.name === App.selection[0] && o.kind === "Sketch" }) ? App.selection[0] : ""
    signal editRequested(string name)
    visible: sketch !== "" && view.tool === "select"
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.border
    width: row.implicitWidth + 12
    height: row.implicitHeight + 10
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 4
        ALabel { text: root.sketch; font.bold: true; rightPadding: 6 }
        AButton { objectName: "sketchExtrude"; text: "Extrude"; onClicked: App.runCommand("sketch.extrude") }
        AButton { objectName: "sketchSolidify"; text: "Solidify"; onClicked: App.runCommand("sketch.solidify") }
        AButton {
            objectName: "sketchCut"
            text: App.sketchTarget(root.sketch) !== "" ? "Cut through " + App.sketchTarget(root.sketch) : "Cut through…"
            onClicked: App.runCommand("sketch.cut")
        }
        AButton { text: "Revolve"; onClicked: App.runCommand("sketch.revolve") }
        AButton { objectName: "sketchEdit"; text: "Edit"; onClicked: root.editRequested(root.sketch) }
    }
}
