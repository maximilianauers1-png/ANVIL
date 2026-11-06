pragma Singleton
import QtQuick
import Anvil.Native

// Design tokens. Every UI color/size comes from here; themes switch live.
//   dark   - dark controls, dark scene
//   studio - dark controls, light scene, grey parts (default)
//   light  - light controls, light scene (muted, not paper-white)
QtObject {
    readonly property string name: App.theme
    readonly property bool darkUi: name !== "light"       // controls
    readonly property bool lightScene: name !== "dark"    // 3D viewport
    readonly property bool dark: darkUi                   // compatibility

    // ---------------------------------------------------------------- controls
    readonly property color bg:          darkUi ? "#0f1114" : "#dcdfe3"
    readonly property color panel:       darkUi ? "#16191d" : "#e4e7ea"
    readonly property color panelAlt:    darkUi ? "#1c2025" : "#eceef0"
    readonly property color field:       darkUi ? "#0c0e11" : "#f2f3f5"
    readonly property color border:      darkUi ? "#2a2f36" : "#c3c8cf"
    readonly property color borderStrong: darkUi ? "#3a414a" : "#a9b0b9"
    readonly property color hover:       darkUi ? "#232830" : "#d4d8dd"
    readonly property color text:        darkUi ? "#e6e9ed" : "#1d2024"
    readonly property color textDim:     darkUi ? "#8b939d" : "#555d68"
    readonly property color textFaint:   darkUi ? "#59616b" : "#8d949e"
    readonly property color accent:      "#ff9a2e"
    readonly property color accentText:  "#1a1208"
    readonly property color info:        darkUi ? "#5ab4ff" : "#1f6fb5"
    readonly property color ok:          darkUi ? "#5fd08a" : "#1c8a4a"
    readonly property color warn:        darkUi ? "#f0c040" : "#a87800"
    readonly property color danger:      darkUi ? "#ff6b61" : "#c0352b"

    // ---------------------------------------------------------------- scene
    readonly property color viewportBg:  name === "dark" ? "#121417" : name === "studio" ? "#eceef1" : "#d9dde2"
    readonly property color grid:        lightScene ? "#9aa1ab" : "#6c7682"
    readonly property color edge:        lightScene ? Qt.rgba(0.10, 0.11, 0.13, 0.70) : Qt.rgba(0, 0, 0, 0.55)
    readonly property color wire:        lightScene ? Qt.rgba(0.12, 0.13, 0.15, 0.38) : Qt.rgba(0.85, 0.88, 0.92, 0.30)
    readonly property var objectColors:  name === "studio" ? ["#8c9199", "#7d838c", "#979ca4", "#868b93"]
                                       : name === "dark" ? ["#aab2bc", "#8fb0d0", "#c9b48f", "#9cc0a4", "#c49ab8", "#b9a7d6"]
                                                         : ["#8a939e", "#6f93b8", "#b0976b", "#79a385", "#a87b9a", "#9785bd"]
    readonly property color sceneText:   lightScene ? "#4a515b" : "#8b939d"   // hints drawn over the viewport
    readonly property color cubeFace:    lightScene ? "#f7f8f9" : "#1c2025"
    readonly property color cubeLine:    lightScene ? "#9aa1ab" : "#3a414a"
    readonly property color cubeText:    lightScene ? "#4a515b" : "#8b939d"

    // snapping marker colors by kind (readable on light and dark scenes)
    function snapColor(kind) {
        switch (kind) {
        case "point": return lightScene ? "#c48a00" : "#ffd23f"
        case "mid": return lightScene ? "#0a84c6" : "#5ad1ff"
        case "center": return lightScene ? "#c2187a" : "#ff7ad9"
        case "cross": return "#e07a10"
        case "constraint": return lightScene ? "#3f9a12" : "#9cff6b"
        case "axis": return "#d9443a"
        case "grid": return lightScene ? "#5d6670" : "#9aa6b2"
        default: return lightScene ? "#3b434c" : "#d0d6dc"
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

    // radial menu (drawn over the scene, styled like the controls)
    readonly property int radialInner: 34
    readonly property int radialOuter: 128
    readonly property color radialFill: darkUi ? Qt.rgba(0.09, 0.10, 0.12, 0.94) : Qt.rgba(0.93, 0.94, 0.95, 0.96)
    readonly property color radialDisabled: darkUi ? "#4a5059" : "#9aa1aa"
    // gear wheel (radial menu): opaque brushed steel, the same in every theme
    readonly property color gearLight: "#4a515c"
    readonly property color gearDark: "#1b1e23"
    readonly property color gearEdge: "#7a838f"
    readonly property color gearGroove: "#0c0e11"
    readonly property color gearFace: "#262a31"
    readonly property color gearFaceHot: "#343a43"
    readonly property color gearText: "#f4f6f8"
    readonly property color gearTextDisabled: "#79818c"
}
