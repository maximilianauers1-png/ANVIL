import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// One line, three roles: fuzzy command search, "> " Python, "? " local AI.
Rectangle {
    id: root
    implicitHeight: 44
    color: Theme.panel
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }

    property string role: field.text.startsWith(">") ? "python" : field.text.startsWith("?") ? "ai" : "search"
    property var results: []
    property int current: 0
    property string syntaxError: ""
    property string hint: ""

    function focusBar() { field.forceActiveFocus(); field.selectAll() }

    function refresh() {
        if (role === "search") {
            results = field.text.length > 0 ? App.fuzzy(field.text) : []
            current = 0
            syntaxError = ""
            hint = ""
            App.clearPreview()
        } else if (role === "python") {
            const src = field.text.substring(1)
            const r = App.checkSyntax(src)
            syntaxError = r.ok ? "" : r.error
            const words = App.complete(src)
            results = words.map(w => ({ label: w, id: "", help: "", completion: true }))
            current = 0
            hint = App.signature(src)
            previewTimer.restart()
        } else {
            results = []
            hint = "The local AI assistant (Ollama on 127.0.0.1, typed tools, reviewable Python) arrives with milestone M6."
            App.clearPreview()
        }
    }

    function accept() {
        if (role === "search") {
            if (results.length > 0 && results[current].enabled !== false) {
                App.runCommand(results[current].id)
                field.text = ""
            }
        } else if (role === "python") {
            const src = field.text.substring(1).trim()
            if (src.length === 0 || syntaxError !== "") return
            App.clearPreview()
            App.execute(src, "console")
            history.unshift(field.text)
            field.text = ""
        }
    }
    property var history: []
    property int historyIndex: -1

    function applyCompletion(word) {
        const t = field.text
        const m = t.match(/[A-Za-z_][A-Za-z0-9_.]*$/)
        field.text = (m ? t.substring(0, t.length - m[0].length) : t) + word
        field.cursorPosition = field.text.length
    }

    Timer {
        id: previewTimer
        interval: 250
        onTriggered: if (root.role === "python" && root.syntaxError === "") App.preview(field.text.substring(1))
    }
    Connections {
        target: App
        function onPreviewError(err) { if (root.role === "python" && err !== "") root.syntaxError = err }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.unit * 2
        anchors.rightMargin: Theme.unit * 2
        spacing: Theme.unit

        Rectangle {
            width: 64
            height: 22
            radius: 11
            color: root.role === "python" ? Theme.info : root.role === "ai" ? Theme.ok : Theme.hover
            Text {
                anchors.centerIn: parent
                text: root.role === "python" ? "PYTHON" : root.role === "ai" ? "AI" : "SEARCH"
                color: root.role === "search" ? Theme.textDim : "#0b0d10"
                font.pixelSize: Theme.fontSmall
                font.bold: true
            }
        }
        AField {
            id: field
            objectName: "commandField"
            Layout.fillWidth: true
            font.family: root.role === "python" ? Theme.mono : Theme.font
            placeholderText: "Search commands   ·   > Python, e.g.  > show(cylinder(d=25*mm, h=3*mm))   ·   ? ask the local AI"
            invalid: root.syntaxError !== ""
            onTextChanged: root.refresh()
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Down) { root.current = Math.min(root.current + 1, root.results.length - 1); e.accepted = true }
                else if (e.key === Qt.Key_Up) {
                    if (root.results.length === 0 && root.history.length > 0) {
                        root.historyIndex = Math.min(root.historyIndex + 1, root.history.length - 1)
                        field.text = root.history[root.historyIndex]
                    } else root.current = Math.max(root.current - 1, 0)
                    e.accepted = true
                }
                else if (e.key === Qt.Key_Tab && root.role === "python" && root.results.length > 0) {
                    root.applyCompletion(root.results[root.current].label); e.accepted = true
                }
                else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { root.accept(); root.historyIndex = -1; e.accepted = true }
                else if (e.key === Qt.Key_Escape) { field.text = ""; App.clearPreview(); viewportFocus(); e.accepted = true }
            }
            function viewportFocus() { field.focus = false }
        }
    }

    // results / errors / hints below the bar
    Rectangle {
        id: popup
        visible: field.activeFocus && (root.results.length > 0 || root.syntaxError !== "" || root.hint !== "")
        x: Theme.unit * 2 + 72
        y: root.height
        width: Math.min(720, root.width - x - Theme.unit * 2)
        height: col.implicitHeight + 8
        color: Theme.panelAlt
        border.color: Theme.borderStrong
        radius: Theme.radius
        Column {
            id: col
            x: 4; y: 4
            width: parent.width - 8
            ALabel {
                visible: root.syntaxError !== ""
                width: parent.width
                text: root.syntaxError
                color: Theme.danger
                wrapMode: Text.Wrap
                font.family: Theme.mono
            }
            ALabel {
                visible: root.hint !== ""
                width: parent.width
                text: root.hint
                dim: true
                wrapMode: Text.Wrap
                font.family: root.role === "python" ? Theme.mono : Theme.font
            }
            Repeater {
                model: root.results
                delegate: Rectangle {
                    width: col.width
                    height: 28
                    radius: 3
                    color: index === root.current ? Theme.hover : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        ALabel {
                            text: modelData.label
                            font.family: modelData.completion ? Theme.mono : Theme.font
                            color: modelData.enabled === false ? Theme.textFaint : Theme.text
                        }
                        ALabel { text: modelData.category || ""; dim: true; font.pixelSize: Theme.fontSmall }
                        ALabel { Layout.fillWidth: true; text: modelData.help || ""; dim: true; font.pixelSize: Theme.fontSmall }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            root.current = index
                            if (modelData.completion) root.applyCompletion(modelData.label)
                            else root.accept()
                        }
                    }
                }
            }
        }
    }
}
