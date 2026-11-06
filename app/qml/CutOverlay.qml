import QtQuick
import Anvil.Native

// Cut tools: the drawn path and, once it is finished, which part stays: the
// side under the pointer is shown as kept (accent), the other as removed (red).
Item {
    id: root
    property var view
    readonly property bool active: view.cutKind !== ""
    visible: active

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        function polyPath(ctx, poly) {
            ctx.moveTo(poly[0].x, poly[0].y)
            for (var i = 1; i < poly.length; ++i) ctx.lineTo(poly[i].x, poly[i].y)
            ctx.closePath()
        }
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (!root.active) return
            var v = root.view
            var pts = v.cutPoints
            if (v.cutChoosing) {
                var reg = v.cutRegion
                var keepRegion = v.cutHover === 1
                // removed side first (whole view), then the region on top
                ctx.fillStyle = keepRegion ? Qt.rgba(1, 0.35, 0.3, 0.16) : Qt.rgba(1, 0.6, 0.18, 0.16)
                ctx.fillRect(0, 0, width, height)
                ctx.globalCompositeOperation = "destination-out"
                ctx.fillStyle = "black"
                ctx.beginPath()
                for (var r = 0; r < reg.length; ++r) polyPath(ctx, reg[r])
                ctx.fill()
                ctx.globalCompositeOperation = "source-over"
                ctx.fillStyle = keepRegion ? Qt.rgba(1, 0.6, 0.18, 0.16) : Qt.rgba(1, 0.35, 0.3, 0.16)
                ctx.beginPath()
                for (var r2 = 0; r2 < reg.length; ++r2) polyPath(ctx, reg[r2])
                ctx.fill()
                // the cut line(s): region outlines
                ctx.lineWidth = 2
                ctx.strokeStyle = Theme.accent
                ctx.beginPath()
                for (var r3 = 0; r3 < reg.length; ++r3) polyPath(ctx, reg[r3])
                ctx.stroke()
            } else if (pts.length > 0) {
                ctx.lineWidth = 2
                ctx.strokeStyle = Theme.accent
                ctx.setLineDash([6, 4])
                ctx.beginPath()
                ctx.moveTo(pts[0].x, pts[0].y)
                for (var i = 1; i < pts.length; ++i) ctx.lineTo(pts[i].x, pts[i].y)
                if (v.cutKind === "lasso") ctx.closePath()
                else ctx.lineTo(v.cutCursor.x, v.cutCursor.y)  // rubber band
                ctx.stroke()
                ctx.setLineDash([])
                ctx.fillStyle = Theme.accent
                if (v.cutKind !== "lasso")
                    for (var j = 0; j < pts.length; ++j) {
                        ctx.beginPath()
                        ctx.arc(pts[j].x, pts[j].y, j === 0 ? 5 : 3.5, 0, 2 * Math.PI)
                        ctx.fill()
                    }
            }
            if (v.cutKind === "drill" && !v.cutChoosing && v.cutCursor.x > -1e5) {
                ctx.strokeStyle = Theme.accent
                ctx.lineWidth = 2
                ctx.beginPath()
                ctx.arc(v.cutCursor.x, v.cutCursor.y, 9, 0, 2 * Math.PI)
                ctx.moveTo(v.cutCursor.x - 14, v.cutCursor.y); ctx.lineTo(v.cutCursor.x + 14, v.cutCursor.y)
                ctx.moveTo(v.cutCursor.x, v.cutCursor.y - 14); ctx.lineTo(v.cutCursor.x, v.cutCursor.y + 14)
                ctx.stroke()
            }
        }
        Connections {
            target: root.view
            function onCutChanged() { canvas.requestPaint() }
            function onCameraChanged() { canvas.requestPaint() }
        }
    }
    // keep / remove label at the pointer while choosing
    Rectangle {
        visible: root.view.cutChoosing && root.view.cutHover >= 0
        x: root.view.cutCursor.x + 18
        y: root.view.cutCursor.y + 14
        width: lbl.implicitWidth + 14
        height: lbl.implicitHeight + 8
        radius: Theme.radius
        color: Theme.gearDark
        border.color: Theme.accent
        Text {
            id: lbl
            anchors.centerIn: parent
            text: "keep this part  ·  Shift: keep both"
            color: Theme.gearText
            font.family: Theme.font
            font.pixelSize: Theme.fontSmall + 1
            font.bold: true
        }
    }
}
