pragma Singleton
import QtQuick

// Tool icons drawn with Canvas 2D (no image files, crisp at any scale).
QtObject {
    function paint(ctx, name, s, ink) {
        ctx.reset()
        ctx.strokeStyle = ink
        ctx.fillStyle = ink
        ctx.lineWidth = 2
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        var k = s / 24
        ctx.scale(k, k)
        ctx.beginPath()
        switch (name) {
        case "select":  // arrow cursor
            ctx.moveTo(6, 3); ctx.lineTo(6, 19); ctx.lineTo(10, 15); ctx.lineTo(13, 21); ctx.lineTo(15.5, 20)
            ctx.lineTo(12.5, 14); ctx.lineTo(18, 14); ctx.closePath(); ctx.fill()
            break
        case "move":  // four-way arrows
            ctx.moveTo(12, 3); ctx.lineTo(12, 21); ctx.moveTo(3, 12); ctx.lineTo(21, 12)
            ctx.moveTo(9, 6); ctx.lineTo(12, 3); ctx.lineTo(15, 6)
            ctx.moveTo(9, 18); ctx.lineTo(12, 21); ctx.lineTo(15, 18)
            ctx.moveTo(6, 9); ctx.lineTo(3, 12); ctx.lineTo(6, 15)
            ctx.moveTo(18, 9); ctx.lineTo(21, 12); ctx.lineTo(18, 15)
            ctx.stroke()
            break
        case "knife":  // blade with handle, dashed cut path below
            ctx.moveTo(4, 14); ctx.lineTo(15, 3); ctx.lineTo(17, 5); ctx.lineTo(8, 15); ctx.closePath(); ctx.fill()
            ctx.beginPath(); ctx.lineWidth = 3
            ctx.moveTo(16, 4); ctx.lineTo(20, 0.5); ctx.stroke()
            ctx.beginPath(); ctx.lineWidth = 1.6
            ctx.setLineDash([2.5, 2]); ctx.moveTo(3, 21); ctx.bezierCurveTo(8, 16, 14, 23, 21, 18); ctx.stroke()
            ctx.setLineDash([])
            break
        case "line":  // straight cut through a block
            ctx.lineWidth = 1.6
            ctx.strokeRect(4, 7, 16, 12)
            ctx.beginPath(); ctx.lineWidth = 2.4
            ctx.moveTo(2, 21); ctx.lineTo(22, 4); ctx.stroke()
            break
        case "lasso":  // dashed loop with a tail
            ctx.lineWidth = 1.8
            ctx.setLineDash([3, 2])
            ctx.ellipse(3, 3, 18, 12); ctx.stroke()
            ctx.setLineDash([])
            ctx.beginPath(); ctx.moveTo(8, 14); ctx.bezierCurveTo(6, 18, 10, 19, 9, 22); ctx.stroke()
            break
        case "drill":  // twist drill bit over a hole
            ctx.lineWidth = 1.6
            ctx.ellipse(5, 17, 14, 5); ctx.stroke()
            ctx.beginPath(); ctx.lineWidth = 2
            ctx.moveTo(12, 2); ctx.lineTo(12, 16)
            ctx.moveTo(9.5, 5); ctx.lineTo(14.5, 7.5); ctx.moveTo(9.5, 9); ctx.lineTo(14.5, 11.5); ctx.moveTo(9.5, 13); ctx.lineTo(14, 15)
            ctx.stroke()
            break
        case "slice":  // block with a horizontal cut plane
            ctx.lineWidth = 1.6
            ctx.strokeRect(6, 4, 12, 16)
            ctx.beginPath(); ctx.lineWidth = 2.4
            ctx.moveTo(2, 14); ctx.lineTo(22, 14); ctx.stroke()
            ctx.beginPath(); ctx.lineWidth = 1.4
            ctx.moveTo(19, 11); ctx.lineTo(22, 14); ctx.lineTo(19, 17); ctx.stroke()
            break
        case "polyline":
            ctx.moveTo(3, 19); ctx.lineTo(9, 7); ctx.lineTo(15, 15); ctx.lineTo(21, 5); ctx.stroke()
            ctx.beginPath()
            ctx.arc(3, 19, 1.8, 0, 2 * Math.PI); ctx.arc(9, 7, 1.8, 0, 2 * Math.PI)
            ctx.fill()
            ctx.beginPath(); ctx.arc(15, 15, 1.8, 0, 2 * Math.PI); ctx.arc(21, 5, 1.8, 0, 2 * Math.PI); ctx.fill()
            break
        case "rect":
            ctx.strokeRect(4, 6, 16, 12)
            break
        case "circle":
            ctx.arc(12, 12, 8, 0, 2 * Math.PI); ctx.stroke()
            ctx.beginPath(); ctx.arc(12, 12, 1.6, 0, 2 * Math.PI); ctx.fill()
            break
        }
    }
}
