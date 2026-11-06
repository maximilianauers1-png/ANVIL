import QtQuick
import QtQuick.Dialogs
import Anvil.Native

Item {
    function importStl() { importDlg.open() }
    function importModel() { modelDlg.open() }
    function save() { saveDlg.open() }
    function open() { openDlg.open() }
    function importStep() { stepInDlg.open() }
    function exportStep() { stepOutDlg.open() }
    function py(s) { return JSON.stringify(s) }  // JSON string literal == Python string literal for paths
    FileDialog {
        id: stepInDlg
        title: "Import STEP"
        fileMode: FileDialog.OpenFile
        nameFilters: ["STEP files (*.step *.stp)"]
        onAccepted: App.execute("import_step(" + py(App.urlToPath(selectedFile)) + ")", "ui")
    }
    FileDialog {
        id: stepOutDlg
        title: "Export STEP"
        fileMode: FileDialog.SaveFile
        nameFilters: ["STEP files (*.step)"]
        onAccepted: App.execute("export_step([" + App.selection.map(py).join(", ") + "], " + py(App.urlToPath(selectedFile)) + ")", "ui")
    }
    FileDialog {
        id: importDlg
        title: "Import STL"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["STL files (*.stl)"]
        onAccepted: App.dropFiles(selectedFiles)
    }
    FileDialog {
        id: modelDlg
        title: "Import mini (Tripo, Meshy, Blender, ...)"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["3D models (*.glb *.gltf *.obj *.stl)", "glTF (*.glb *.gltf)", "Wavefront OBJ (*.obj)"]
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
