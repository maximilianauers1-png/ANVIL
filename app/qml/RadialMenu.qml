import QtQuick
import QtQuick.Shapes
import Anvil.Native

// Marking menu. Instantiated once at start-up; only `visible` toggles, so it
// appears in the first frame after the key/button press (gate G2). Selection
// is computed from the gesture in C++ (angle, dead zone), not from this drawing.
Item {
    id: root
    property var view
    visible: view.radialActive
    property var items: view.radialActive ? App.radialItems(view.radialPath, view.radialContext) : []
    readonly property real cx: view.radialCenter.x
    readonly property real cy: view.radialCenter.y
    readonly property int n: 8
    readonly property real r0: Theme.radialInner
    readonly property real r1: Theme.radialOuter

    // dead zone
    Rectangle {
        x: root.cx - root.r0 + 6
        y: root.cy - root.r0 + 6
        width: 2 * (root.r0 - 6)
        height: width
        radius: width / 2
        color: Theme.radialFill
        border.color: Theme.borderStrong
        Text {
            anchors.centerIn: parent
            text: root.view.radialPath.length > 0 ? "◂" : "×"
            color: Theme.textDim
            font.pixelSize: 14
        }
    }

    Repeater {
        model: root.n
        delegate: Item {
            id: sector
            required property int index
            readonly property var item: root.items.length > index ? root.items[index] : ({ label: "", enabled: false })
            readonly property bool hot: root.view.radialHighlight === index
            readonly property real mid: -index * 360 / root.n   // screen degrees, clockwise from east
            anchors.fill: parent
            Shape {
                anchors.fill: parent
                ShapePath {
                    strokeColor: Theme.border
                    strokeWidth: 1
                    fillColor: sector.hot && sector.item.enabled ? Theme.accent : Theme.radialFill
                    PathAngleArc {
                        centerX: root.cx; centerY: root.cy
                        radiusX: root.r1; radiusY: root.r1
                        startAngle: sector.mid - 180 / root.n + 1
                        sweepAngle: 360 / root.n - 2
                        moveToStart: true
                    }
                    PathAngleArc {
                        centerX: root.cx; centerY: root.cy
                        radiusX: root.r0; radiusY: root.r0
                        startAngle: sector.mid + 180 / root.n - 1
                        sweepAngle: -(360 / root.n - 2)
                    }
                    PathLine {
                        x: root.cx + root.r1 * Math.cos((sector.mid - 180 / root.n + 1) * Math.PI / 180)
                        y: root.cy + root.r1 * Math.sin((sector.mid - 180 / root.n + 1) * Math.PI / 180)
                    }
                }
            }
            Text {
                readonly property real rr: (root.r0 + root.r1) / 2 + 4
                x: root.cx + rr * Math.cos(sector.mid * Math.PI / 180) - width / 2
                y: root.cy + rr * Math.sin(sector.mid * Math.PI / 180) - height / 2
                width: 86
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: sector.item.label
                font.family: Theme.font
                font.pixelSize: Theme.fontSize
                font.bold: sector.hot
                color: !sector.item.enabled ? Theme.radialDisabled : sector.hot ? Theme.accentText : Theme.text
            }
        }
    }
    // help line for the highlighted command
    Text {
        readonly property var it: root.view.radialHighlight >= 0 && root.items.length > root.view.radialHighlight
                                  ? root.items[root.view.radialHighlight] : null
        visible: it !== null && (it.help || "") !== ""
        x: root.cx - width / 2
        y: root.cy + root.r1 + 10
        text: it ? (it.enabled ? (it.help || "") : (it.help || "") + "  (not applicable to the selection)") : ""
        color: Theme.textDim
        font.pixelSize: Theme.fontSmall
        font.family: Theme.font
        style: Text.Outline
        styleColor: Theme.bg
    }
}
