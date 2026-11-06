import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// AI mode: the local language model (Ollama on 127.0.0.1), its models
// (installer) and the prompt. ANVIL itself never connects to the internet;
// downloads are done by Ollama after you click Install.
Rectangle {
    id: root
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.border
    width: 380
    height: col.implicitHeight + 16
    readonly property var st: App.aiStatus
    readonly property bool running: st && st.running === true
    function installed(name) {
        if (!st || !st.models) return false
        for (var i = 0; i < st.models.length; ++i)
            if (st.models[i].name === name || st.models[i].name === name + ":latest") return true
        return false
    }
    Component.onCompleted: App.aiRefresh()

    component Section: ALabel { dim: true; font.pixelSize: Theme.fontSmall; topPadding: 4 }

    ColumnLayout {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 5
        ALabel { text: "LOCAL AI ASSISTANT"; font.bold: true }
        ALabel {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: "Describe what to do with your minis (German or English). The model answers only with ANVIL tools; " +
                  "you see the exact Python and apply it as one undo step. Everything runs on this PC."
        }

        Section { text: "1  SERVER (OLLAMA)" }
        RowLayout {
            Layout.fillWidth: true
            Rectangle { width: 10; height: 10; radius: 5; color: root.running ? Theme.ok : Theme.danger }
            ALabel {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.running ? "Ollama " + (root.st.version || "") + " on 127.0.0.1" : "Not found"
            }
            AButton { objectName: "aiCheck"; text: "Check again"; onClicked: App.aiRefresh() }
        }
        ALabel {
            visible: !root.running
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSmall
            text: "Install Ollama once (free, MIT licence): download it from ollama.com in your browser and run the " +
                  "installer; it starts in the background. ANVIL only connects to it locally (127.0.0.1:11434)." +
                  (root.st && root.st.message ? "\n" + root.st.message : "")
        }

        Section { text: "2  MODEL" }
        Repeater {
            model: App.aiModels
            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: mrow.implicitHeight + 10
                radius: Theme.radius
                color: App.aiModel === modelData.name ? Theme.panelAlt : "transparent"
                border.color: App.aiModel === modelData.name ? Theme.accent : Theme.border
                ColumnLayout {
                    id: mrow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 6
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        ALabel { text: modelData.label + (modelData.recommended ? "  (recommended)" : ""); font.bold: true; Layout.fillWidth: true }
                        ALabel { text: modelData.size_gb.toFixed(1) + " GB"; dim: true }
                    }
                    ALabel {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.fontSmall
                        dim: true
                        text: modelData.note + " · " + modelData.license + " · " + modelData.source
                    }
                    RowLayout {
                        spacing: 4
                        AButton {
                            objectName: "aiInstall_" + modelData.name
                            visible: root.running && !root.installed(modelData.name)
                            enabled: !App.aiBusy
                            text: "Install"
                            tip: "Ollama downloads " + modelData.name + " (" + modelData.size_gb.toFixed(1) + " GB) from its registry"
                            onClicked: App.aiPull(modelData.name)
                        }
                        AButton {
                            visible: root.installed(modelData.name)
                            text: App.aiModel === modelData.name ? "In use" : "Use"
                            primary: App.aiModel === modelData.name
                            onClicked: App.aiModel = modelData.name
                        }
                        AButton {
                            visible: root.installed(modelData.name) && App.aiModel !== modelData.name
                            text: "Remove"
                            danger: true
                            onClicked: App.aiDelete(modelData.name)
                        }
                    }
                }
            }
        }
        ProgressBar {
            visible: App.aiProgress >= 0
            Layout.fillWidth: true
            from: 0; to: 1
            value: Math.max(0, App.aiProgress)
        }
        RowLayout {
            visible: App.aiActivity !== ""
            Layout.fillWidth: true
            ALabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: App.aiActivity; color: Theme.accent; font.pixelSize: Theme.fontSmall }
            AButton { visible: App.aiBusy; text: "Stop"; onClicked: App.aiCancel() }
        }

        Section { text: "3  ASK" }
        AField {
            id: prompt
            objectName: "aiPrompt"
            Layout.fillWidth: true
            placeholderText: "e.g. Hebe den rechten Arm und gib ihm einen Zweihänder"
            onAccepted: { App.aiAsk(text); text = "" }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: ["Pose: attack", "Zweihänder in die rechte Hand", "32-mm-Sockel mit Magnetloch 5 mm", "Unten 0,4 mm flach abschneiden"]
                delegate: AButton {
                    required property var modelData
                    text: modelData
                    font.pixelSize: Theme.fontSmall
                    onClicked: App.aiAsk(modelData)
                }
            }
        }
        ALabel {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            dim: true
            font.pixelSize: Theme.fontSmall
            text: "Tip: type ? followed by your request in the command bar from any mode."
        }
    }
}
