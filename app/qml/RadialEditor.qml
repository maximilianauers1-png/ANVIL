import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Radial layout editor: drag commands onto the 8 slots, or drag slots onto
// each other to swap. Slot positions are what muscle memory learns, so they
// only change here.
Popup {
    id: dlg
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 760
    height: 520
    padding: Theme.unit * 2
    background: Rectangle { color: Theme.panelAlt; border.color: Theme.borderStrong; radius: Theme.radius * 2 }
    property string context: "empty"
    property var layout: ({})
    property var ring: []

    onOpened: { layout = JSON.parse(JSON.stringify(App.radialLayout())); load() }
    function load() {
        const r = (layout[App.mode] || {})[context] || []
        const out = []
        for (let i = 0; i < 8; ++i) out.push(i < r.length ? r[i] : "")
        ring = out
    }
    function setSlot(i, value) {
        const r = ring.slice()
        r[i] = value
        ring = r
        if (!layout[App.mode]) layout[App.mode] = {}
        layout[App.mode][context] = r
    }
    // floating drag proxy shared by slots and the command list
    function startDrag(item, m, kind, payload, label) {
        const p = item.mapToItem(dlg.contentItem, m.x, m.y)
        proxy.kind = kind
        proxy.payload = payload
        proxy.label = label
        proxy.x = p.x - proxy.width / 2
        proxy.y = p.y - proxy.height / 2
        proxy.visible = true
    }
    function finishDrag() {
        proxy.Drag.drop()
        proxy.visible = false
    }
    function slotLabel(v) {
        if (v === "" || v === undefined || v === null) return "(empty)"
        if (typeof v === "object") return v.submenu + " ▸"
        return App.commandLabel(v)
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.unit * 2
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: 420
            ALabel { text: "Radial menu  ·  " + App.mode; font.pixelSize: Theme.fontLarge; font.bold: true }
            RowLayout {
                AButton { text: "Nothing selected"; checkable: true; checked: dlg.context === "empty"; onClicked: { dlg.context = "empty"; dlg.load() } }
                AButton { text: "With selection"; checkable: true; checked: dlg.context === "selection"; onClicked: { dlg.context = "selection"; dlg.load() } }
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Repeater {
                    model: 8
                    delegate: Rectangle {
                        id: slot
                        required property int index
                        readonly property real a: -index * Math.PI / 4
                        width: 110
                        height: 40
                        radius: Theme.radius
                        x: parent.width / 2 + 150 * Math.cos(a) - width / 2
                        y: parent.height / 2 + 150 * Math.sin(a) - height / 2
                        color: drop.containsDrag ? Theme.accent : Theme.field
                        border.color: Theme.border
                        ALabel {
                            anchors.centerIn: parent
                            width: parent.width - 8
                            horizontalAlignment: Text.AlignHCenter
                            text: dlg.slotLabel(dlg.ring[slot.index])
                        }
                        MouseArea {
                            anchors.fill: parent
                            drag.target: proxy
                            onPressed: (m) => dlg.startDrag(slot, m, "slot", slot.index, dlg.slotLabel(dlg.ring[slot.index]))
                            onReleased: dlg.finishDrag()
                        }
                        DropArea {
                            id: drop
                            anchors.fill: parent
                            keys: ["slot", "command"]
                            onDropped: (d) => {
                                if (proxy.kind === "command") dlg.setSlot(slot.index, proxy.payload)
                                else if (proxy.payload !== slot.index) {
                                    const other = proxy.payload
                                    const tmp = dlg.ring[other]
                                    dlg.setSlot(other, dlg.ring[slot.index])
                                    dlg.setSlot(slot.index, tmp)
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 60; height: 60; radius: 30
                    color: "transparent"; border.color: Theme.border
                    ALabel { anchors.centerIn: parent; text: "cursor"; dim: true; font.pixelSize: Theme.fontSmall }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ALabel { text: "Commands (drag onto a slot)"; dim: true }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: App.commands
                spacing: 2
                delegate: Rectangle {
                    id: cmd
                    property string cmdId: modelData.id
                    width: ListView.view.width
                    height: 26
                    radius: 3
                    color: cmdMouse.containsMouse ? Theme.hover : "transparent"
                    ALabel { anchors.verticalCenter: parent.verticalCenter; x: 6; text: modelData.label + "   " + modelData.category; }
                    MouseArea {
                        id: cmdMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        drag.target: proxy
                        onPressed: (m) => dlg.startDrag(cmd, m, "command", modelData.id, modelData.label)
                        onReleased: dlg.finishDrag()
                    }
                }
            }
            RowLayout {
                AButton { text: "Reset to default"; onClicked: { App.resetRadialLayout(); dlg.layout = JSON.parse(JSON.stringify(App.radialLayout())); dlg.load() } }
                Item { Layout.fillWidth: true }
                AButton { text: "Cancel"; onClicked: dlg.close() }
                AButton { text: "Save"; primary: true; onClicked: { App.saveRadialLayout(dlg.layout); dlg.close() } }
            }
        }
    }
    Rectangle {
        id: proxy
        parent: dlg.contentItem
        visible: false
        z: 100
        width: 110
        height: 30
        radius: Theme.radius
        color: Theme.accent
        opacity: 0.9
        property string kind: ""
        property var payload
        property string label: ""
        Drag.active: visible
        Drag.keys: ["slot", "command"]
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2
        Text { anchors.centerIn: parent; text: proxy.label; color: Theme.accentText; font.family: Theme.font }
    }
}
