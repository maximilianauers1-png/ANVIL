import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// What the local AI proposes: a summary, the exact Python it would run and
// anything it was not allowed to do. Nothing changes until "Apply" (one undo step).
Rectangle {
    id: root
    readonly property var p: App.aiProposal
    readonly property bool hasProposal: p && p.prompt !== undefined
    visible: hasProposal
    height: col.implicitHeight + 20
    radius: Theme.radius
    color: Theme.panel
    border.color: Theme.accent
    border.width: 1.5

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 10
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            ALabel { text: "AI"; font.bold: true; color: Theme.accent }
            ALabel {
                Layout.fillWidth: true
                text: root.hasProposal ? "“" + root.p.prompt + "”" : ""
                elide: Text.ElideRight
            }
            ALabel { text: App.aiModel; dim: true; font.pixelSize: Theme.fontSmall }
        }
        ALabel {
            visible: root.hasProposal && root.p.pending === true
            text: App.aiActivity !== "" ? App.aiActivity : "Thinking…"
            color: Theme.accent
        }
        Repeater {
            model: root.hasProposal && root.p.summary ? root.p.summary : []
            delegate: ALabel { required property var modelData; text: "•  " + modelData }
        }
        ALabel {
            visible: root.hasProposal && (root.p.answer || "") !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.hasProposal ? (root.p.answer || "") : ""
        }
        Repeater {
            model: root.hasProposal && root.p.errors ? root.p.errors : []
            delegate: ALabel {
                required property var modelData
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "⚠  " + modelData
                color: Theme.warn
            }
        }
        Rectangle {
            visible: root.hasProposal && (root.p.python || "") !== ""
            Layout.fillWidth: true
            implicitHeight: code.implicitHeight + 12
            radius: Theme.radius
            color: Theme.field
            border.color: Theme.border
            Text {
                id: code
                anchors.fill: parent
                anchors.margins: 6
                text: root.hasProposal ? (root.p.python || "") : ""
                font.family: Theme.mono
                font.pixelSize: Theme.fontSmall + 1
                color: Theme.text
                wrapMode: Text.WrapAnywhere
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 6
            AButton {
                objectName: "aiDiscard"
                text: root.hasProposal && root.p.pending === true ? "Cancel" : "Discard"
                onClicked: { if (root.p.pending === true) App.aiCancel(); App.aiDiscard() }
            }
            AButton {
                objectName: "aiApply"
                visible: root.hasProposal && (root.p.python || "") !== ""
                primary: true
                text: "Apply (one undo step)"
                onClicked: App.aiApply()
            }
        }
    }
}
