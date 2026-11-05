import QtQuick
import QtQuick.Layouts
import Anvil.Native

Rectangle {
    id: bar
    property var view
    implicitHeight: 26
    color: Theme.panel
    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

    function mb(b) { return (b / 1048576).toFixed(b < 1048576 * 10 ? 1 : 0) + " MiB" }
    function gb(b) { return (b / 1073741824).toFixed(1) + " GiB" }
    function count(n) { return n >= 1e6 ? (n / 1e6).toFixed(2) + " M" : n >= 1e3 ? (n / 1e3).toFixed(1) + " k" : "" + n }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.unit
        anchors.rightMargin: Theme.unit
        spacing: Theme.unit * 2

        ALabel {
            objectName: "statusMessage"
            Layout.fillWidth: true
            font.pixelSize: Theme.fontSmall
            text: App.busy ? "Working…" : App.lastError !== "" ? App.lastError : App.hardwareWarning
            color: App.lastError !== "" ? Theme.danger : App.hardwareWarning !== "" ? Theme.warn : Theme.textDim
            elide: Text.ElideRight
        }
        ALabel { text: App.autosaveInfo; dim: true; font.pixelSize: Theme.fontSmall }
        ALabel { text: bar.count(bar.view.triangles) + " △"; dim: true; font.pixelSize: Theme.fontSmall }
        ALabel {
            text: "frame p50 " + bar.view.frameP50.toFixed(1) + " / p99 " + bar.view.frameP99.toFixed(1) + " ms"
                  + (bar.view.gpuMs > 0 ? "  ·  GPU " + bar.view.gpuMs.toFixed(2) + " ms" : "")
            font.pixelSize: Theme.fontSmall
            color: bar.view.frameP99 > 16.7 ? Theme.warn : Theme.textDim
        }
        ALabel {
            text: "VRAM " + bar.mb(bar.view.gpuBytes) + " / " + bar.gb(bar.view.vramTotal)
            dim: true
            font.pixelSize: Theme.fontSmall
        }
        ALabel {
            text: (App.gpuInfo !== "" ? App.gpuInfo : bar.view.deviceName) + "  ·  MSAA " + bar.view.msaa + "x"
            dim: true
            font.pixelSize: Theme.fontSmall
        }
        ALabel {
            visible: App.startupMs > 0
            text: "start " + (App.startupMs / 1000).toFixed(2) + " s"
            font.pixelSize: Theme.fontSmall
            color: App.startupMs > 3000 ? Theme.warn : Theme.textDim
        }
        ALabel { text: "offline"; color: Theme.ok; font.pixelSize: Theme.fontSmall }
    }
}
