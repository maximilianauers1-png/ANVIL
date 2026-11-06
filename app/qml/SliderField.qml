import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Anvil.Native

// A slider with a number box next to it: drag, or type a value and press
// Enter (or leave the box). kind: "length" (document unit, "2", "2 mm",
// "0.1 in" are accepted), "percent" (value 0..1 shown as %), "number".
// Typed values are clamped to [from, to]; log: logarithmic slider.
RowLayout {
    id: r
    property string label: ""
    property real value: 0
    property real from: 0
    property real to: 1
    property bool log: false
    property string kind: "number"
    property int decimals: 2
    property string sliderName: ""
    property string fieldName: ""
    property string tip: ""
    signal edited(real v)
    spacing: 6

    function show(v) {
        if (kind === "length") return App.formatLength(v)
        if (kind === "percent") return Math.round(v * 100) + " %"
        return Number(v).toFixed(decimals)
    }
    function parse(t) {
        if (kind === "length") return App.parseLength(t)
        var n = parseFloat(String(t).replace(",", ".").replace("%", ""))
        if (isNaN(n)) return NaN
        return kind === "percent" ? n / 100 : n
    }
    function commit() {
        var v = parse(field.text)
        if (isNaN(v)) { field.invalid = true; return }
        field.invalid = false
        v = Math.min(to, Math.max(from, v))
        r.edited(v)
        field.text = show(v)
    }

    ALabel { text: r.label; Layout.preferredWidth: 58 }
    Slider {
        objectName: r.sliderName
        Layout.preferredWidth: 150
        from: r.log ? Math.log(r.from) : r.from
        to: r.log ? Math.log(r.to) : r.to
        value: r.log ? Math.log(Math.max(r.value, r.from)) : r.value
        onMoved: r.edited(r.log ? Math.exp(value) : value)
        ToolTip.visible: r.tip !== "" && hovered
        ToolTip.text: r.tip
    }
    AField {
        id: field
        objectName: r.fieldName
        Layout.preferredWidth: 74
        horizontalAlignment: TextInput.AlignRight
        text: r.show(r.value)
        onAccepted: r.commit()
        onEditingFinished: r.commit()
        // follow slider / external changes while not being edited
        Connections {
            target: r
            function onValueChanged() { if (!field.activeFocus) field.text = r.show(r.value) }
        }
    }
}
