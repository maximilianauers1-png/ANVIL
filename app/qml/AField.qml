import QtQuick
import QtQuick.Controls

TextField {
    id: f
    property bool invalid: false
    implicitHeight: 26
    font.family: Theme.font
    font.pixelSize: Theme.fontSize
    color: Theme.text
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    placeholderTextColor: Theme.textFaint
    leftPadding: 6
    rightPadding: 6
    selectByMouse: true
    background: Rectangle {
        radius: Theme.radius
        color: Theme.field
        border.width: 1
        border.color: f.invalid ? Theme.danger : f.activeFocus ? Theme.accent : Theme.border
    }
}
