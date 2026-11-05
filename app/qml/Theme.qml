pragma Singleton
import QtQuick
import Anvil.Native

// Design tokens. Every UI color/size comes from here; dark/light switch live.
QtObject {
    readonly property bool dark: App.theme !== "light"

    // surfaces
    readonly property color bg:          dark ? "#0f1114" : "#eef0f3"
    readonly property color panel:       dark ? "#16191d" : "#f8f9fb"
    readonly property color panelAlt:    dark ? "#1c2025" : "#ffffff"
    readonly property color field:       dark ? "#0c0e11" : "#ffffff"
    readonly property color border:      dark ? "#2a2f36" : "#d3d8df"
    readonly property color borderStrong: dark ? "#3a414a" : "#b7bfc9"
    readonly property color hover:       dark ? "#232830" : "#e6eaf0"
    // text
    readonly property color text:        dark ? "#e6e9ed" : "#1a1d21"
    readonly property color textDim:     dark ? "#8b939d" : "#5d6672"
    readonly property color textFaint:   dark ? "#59616b" : "#9aa2ac"
    // accents
    readonly property color accent:      "#ff9a2e"
    readonly property color accentText:  "#1a1208"
    readonly property color info:        dark ? "#5ab4ff" : "#1f78c8"
    readonly property color ok:          dark ? "#5fd08a" : "#1f9a52"
    readonly property color warn:        "#f0c040"
    readonly property color danger:      dark ? "#ff6b61" : "#c8372d"
    // viewport
    readonly property color viewportBg:  dark ? "#121417" : "#e9ecf0"
    readonly property color grid:        dark ? "#6c7682" : "#8a95a3"
    readonly property color edge:        dark ? Qt.rgba(0, 0, 0, 0.55) : Qt.rgba(0.1, 0.12, 0.15, 0.6)
    readonly property var objectColors:  dark ? ["#aab2bc", "#8fb0d0", "#c9b48f", "#9cc0a4", "#c49ab8", "#b9a7d6"]
                                               : ["#8a939e", "#6f93b8", "#b0976b", "#79a385", "#a87b9a", "#9785bd"]
    // snapping marker colors by kind
    function snapColor(kind) {
        switch (kind) {
        case "point": return "#ffd23f"
        case "mid": return "#5ad1ff"
        case "center": return "#ff7ad9"
        case "cross": return "#ff9a2e"
        case "constraint": return "#9cff6b"
        case "axis": return "#ff6b61"
        case "grid": return "#9aa6b2"
        default: return dark ? "#d0d6dc" : "#3b434c"
        }
    }

    // typography & metrics
    readonly property string font: Qt.platform.os === "windows" ? "Segoe UI" : "Sans Serif"
    readonly property string mono: Qt.platform.os === "windows" ? "Consolas" : "Monospace"
    readonly property int fontSize: 13
    readonly property int fontSmall: 11
    readonly property int fontLarge: 16
    readonly property int unit: 8          // spacing grid
    readonly property int radius: 4
    readonly property int barHeight: 40
    readonly property int panelWidth: 340

    // radial menu
    readonly property int radialInner: 34
    readonly property int radialOuter: 128
    readonly property color radialFill: dark ? Qt.rgba(0.09, 0.10, 0.12, 0.94) : Qt.rgba(0.98, 0.98, 0.99, 0.96)
    readonly property color radialDisabled: dark ? "#4a5059" : "#b0b6be"
}
