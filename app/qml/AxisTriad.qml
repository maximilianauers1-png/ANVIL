import QtQuick
import Anvil.Native

Canvas {
    id: c
    property var view
    width: 64
    height: 64
    renderStrategy: Canvas.Immediate
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const R = view.camRight, U = view.camUp, F = view.camForward
        const axes = [
            { d: [1, 0, 0], col: "#dc4a3d", l: "X" },
            { d: [0, 1, 0], col: "#4cb84c", l: "Y" },
            { d: [0, 0, 1], col: "#4a8fe0", l: "Z" }
        ]
        // draw back-to-front
        axes.sort((a, b) => (b.d[0] * F.x + b.d[1] * F.y + b.d[2] * F.z) - (a.d[0] * F.x + a.d[1] * F.y + a.d[2] * F.z))
        for (const a of axes) {
            const x = 32 + 24 * (a.d[0] * R.x + a.d[1] * R.y + a.d[2] * R.z)
            const y = 32 - 24 * (a.d[0] * U.x + a.d[1] * U.y + a.d[2] * U.z)
            ctx.strokeStyle = a.col
            ctx.lineWidth = 2
            ctx.beginPath(); ctx.moveTo(32, 32); ctx.lineTo(x, y); ctx.stroke()
            ctx.fillStyle = a.col
            ctx.font = "bold 11px sans-serif"
            ctx.fillText(a.l, x + 2, y - 2)
        }
    }
    Connections {
        target: c.view
        function onCameraChanged() { c.requestPaint() }
    }
}
