import QtQuick
import Anvil.Native

// ViewCube with 26 clickable directions (6 faces, 12 edges, 8 corners).
// Drawn as an orthographic projection of [-1,1]^3 with scale k = 0.55
// (same constant as ViewportItem::cubeDirection).
Item {
    id: root
    property var view
    width: 128
    height: 128
    readonly property real k: 0.55
    readonly property real half: width / 2
    property vector3d hoverDir: Qt.vector3d(0, 0, 0)

    function proj(p) {
        const R = view.camRight, U = view.camUp
        return Qt.point(half + half * k * (p.x * R.x + p.y * R.y + p.z * R.z),
                        half - half * k * (p.x * U.x + p.y * U.y + p.z * U.z))
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Immediate
        property var faces: [
            { n: [0, 0, 1], label: "TOP" }, { n: [0, 0, -1], label: "BOTTOM" },
            { n: [0, -1, 0], label: "FRONT" }, { n: [0, 1, 0], label: "BACK" },
            { n: [1, 0, 0], label: "RIGHT" }, { n: [-1, 0, 0], label: "LEFT" }
        ]
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const F = root.view.camForward
            // painter's order: back faces culled; draw visible faces
            for (const f of faces) {
                const d = f.n[0] * F.x + f.n[1] * F.y + f.n[2] * F.z
                if (d > -1e-3) continue  // facing away
                // face basis
                const ua = f.n[0] !== 0 ? [0, 1, 0] : (f.n[1] !== 0 ? [1, 0, 0] : [1, 0, 0])
                const vb = f.n[2] !== 0 ? [0, 1, 0] : [0, 0, 1]
                const P = (s, t) => root.proj(Qt.vector3d(f.n[0] + ua[0] * s + vb[0] * t,
                                                          f.n[1] + ua[1] * s + vb[1] * t,
                                                          f.n[2] + ua[2] * s + vb[2] * t))
                const hot = root.hoverDir.x === f.n[0] && root.hoverDir.y === f.n[1] && root.hoverDir.z === f.n[2]
                ctx.beginPath()
                let p = P(-1, -1); ctx.moveTo(p.x, p.y)
                p = P(1, -1); ctx.lineTo(p.x, p.y)
                p = P(1, 1); ctx.lineTo(p.x, p.y)
                p = P(-1, 1); ctx.lineTo(p.x, p.y)
                ctx.closePath()
                ctx.fillStyle = hot ? Theme.accent : Theme.panelAlt
                ctx.globalAlpha = 0.92
                ctx.fill()
                ctx.globalAlpha = 1
                ctx.strokeStyle = Theme.borderStrong
                ctx.lineWidth = 1
                ctx.stroke()
                // 3x3 regions (edges/corners are clickable directions)
                ctx.strokeStyle = Theme.border
                for (const s of [-0.6, 0.6]) {
                    let q0 = P(s, -1), q1 = P(s, 1)
                    ctx.beginPath(); ctx.moveTo(q0.x, q0.y); ctx.lineTo(q1.x, q1.y); ctx.stroke()
                    q0 = P(-1, s); q1 = P(1, s)
                    ctx.beginPath(); ctx.moveTo(q0.x, q0.y); ctx.lineTo(q1.x, q1.y); ctx.stroke()
                }
                if (-d > 0.35) {
                    const c = P(0, 0)
                    ctx.fillStyle = hot ? Theme.accentText : Theme.textDim
                    ctx.font = "bold 10px sans-serif"
                    ctx.textAlign = "center"
                    ctx.textBaseline = "middle"
                    ctx.fillText(f.label, c.x, c.y)
                }
            }
        }
        Connections {
            target: root.view
            function onCameraChanged() { canvas.requestPaint() }
        }
        Connections {
            target: Theme
            function onDarkChanged() { canvas.requestPaint() }
        }
    }
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        function dirAt(m) {
            return root.view.cubeDirection((m.x - root.half) / root.half, -(m.y - root.half) / root.half)
        }
        onPositionChanged: (m) => { root.hoverDir = dirAt(m); canvas.requestPaint() }
        onExited: { root.hoverDir = Qt.vector3d(0, 0, 0); canvas.requestPaint() }
        onClicked: (m) => {
            const d = dirAt(m)
            if (d.x !== 0 || d.y !== 0 || d.z !== 0) App.requestViewDirection(d)
        }
    }
    Row {
        anchors.top: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 4
        AButton { text: "Iso"; implicitHeight: 22; onClicked: App.requestView("iso") }
        AButton { text: "Fit"; implicitHeight: 22; onClicked: App.requestView("frame") }
        AButton {
            text: root.view.ortho ? "Ortho" : "Persp"
            implicitHeight: 22
            onClicked: App.requestView(root.view.ortho ? "perspective" : "ortho")
        }
    }
}
