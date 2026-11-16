pragma Singleton

import QtQuick 2.15
import Smith.Desktop 1.0

// The colors and fonts, from the backend's palette for the config's theme.
QtObject {
    readonly property var p: Backend.palette
    readonly property bool matrix: Backend.matrix
    // A light window, whatever the theme is called: hovering darkens instead of lightening.
    readonly property bool light: (Math.max(window.r, window.g, window.b) + Math.min(window.r, window.g, window.b)) / 2 > 0.5

    readonly property color window: p.window
    readonly property color surface: p.surface
    readonly property color surfaceRaised: p.surfaceRaised
    readonly property color border: p.border
    readonly property color borderStrong: p.borderStrong
    readonly property color text: p.text
    readonly property color textDim: p.textDim
    readonly property color textFaint: p.textFaint
    readonly property color accent: p.accent
    readonly property color accentSoft: p.accentSoft
    readonly property color accentWash: p.accentWash
    readonly property color code: p.code
    readonly property color codeWash: p.codeWash
    readonly property color info: p.info
    readonly property color warn: p.warn
    readonly property color error: p.error
    readonly property color added: p.added
    readonly property color addedWash: p.addedWash
    readonly property color removed: p.removed
    readonly property color removedWash: p.removedWash
    readonly property color userWash: p.userWash
    readonly property color selection: p.selection

    readonly property string mono: p.monoFamily
    readonly property string textFamily: p.textFamily
    readonly property int fontSize: 14
    readonly property int smallSize: 12
    readonly property int tinySize: 11
    readonly property int radius: matrix ? 2 : 8
    readonly property int gap: 12

    function levelColor(level) {
        return level === "error" ? error : level === "warn" ? warn : info
    }
}
