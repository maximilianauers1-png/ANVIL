import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Anvil.Native

// STL export. STL has no unit, so the unit is shown large and explicitly.
// Export is only possible after every printability check passed.
Popup {
    id: dlg
    objectName: "exportDialog"
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: 560
    padding: Theme.unit * 2
    property var names: []
    property string unit: App.unit
    property var report: ({})
    background: Rectangle { color: Theme.panelAlt; border.color: Theme.borderStrong; radius: Theme.radius * 2 }

    function openFor(sel) {
        names = sel
        unit = App.unit
        pathField.text = App.defaultExportPath()
        chordField.text = String(App.chordMm)
        open()
        runCheck()
    }
    function runCheck() {
        const c = parseFloat(chordField.text)
        report = App.checkForExport(names, isNaN(c) ? App.chordMm : c)
    }
    readonly property var unitNames: ({ "mm": "millimetres (mm)", "cm": "centimetres (cm)", "m": "metres (m)", "in": "inches (in)" })

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.unit
        ALabel { text: "Export STL"; font.pixelSize: Theme.fontLarge; font.bold: true }
        ALabel {
            Layout.fillWidth: true
            text: dlg.names.length === 0 ? "All visible objects" : "Objects: " + dlg.names.join(", ")
            dim: true
            elide: Text.ElideRight
        }
        Rectangle {
            Layout.fillWidth: true
            height: 54
            radius: Theme.radius
            color: Theme.field
            border.color: Theme.accent
            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.unit
                Column {
                    Layout.fillWidth: true
                    ALabel { text: "Coordinates in the file are written in"; dim: true; font.pixelSize: Theme.fontSmall }
                    ALabel { objectName: "exportUnitLabel"; text: dlg.unitNames[dlg.unit]; font.pixelSize: Theme.fontLarge; font.bold: true; color: Theme.accent }
                }
                Repeater {
                    model: ["mm", "cm", "m", "in"]
                    delegate: AButton {
                        text: modelData
                        checkable: true
                        checked: dlg.unit === modelData
                        leftPadding: 6
                        rightPadding: 6
                        implicitWidth: 40
                        onClicked: dlg.unit = modelData
                    }
                }
            }
        }
        GridLayout {
            columns: 2
            Layout.fillWidth: true
            columnSpacing: Theme.unit
            ALabel { text: "Chord error"; dim: true }
            RowLayout {
                AField {
                    id: chordField
                    objectName: "exportChord"
                    Layout.preferredWidth: 100
                    onEditingFinished: dlg.runCheck()
                }
                ALabel { text: "mm   (circle segments n = ⌈π / arccos(1 − δ/r)⌉)"; dim: true; font.pixelSize: Theme.fontSmall }
            }
            ALabel { text: "Format"; dim: true }
            RowLayout {
                AButton { id: binBtn; text: "Binary"; checkable: true; checked: true; onClicked: asciiBtn.checked = false }
                AButton { id: asciiBtn; text: "ASCII"; checkable: true; onClicked: binBtn.checked = !checked }
            }
            ALabel { text: "File"; dim: true }
            RowLayout {
                AField { id: pathField; objectName: "exportPath"; Layout.fillWidth: true }
                AButton { text: "Browse…"; onClicked: saveDialog.open() }
            }
        }
        // checks
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(180, checkCol.implicitHeight + 16)
            radius: Theme.radius
            color: Theme.field
            border.color: dlg.report.ok ? Theme.ok : Theme.danger
            Flickable {
                anchors.fill: parent
                anchors.margins: 8
                contentHeight: checkCol.implicitHeight
                clip: true
                Column {
                    id: checkCol
                    width: parent.width
                    spacing: 3
                    ALabel {
                        objectName: "exportCheckSummary"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: (dlg.report.ok ? "✓ " : "✗ ") + (dlg.report.summary || "")
                        color: dlg.report.ok ? Theme.ok : Theme.danger
                    }
                    ALabel {
                        visible: dlg.report.ok === true
                        dim: true
                        font.pixelSize: Theme.fontSmall
                        text: "closed 2-manifold (3F = 2E, opposite half-edges) · Euler V−E+F = 2(1−g) per shell · signed volume > 0 · no degenerate triangles · no self-intersections"
                        width: parent.width
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: dlg.report.issues || []
                        delegate: ALabel {
                            width: checkCol.width
                            wrapMode: Text.Wrap
                            text: "• " + modelData.message
                            color: Theme.danger
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            AButton { text: "Re-check"; onClicked: dlg.runCheck() }
            Item { Layout.fillWidth: true }
            AButton { text: "Cancel"; onClicked: dlg.close() }
            AButton {
                objectName: "exportConfirm"
                text: "Export"
                primary: true
                enabled: dlg.report.ok === true && pathField.text !== ""
                onClicked: {
                    App.exportStl(dlg.names, pathField.text, dlg.unit, parseFloat(chordField.text), asciiBtn.checked)
                    dlg.close()
                }
            }
        }
    }
    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        nameFilters: ["STL files (*.stl)"]
        onAccepted: pathField.text = App.urlToPath(selectedFile)
    }
}
