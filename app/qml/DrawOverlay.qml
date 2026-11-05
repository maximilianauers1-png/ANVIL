import QtQuick
import Anvil.Native

// Drawing tools: placed points, rubber band to the cursor, inference guide and
// the live segment length. Typing a number places the next point at that
// distance along the cursor direction (Enter), so exact shapes need no dialog.
Item {
    id: root
    property var view
    readonly property bool active: view.drawKind !== ""
    visible: active

    function sketchPath() {
        // polygon (in item coordinates) of what would be created right now
        var pts = view.drawPointsSketch
        var c = view.drawCursorSketch
        var kind = view.drawKind
        var out = []
        if (kind === "polyline") {
            for (var i = 0; i < pts.length; ++i) out.push(view.drawToItem(pts[i].x, pts[i].y))
            if (pts.length > 0 && view.drawPlane !== "") out.push(view.drawToItem(c.x, c.y))
        } else if (kind === "rect" && pts.length === 1) {
            var a = pts[0]
            out = [view.drawToItem(a.x, a.y), view.drawToItem(c.x, a.y), view.drawToItem(c.x, c.y),
                   view.drawToItem(a.x, c.y), view.drawToItem(a.x, a.y)]
        } else if (kind === "circle" && pts.length === 1) {
            var r = Math.hypot(c.x - pts[0].x, c.y - pts[0].y)
            var n = 72
            for (var k = 0; k <= n; ++k) {
                var t = 2 * Math.PI * k / n
                out.push(view.drawToItem(pts[0].x + r * Math.cos(t), pts[0].y + r * Math.sin(t)))
            }
            out.push(view.drawToItem(pts[0].x, pts[0].y))  // radius line
            out.push(view.drawToItem(c.x, c.y))
        }
        return out
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (!root.active) return
            var p = root.sketchPath()
            if (p.length > 1) {
                ctx.lineWidth = 2
                ctx.strokeStyle = Theme.accent
                ctx.beginPath()
                ctx.moveTo(p[0].x, p[0].y)
                for (var i = 1; i < p.length; ++i) ctx.lineTo(p[i].x, p[i].y)
                ctx.stroke()
            }
            // placed points
            ctx.fillStyle = Theme.accent
            var pts = root.view.drawPoints
            for (var j = 0; j < pts.length; ++j) {
                ctx.beginPath()
                ctx.arc(pts[j].x, pts[j].y, j === 0 ? 5 : 3.5, 0, 2 * Math.PI)
                ctx.fill()
            }
        }
        Connections {
            target: root.view
            function onDrawChanged() { canvas.requestPaint() }
            function onCameraChanged() { canvas.requestPaint() }
        }
    }

    // live label: segment length / radius and the active guide
    Rectangle {
        id: label
        visible: root.active && root.view.drawPoints.length > 0 && !lengthBox.visible
        x: root.view.drawCursor.x + 16
        y: root.view.drawCursor.y + 16
        radius: Theme.radius
        color: Theme.panelAlt
        border.color: Theme.border
        width: labelText.implicitWidth + 14
        height: labelText.implicitHeight + 8
        ALabel {
            id: labelText
            anchors.centerIn: parent
            text: {
                var s = (root.view.drawKind === "circle" ? "r " : "") + App.formatLength(root.view.drawSegment)
                if (root.view.drawKind === "rect" && root.view.drawPointsSketch.length === 1) {
                    var a = root.view.drawPointsSketch[0], c = root.view.drawCursorSketch
                    s = App.formatLength(Math.abs(c.x - a.x)) + " × " + App.formatLength(Math.abs(c.y - a.y))
                }
                if (root.view.drawGuide !== "") s += "   " + root.view.drawGuide
                return s + "   (type a length)"
            }
        }
    }

    // typed length: appears with the first digit
    Rectangle {
        id: lengthBox
        visible: lengthField.text !== ""
        x: root.view.drawCursor.x + 16
        y: root.view.drawCursor.y + 16
        width: 150
        height: 30
        radius: Theme.radius
        color: Theme.panelAlt
        border.color: Theme.accent
        AField {
            id: lengthField
            anchors.fill: parent
            anchors.margins: 2
            invalid: text !== "" && isNaN(App.parseLength(text))
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                    var mm = App.parseLength(text)
                    if (!isNaN(mm) && mm > 0) root.view.drawTypedLength(mm)
                    text = ""
                    root.view.forceActiveFocus()
                    e.accepted = true
                } else if (e.key === Qt.Key_Escape) {
                    text = ""
                    root.view.forceActiveFocus()
                    e.accepted = true
                }
            }
        }
    }

    /// Called by the viewport's key handler. Returns true if the key was used.
    function handleKey(e) {
        if (!active) return false
        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
            view.finishDraw()
            return true
        }
        if (e.key === Qt.Key_Backspace) {
            view.drawUndoPoint()
            return true
        }
        if (view.drawPoints.length > 0 && e.text.length === 1 && "0123456789.,".indexOf(e.text) >= 0) {
            lengthField.text = e.text
            lengthField.forceActiveFocus()
            lengthField.cursorPosition = 1
            return true
        }
        return false
    }
}
