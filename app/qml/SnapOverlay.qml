import QtQuick
import QtQuick.Shapes
import Anvil.Native

// Active snap: colored glyph + label (type and coordinates in the display unit).
Item {
    id: root
    property var view
    readonly property color c: Theme.snapColor(view.snapKind)
    readonly property bool show: view.snapVisible && !view.radialActive && view.snapKind !== "free"

    // rubber band of the move tool
    Shape {
        anchors.fill: parent
        visible: root.view.hasBasePoint
        ShapePath {
            strokeColor: Theme.accent
            strokeWidth: 1.5
            strokeStyle: ShapePath.DashLine
            fillColor: "transparent"
            startX: root.view.basePos.x; startY: root.view.basePos.y
            PathLine { x: root.view.snapPos.x; y: root.view.snapPos.y }
        }
    }
    Rectangle {
        visible: root.view.hasBasePoint
        x: root.view.basePos.x - 4; y: root.view.basePos.y - 4
        width: 8; height: 8; radius: 4
        color: Theme.accent
    }

    Item {
        visible: root.show
        x: root.view.snapPos.x
        y: root.view.snapPos.y
        // glyph per kind
        Rectangle {  // point: square
            visible: root.view.snapKind === "point"
            x: -6; y: -6; width: 12; height: 12
            color: "transparent"; border.color: root.c; border.width: 2
        }
        Shape {      // midpoint: triangle
            visible: root.view.snapKind === "mid"
            ShapePath {
                strokeColor: root.c; strokeWidth: 2; fillColor: "transparent"
                startX: 0; startY: -7
                PathLine { x: 7; y: 6 }
                PathLine { x: -7; y: 6 }
                PathLine { x: 0; y: -7 }
            }
        }
        Rectangle {  // center: circle
            visible: root.view.snapKind === "center"
            x: -7; y: -7; width: 14; height: 14; radius: 7
            color: "transparent"; border.color: root.c; border.width: 2
        }
        Shape {      // intersection / constraint / grid / axis: cross
            visible: ["cross", "constraint", "grid", "axis"].indexOf(root.view.snapKind) >= 0
            ShapePath {
                strokeColor: root.c; strokeWidth: 2; fillColor: "transparent"
                startX: -6; startY: -6
                PathLine { x: 6; y: 6 }
                PathMove { x: 6; y: -6 }
                PathLine { x: -6; y: 6 }
            }
        }
        Rectangle {
            x: 12; y: 10
            width: lbl.implicitWidth + 10
            height: lbl.implicitHeight + 4
            radius: 3
            color: Theme.panelAlt
            opacity: 0.92
            border.color: root.c
            Text {
                id: lbl
                anchors.centerIn: parent
                text: root.view.snapLabel + "   " + (App.unit, App.formatPoint(root.view.snapPoint))
                color: Theme.text
                font.family: Theme.font
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
