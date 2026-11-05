import QtQuick
import QtQuick.Dialogs
import Anvil.Native

Item {
    function importStl() { importDlg.open() }
    function save() { saveDlg.open() }
    function open() { openDlg.open() }
    FileDialog {
        id: importDlg
        title: "Import STL"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["STL files (*.stl)"]
        onAccepted: App.dropFiles(selectedFiles)
    }
    FileDialog {
        id: saveDlg
        title: "Save document"
        fileMode: FileDialog.SaveFile
        nameFilters: ["ANVIL documents (*.anvil)"]
        onAccepted: App.saveDocument(App.urlToPath(selectedFile))
    }
    FileDialog {
        id: openDlg
        title: "Open document"
        fileMode: FileDialog.OpenFile
        nameFilters: ["ANVIL documents (*.anvil)"]
        onAccepted: App.openDocument(App.urlToPath(selectedFile))
    }
}
