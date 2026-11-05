import QtQuick
import QtQuick.Controls

// Flat button styled by the design tokens.
Button {
    id: b
    property bool primary: false
    property bool danger: false
    property string tip: ""
    implicitHeight: 28
    padding: 6
    leftPadding: 10
    rightPadding: 10
    font.family: Theme.font
    font.pixelSize: Theme.fontSize
    hoverEnabled: true
    ToolTip.visible: tip !== "" && hovered
    ToolTip.text: tip
    ToolTip.delay: 400
    contentItem: Text {
        text: b.text
        font: b.font
        color: !b.enabled ? Theme.textFaint : b.primary ? Theme.accentText : b.danger ? Theme.danger : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.radius
        color: b.primary ? (b.down ? Qt.darker(Theme.accent, 1.2) : Theme.accent)
                         : b.checked ? Theme.hover : b.down ? Theme.border : b.hovered ? Theme.hover : "transparent"
        border.color: b.primary ? "transparent" : (b.checked ? Theme.accent : Theme.border)
        border.width: 1
    }
}
