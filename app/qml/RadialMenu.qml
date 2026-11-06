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

    // gear body: opaque steel ring with teeth around the rim (independent of
    // the items, built once). Always dark so the light labels read over any
    // viewport colour.
    readonly property int teeth: 24
    readonly property real toothH: 11
    function gearOutline() {
        var pts = [], N = root.teeth, ri = root.r1 + 2, ro = root.r1 + root.toothH
        for (var k = 0; k < N; ++k) {
            var a = 2 * Math.PI * k / N, w = Math.PI / N
            // tooth: flank up, flat top, flank down (trapezoid, 45 % of the pitch on top)
            var a0 = a - w, a1 = a - 0.45 * w, a2 = a + 0.45 * w, a3 = a + w
            pts.push(Qt.point(root.cx + ri * Math.cos(a0), root.cy + ri * Math.sin(a0)))
            pts.push(Qt.point(root.cx + ro * Math.cos(a1), root.cy + ro * Math.sin(a1)))
            pts.push(Qt.point(root.cx + ro * Math.cos(a2), root.cy + ro * Math.sin(a2)))
            pts.push(Qt.point(root.cx + ri * Math.cos(a3), root.cy + ri * Math.sin(a3)))
        }
        pts.push(pts[0])
        return pts
    }
    Shape {
        anchors.fill: parent
        // soft shadow under the gear
        ShapePath {
            strokeWidth: -1
            fillColor: Qt.rgba(0, 0, 0, 0.35)
            PathAngleArc { centerX: root.cx + 3; centerY: root.cy + 5; radiusX: root.r1 + root.toothH + 4; radiusY: root.r1 + root.toothH + 4; startAngle: 0; sweepAngle: 360; moveToStart: true }
        }
        ShapePath {
            strokeColor: Theme.gearEdge
            strokeWidth: 1.5
            fillGradient: RadialGradient {
                centerX: root.cx; centerY: root.cy; centerRadius: root.r1 + root.toothH
                focalX: root.cx - 30; focalY: root.cy - 40
                GradientStop { position: 0.0; color: Theme.gearLight }
                GradientStop { position: 1.0; color: Theme.gearDark }
            }
            PathPolyline { path: root.visible ? root.gearOutline() : [] }
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
                    strokeColor: Theme.gearGroove
                    strokeWidth: 2
                    fillColor: sector.hot && sector.item.enabled ? Theme.accent : sector.hot ? Theme.gearFaceHot : Theme.gearFace
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
                font.bold: true
                color: !sector.item.enabled ? Theme.gearTextDisabled : sector.hot ? Theme.accentText : Theme.gearText
                style: sector.hot && sector.item.enabled ? Text.Normal : Text.Outline
                styleColor: Theme.gearDark
            }
        }
    }
    // hub: the dead zone (release here = cancel; ◂ = back out of a submenu)
    Rectangle {
        x: root.cx - root.r0 + 4
        y: root.cy - root.r0 + 4
        width: 2 * (root.r0 - 4)
        height: width
        radius: width / 2
        color: Theme.gearLight
        border.color: Theme.gearEdge
        border.width: 2
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.62; height: width; radius: width / 2
            color: Theme.gearDark
            border.color: Theme.gearGroove
            border.width: 2
        }
        Text {
            anchors.centerIn: parent
            text: root.view.radialPath.length > 0 ? "◂" : "×"
            color: Theme.gearText
            font.pixelSize: 15
            font.bold: true
        }
    }
    // help line for the highlighted command
    Text {
        readonly property var it: root.view.radialHighlight >= 0 && root.items.length > root.view.radialHighlight
                                  ? root.items[root.view.radialHighlight] : null
        visible: it !== null && (it.help || "") !== ""
        x: root.cx - width / 2
        text: it ? (it.enabled ? (it.help || "") : (it.help || "") + "  (not applicable to the selection)") : ""
        color: Theme.gearText
        font.pixelSize: Theme.fontSmall + 1
        font.family: Theme.font
        y: root.cy + root.r1 + root.toothH + 12
        style: Text.Outline
        styleColor: "#000000"
    }
}
