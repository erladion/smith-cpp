import QtQuick 2.15
import Smith.Desktop 1.0

// The TUI's spinner: half-width katakana with the matrix theme, braille dots otherwise.
Text {
    id: spinner

    property bool running: true
    property int frame: 0

    text: Backend.spinnerFrames[frame % Backend.spinnerFrames.length]
    color: Theme.matrix ? Theme.accent : Theme.warn
    font.family: Theme.mono
    font.pixelSize: Theme.fontSize
    font.bold: true

    Timer {
        interval: 80
        repeat: true
        running: spinner.running && spinner.visible
        onTriggered: spinner.frame++
    }
}
