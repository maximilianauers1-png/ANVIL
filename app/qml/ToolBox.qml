import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Toolbox on the left edge: one icon per tool, top to bottom. Each tool is a
// mouse workflow ending in one Python command (one undo step); the active
// tool is highlighted, Esc returns to Select.
Rectangle {
    id: box
    property var view
    width: 48
    height: col.implicitHeight + 12
    radius: Theme.radius
    color: Theme.gearDark
    border.color: Theme.gearEdge

    function isActive(t) {
        var v = box.view
        if (t.cut) return v.tool === "cut" && v.cutKind === t.cut
        if (t.draw) return v.drawKind === t.draw
        if (t.id === "select") return v.tool === "select"
        if (t.id === "move") return v.tool === "move"
        return false
    }
    function activate(t) {
        if (t.cut) box.view.startCut(t.cut)
        else if (t.cmd) App.runCommand(t.cmd)
        else box.view.cancelTool()
    }

    readonly property var tools: [
        { id: "select", icon: "select", tip: "Select / orbit (Esc)" },
        { id: "move", icon: "move", cmd: "edit.move", tip: "Move the selection: base point, then destination" },
        { sep: true },
        { id: "knife", icon: "knife", cut: "knife", tip: "Knife: click a path across the object (Enter finishes), then click the part to keep. Cuts all the way through and closes the cut." },
        { id: "line", icon: "line", cut: "line", tip: "Straight cut: two clicks, then click the part to keep" },
        { id: "lasso", icon: "lasso", cut: "lasso", tip: "Lasso: drag a loop, then click inside (keep the inside) or outside (cut it out)" },
        { id: "drill", icon: "drill", cut: "drill", tip: "Drill: click to bore a round hole straight through (along the view)" },
        { id: "slice", icon: "slice", cut: "slice", tip: "Slice: click a height for a horizontal cut (flat bottom), then the part to keep" },
        { sep: true },
        { id: "polyline", icon: "polyline", cmd: "draw.polyline", draw: "polyline", tip: "Polyline sketch" },
        { id: "rect", icon: "rect", cmd: "draw.rect", draw: "rect", tip: "Rectangle sketch" },
        { id: "circle", icon: "circle", cmd: "draw.circle", draw: "circle", tip: "Circle sketch" }
    ]

    ColumnLayout {
        id: col
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 6
        spacing: 4
        Repeater {
            model: box.tools
            delegate: Item {
                id: cell
                required property var modelData
                Layout.preferredWidth: 38
                Layout.preferredHeight: modelData.sep ? 9 : 38
                Rectangle {  // separator
                    visible: !!cell.modelData.sep
                    anchors.centerIn: parent
                    width: 28; height: 1
                    color: Theme.gearEdge
                }
                Rectangle {
                    id: btn
                    visible: !cell.modelData.sep
                    objectName: cell.modelData.sep ? "" : "tool_" + cell.modelData.id
                    anchors.fill: parent
                    radius: Theme.radius
                    readonly property bool on: !cell.modelData.sep && box.isActive(cell.modelData)
                    color: on ? Theme.accent : ma.containsMouse ? Theme.gearFaceHot : "transparent"
                    border.color: on ? Theme.accent : ma.containsMouse ? Theme.gearEdge : "transparent"
                    Canvas {
                        id: icon
                        anchors.centerIn: parent
                        width: 26; height: 26
                        property color ink: btn.on ? Theme.accentText : Theme.gearText
                        onInkChanged: requestPaint()
                        onPaint: ToolIcons.paint(getContext("2d"), cell.modelData.icon || "", width, ink)
                    }
                    MouseArea {
                        id: ma
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: box.activate(cell.modelData)
                    }
                    ToolTip.visible: ma.containsMouse && !cell.modelData.sep
                    ToolTip.text: cell.modelData.tip || ""
                    ToolTip.delay: 300
                }
            }
        }
    }

    // drill diameter, next to the toolbox while the drill is active
    Rectangle {
        visible: box.view.tool === "cut" && box.view.cutKind === "drill"
        x: box.width + 6
        y: 6 + 7 * 42 - 4
        width: drow.implicitWidth + 16
        height: drow.implicitHeight + 12
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.border
        RowLayout {
            id: drow
            anchors.centerIn: parent
            spacing: 6
            ALabel { text: "Hole Ø" }
            AField {
                objectName: "drillDiameter"
                Layout.preferredWidth: 80
                text: App.formatLength(box.view.drillDiameter)
                onAccepted: {
                    var v = App.parseLength(text)
                    invalid = isNaN(v) || v <= 0
                    if (!invalid) box.view.drillDiameter = v
                }
            }
        }
    }
}
