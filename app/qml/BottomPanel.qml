import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// Collapsible bottom panel: Timeline (undo history), Script editor, Console.
Rectangle {
    id: root
    property bool expanded: true
    property int tab: 0
    implicitHeight: expanded ? 210 : 30
    color: Theme.panel
    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 1
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            Layout.leftMargin: Theme.unit
            spacing: 2
            Repeater {
                model: ["Timeline", "Script", "Console"]
                delegate: AButton {
                    objectName: "tab_" + modelData
                    text: modelData
                    checkable: true
                    checked: root.tab === index && root.expanded
                    implicitHeight: 24
                    onClicked: { root.tab = index; root.expanded = true }
                }
            }
            Item { Layout.fillWidth: true }
            ALabel {
                text: App.historyPosition + " / " + App.history.length + " steps"
                dim: true
                font.pixelSize: Theme.fontSmall
            }
            AButton {
                text: root.expanded ? "▾" : "▴"
                implicitHeight: 24
                tip: root.expanded ? "Collapse" : "Expand"
                onClicked: root.expanded = !root.expanded
            }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.expanded
            currentIndex: root.tab

            // ---------------------------------------------------------- timeline
            ListView {
                id: timeline
                orientation: ListView.Horizontal
                clip: true
                spacing: 6
                leftMargin: Theme.unit
                rightMargin: Theme.unit
                topMargin: Theme.unit
                model: App.history
                ScrollBar.horizontal: ScrollBar {}
                onCountChanged: positionViewAtEnd()
                header: Item {
                    width: 70; height: 1
                    AButton {
                        y: 0
                        text: "Start"
                        implicitHeight: 52
                        checked: App.historyPosition === 0
                        onClicked: App.jumpToHistory(0)
                    }
                }
                delegate: Rectangle {
                    readonly property bool applied: index < App.historyPosition
                    readonly property bool current: index === App.historyPosition - 1
                    width: 220
                    height: 52
                    radius: Theme.radius
                    color: current ? Qt.rgba(1, 0.6, 0.18, 0.18) : Theme.panelAlt
                    border.color: current ? Theme.accent : Theme.border
                    opacity: applied ? 1 : 0.45
                    Column {
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 2
                        ALabel {
                            text: "#" + (index + 1) + "  " + modelData.origin
                            dim: true
                            font.pixelSize: Theme.fontSmall
                        }
                        ALabel {
                            width: parent.width
                            text: modelData.label
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        ToolTip.visible: containsMouse
                        ToolTip.text: modelData.source
                        ToolTip.delay: 500
                        onClicked: App.jumpToHistory(index + 1)
                    }
                }
            }

            // ---------------------------------------------------------- script
            RowLayout {
                spacing: 0
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    TextArea {
                        id: script
                        objectName: "scriptEditor"
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize
                        color: Theme.text
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.accentText
                        wrapMode: TextEdit.NoWrap
                        placeholderText: "from anvil import *\nbase = cylinder(d=25*mm, h=3*mm).chamfer(top=0.6*mm)\npart = base - cylinder(d=5.1*mm, h=2.2*mm)\nshow(part)"
                        placeholderTextColor: Theme.textFaint
                        background: Rectangle { color: Theme.field }
                        Keys.onPressed: (e) => {
                            if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && (e.modifiers & Qt.ControlModifier)) {
                                App.execute(script.text, "script"); e.accepted = true
                            }
                        }
                    }
                }
                ColumnLayout {
                    Layout.margins: Theme.unit
                    AButton { objectName: "btnRunScript"; text: "▶ Run"; primary: true; tip: "Run as one undoable command (Ctrl+Enter)"; onClicked: App.execute(script.text, "script") }
                    AButton { text: "Clear"; onClicked: script.text = "" }
                    Item { Layout.fillHeight: true }
                }
            }

            // ---------------------------------------------------------- console
            Flickable {
                id: consoleView
                clip: true
                ScrollBar.vertical: ScrollBar {}
                TextArea.flickable: TextArea {
                    id: consoleArea
                    readOnly: true
                    text: App.consoleText
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSmall
                    color: Theme.text
                    wrapMode: TextEdit.Wrap
                    background: Rectangle { color: Theme.field }
                    onTextChanged: cursorPosition = length
                }
            }
        }
    }
}
