import QtQuick
import QtQuick.Layouts
import Anvil.Native

// Sketch editing on top of the viewport: drag points (the solver keeps all
// constraints), click a dimension to change it, select points/curves and apply
// constraints from the toolbar. Every action is one Python command.
Item {
    id: root
    property var view
    property string sketch: ""          // sketch being edited ("" = closed)
    property var info: ({})
    property var picked: []             // selected point / curve ids
    property int camTick: 0
    readonly property bool active: sketch !== ""
    visible: active

    function open(name) {
        sketch = name
        picked = []
        refresh()
    }
    function close() { sketch = ""; picked = [] }
    function refresh() {
        if (!active) return
        info = App.sketchInfo(sketch)
        if (!info.name) close()
    }
    function toggle(id) {
        var p = picked.slice()
        var i = p.indexOf(id)
        if (i >= 0) p.splice(i, 1); else p.push(id)
        picked = p
    }
    function kindOf(id) { return id.length > 0 ? id[0] : "" }  // P point, L line, C circle, A arc
    function pickedOf(k) { return picked.filter(function (x) { return kindOf(x) === k }) }
    function pos(w) { camTick; return view.projectWorld(w) }
    // Label position (screen) for a curve: lines get it outside the sketch,
    // offset along the line normal that points away from the centroid of all
    // points; circles at their 45 degree point. `gap` in px.
    function labelPos(c, gap, w, h) {
        var a = pos(c.anchor)
        if (c.kind !== "line") return Qt.point(a.x + gap * 0.7, a.y - gap * 0.7)
        var pa = null, pb = null, sx = 0, sy = 0, n = 0
        for (var i = 0; i < info.points.length; ++i) {
            var q = pos(info.points[i].world)
            sx += q.x; sy += q.y; n++
            if (info.points[i].id === c.points[0]) pa = q
            if (info.points[i].id === c.points[1]) pb = q
        }
        if (!pa || !pb) return a
        var dx = pb.x - pa.x, dy = pb.y - pa.y, l = Math.hypot(dx, dy)
        if (l < 1e-6) return a
        var nx = -dy / l, ny = dx / l
        if ((a.x - sx / n) * nx + (a.y - sy / n) * ny < 0) nx = -nx, ny = -ny
        // support distance of the label box along the normal keeps it off the line and its handles
        var d = gap + Math.abs(nx) * (w || 0) / 2 + Math.abs(ny) * (h || 0) / 2
        return Qt.point(a.x + nx * d, a.y + ny * d)
    }
    function stateColor(s) { return s === "fixed" ? Theme.ok : s === "conflict" ? Theme.danger : Theme.accent }

    Connections {
        target: App
        function onDocumentChanged() { root.refresh() }
    }
    Connections {
        target: root.view
        function onCameraChanged() { root.camTick++ }
    }

    // ---- curve handles (select lines / circles)
    Repeater {
        model: root.active && root.info.curves ? root.info.curves : []
        delegate: Rectangle {
            required property var modelData
            readonly property point p: root.pos(modelData.anchor)
            readonly property bool sel: root.picked.indexOf(modelData.id) >= 0
            x: p.x - 6; y: p.y - 6; width: 12; height: 12
            rotation: 45
            color: sel ? Theme.accent : Theme.panelAlt
            border.color: Theme.accent
            border.width: 1.5
            MouseArea {
                anchors.fill: parent
                anchors.margins: -4
                cursorShape: Qt.PointingHandCursor
                onClicked: root.toggle(parent.modelData.id)
            }
        }
    }

    // ---- point handles (drag; click to select)
    Repeater {
        model: root.active && root.info.points ? root.info.points : []
        delegate: Rectangle {
            id: handle
            required property var modelData
            readonly property point p: root.pos(modelData.world)
            readonly property bool sel: root.picked.indexOf(modelData.id) >= 0
            property bool dragging: false
            property point dragPos
            x: (dragging ? dragPos.x : p.x) - 6
            y: (dragging ? dragPos.y : p.y) - 6
            width: 12; height: 12; radius: 6
            color: sel ? Theme.accent : Theme.panel
            border.color: root.stateColor(modelData.state)
            border.width: 2.5
            MouseArea {
                anchors.fill: parent
                anchors.margins: -5
                cursorShape: Qt.SizeAllCursor
                property point start
                onPressed: (m) => { start = Qt.point(m.x, m.y) }
                onPositionChanged: (m) => {
                    var g = mapToItem(root, m.x, m.y)
                    if (!handle.dragging && Math.hypot(m.x - start.x, m.y - start.y) > 3) handle.dragging = true
                    if (handle.dragging) handle.dragPos = g
                }
                onReleased: (m) => {
                    if (!handle.dragging) { root.toggle(handle.modelData.id); return }
                    handle.dragging = false
                    var g = mapToItem(root, m.x, m.y)
                    var q = root.view.planePoint(g.x, g.y, root.info.plane, root.info.offset)
                    if (q !== undefined && q !== null) App.sketchDrag(root.sketch, handle.modelData.id, q.x, q.y)
                }
            }
        }
    }

    // ---- measured sizes of undimensioned lines/circles: click, type, Enter adds the dimension
    function dimensioned(id) {
        var cs = info.constraints || []
        for (var i = 0; i < cs.length; ++i)
            if (["length", "radius", "diameter"].indexOf(cs[i].kind) >= 0 && cs[i].refs[0] === id) return true
        return false
    }
    Repeater {
        model: root.active && root.info.curves
               ? root.info.curves.filter(function (c) { return c.kind !== "arc" && !c.construction && !root.dimensioned(c.id) }) : []
        delegate: Rectangle {
            id: measBox
            required property var modelData
            readonly property point p: root.labelPos(modelData, 8, width, height)
            readonly property string measured: modelData.kind === "line" ? App.formatLength(modelData.length)
                                                                          : "Ø " + App.formatLength(2 * modelData.radius)
            property bool editing: false
            x: p.x - width / 2; y: p.y - height / 2
            width: editing ? 110 : measText.implicitWidth + 12
            height: 22
            radius: Theme.radius
            color: editing ? Theme.panelAlt : "transparent"
            border.color: editing ? Theme.accent : Theme.border
            border.width: editing ? 1 : 0
            ALabel {
                id: measText
                visible: !measBox.editing
                anchors.centerIn: parent
                text: measBox.measured
                color: Theme.sceneText
            }
            MouseArea {
                objectName: "measLabel_" + measBox.modelData.id
                anchors.fill: parent
                visible: !measBox.editing
                cursorShape: Qt.IBeamCursor
                hoverEnabled: true
                onEntered: measText.color = Theme.accent
                onExited: measText.color = Theme.sceneText
                onClicked: { measBox.editing = true; measField.text = ""; measField.forceActiveFocus() }
            }
            AField {
                id: measField
                objectName: "measure_" + measBox.modelData.id
                visible: measBox.editing
                anchors.fill: parent
                placeholderText: measBox.measured
                onAccepted: {
                    if (text !== "")
                        App.sketchDimension(root.sketch, measBox.modelData.kind === "line" ? "length" : "diameter",
                                            [measBox.modelData.id], text)
                    measBox.editing = false
                    root.view.forceActiveFocus()
                }
                onActiveFocusChanged: if (!activeFocus) measBox.editing = false
                Keys.onEscapePressed: { measBox.editing = false; root.view.forceActiveFocus() }
            }
        }
    }

    // ---- dimension labels (click to edit)
    Repeater {
        model: root.active && root.info.constraints ? root.info.constraints.filter(function (c) { return c.hasValue }) : []
        delegate: Rectangle {
            id: dimBox
            required property var modelData
            readonly property point p: {
                // anchor: first referenced curve, else the midpoint of the referenced points
                var refs = modelData.refs
                for (var i = 0; i < root.info.curves.length; ++i)
                    if (root.info.curves[i].id === refs[0]) return root.labelPos(root.info.curves[i], 8, dimBox.width, dimBox.height)
                var sx = 0, sy = 0, n = 0
                for (var j = 0; j < root.info.points.length; ++j)
                    if (refs.indexOf(root.info.points[j].id) >= 0) {
                        var q = root.pos(root.info.points[j].world); sx += q.x; sy += q.y; n++
                    }
                return n > 0 ? Qt.point(sx / n, sy / n) : Qt.point(-1000, -1000)
            }
            x: p.x - width / 2; y: p.y - height / 2
            width: editing ? 110 : dimText.implicitWidth + 12
            height: 22
            radius: Theme.radius
            property bool editing: false
            color: Theme.panelAlt
            border.color: modelData.state === "conflict" ? Theme.danger : modelData.state === "redundant" ? Theme.warn : Theme.border
            ALabel {
                id: dimText
                visible: !dimBox.editing
                anchors.centerIn: parent
                text: dimBox.modelData.kind === "radius" ? "R " + dimBox.modelData.text
                      : dimBox.modelData.kind === "diameter" ? "Ø " + dimBox.modelData.text : dimBox.modelData.text
            }
            MouseArea {
                anchors.fill: parent
                visible: !dimBox.editing
                cursorShape: Qt.IBeamCursor
                onClicked: { dimBox.editing = true; dimField.text = dimBox.modelData.text; dimField.selectAll(); dimField.forceActiveFocus() }
            }
            AField {
                id: dimField
                visible: dimBox.editing
                anchors.fill: parent
                onAccepted: { App.sketchSetDim(root.sketch, dimBox.modelData.id, text); dimBox.editing = false; root.view.forceActiveFocus() }
                onActiveFocusChanged: if (!activeFocus) dimBox.editing = false
                Keys.onEscapePressed: { dimBox.editing = false; root.view.forceActiveFocus() }
            }
        }
    }

    // ---- toolbar
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.unit * 2
        radius: Theme.radius
        color: Theme.panel
        border.color: Theme.accent
        width: bar.implicitWidth + 16
        height: bar.implicitHeight + 12
        ColumnLayout {
            id: bar
            anchors.centerIn: parent
            spacing: 6
            RowLayout {
                spacing: 6
                ALabel { text: "Edit " + root.sketch; font.bold: true }
                ALabel {
                    text: root.info.dof === undefined ? "" : root.info.dof === 0 ? "fully defined" : root.info.dof + " DOF free"
                    color: root.info.dof === 0 ? Theme.ok : Theme.textDim
                }
                ALabel { text: root.info.converged === false ? root.info.message : ""; color: Theme.danger }
            }
            RowLayout {
                spacing: 4
                readonly property var pts: root.pickedOf("P")
                readonly property var lines: root.pickedOf("L")
                readonly property var circles: root.pickedOf("C").concat(root.pickedOf("A"))
                AButton { text: "Horizontal"; enabled: parent.lines.length >= 1 || parent.pts.length === 2
                    onClicked: { (parent.lines.length ? parent.lines : [null]).forEach(function (l) {
                        App.sketchConstrain(root.sketch, "horizontal", l ? [l] : root.pickedOf("P")) }); root.picked = [] } }
                AButton { text: "Vertical"; enabled: parent.lines.length >= 1 || parent.pts.length === 2
                    onClicked: { (parent.lines.length ? parent.lines : [null]).forEach(function (l) {
                        App.sketchConstrain(root.sketch, "vertical", l ? [l] : root.pickedOf("P")) }); root.picked = [] } }
                AButton { text: "Parallel"; enabled: parent.lines.length === 2
                    onClicked: { App.sketchConstrain(root.sketch, "parallel", parent.lines); root.picked = [] } }
                AButton { text: "Perpendicular"; enabled: parent.lines.length === 2
                    onClicked: { App.sketchConstrain(root.sketch, "perpendicular", parent.lines); root.picked = [] } }
                AButton { text: "Equal"; enabled: parent.lines.length === 2 || parent.circles.length === 2
                    onClicked: { App.sketchConstrain(root.sketch, "equal", parent.lines.length === 2 ? parent.lines : parent.circles); root.picked = [] } }
                AButton { text: "Coincident"; enabled: parent.pts.length === 2
                    onClicked: { App.sketchConstrain(root.sketch, "coincident", parent.pts); root.picked = [] } }
                AButton { text: "Fix"; enabled: parent.pts.length >= 1
                    onClicked: { parent.pts.forEach(function (p) { App.sketchConstrain(root.sketch, "fix", [p]) }); root.picked = [] } }
                AButton {
                    text: "Dimension"
                    enabled: (parent.lines.length === 1 && parent.pts.length === 0) || parent.pts.length === 2 ||
                             (parent.circles.length === 1 && parent.lines.length === 0) || parent.lines.length === 2
                    onClicked: dimRow.visible = true
                }
                AButton { text: "Delete"; enabled: root.picked.length > 0 && root.pickedOf("P").length === 0
                    onClicked: { App.sketchDelete(root.sketch, root.picked); root.picked = [] } }
                AButton { objectName: "sketchDone"; text: "Done"; onClicked: root.close() }
            }
            RowLayout {
                id: dimRow
                visible: false
                spacing: 4
                ALabel { text: "Value:" }
                AField {
                    id: newDim
                    Layout.preferredWidth: 120
                    placeholderText: "e.g. 40 or 2*w"
                    onVisibleChanged: if (visible) { text = ""; forceActiveFocus() }
                    onAccepted: {
                        var L = root.pickedOf("L"), P = root.pickedOf("P"), C = root.pickedOf("C").concat(root.pickedOf("A"))
                        if (L.length === 2) App.sketchDimension(root.sketch, "angle", L, text)
                        else if (L.length === 1) App.sketchDimension(root.sketch, "length", L, text)
                        else if (P.length === 2) App.sketchDimension(root.sketch, "distance", P, text)
                        else if (C.length === 1) App.sketchDimension(root.sketch, "diameter", C, text)
                        dimRow.visible = false
                        root.picked = []
                        root.view.forceActiveFocus()
                    }
                    Keys.onEscapePressed: { dimRow.visible = false; root.view.forceActiveFocus() }
                }
                ALabel { text: "(two lines: angle in deg; circle: diameter)"; color: Theme.textDim }
            }
        }
    }
}
