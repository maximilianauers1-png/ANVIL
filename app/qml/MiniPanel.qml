import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Mini Forge: figure -> pose -> base -> sculpt details -> print.
// Every button is one Python command; pose angles and proportions are normal
// parameters in the panel on the right (type or drag their names).
Rectangle {
    id: root
    readonly property string sel: App.selection.length === 1 ? App.selection[0] : ""
    function kindOf(n) {
        for (var i = 0; i < App.objects.length; ++i) if (App.objects[i].name === n) return App.objects[i].kind
        return ""
    }
    readonly property bool selFigure: sel !== "" && kindOf(sel) === "Figure"
    readonly property var figures: App.objects.filter(function (o) { return o.kind === "Figure" || o.kind === "Sculpt" }).map(function (o) { return o.name })
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
        RowLayout {
            spacing: 4
            AButton { objectName: "miniDemon"; text: "New demon"; onClicked: App.runCommand("mini.demon") }
            AButton { objectName: "miniHuman"; text: "New human"; onClicked: App.runCommand("mini.human") }
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
        ALabel {
            visible: root.selFigure
            Layout.maximumWidth: 330
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: "Fine-tune every joint on the right: drag a name (e.g. l_elbow, tail_curl) sideways. Build and heroic change the proportions."
        }

        Section { text: "3  BASE" }
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

        Section { text: "4  DETAILS" }
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
