import QtQuick

Text {
    property bool dim: false
    color: dim ? Theme.textDim : Theme.text
    font.family: Theme.font
    font.pixelSize: Theme.fontSize
    elide: Text.ElideRight
}
