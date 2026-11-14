import QtQuick
import QtQuick.Layouts
import Smith.Desktop

// What the agent is doing right now: the spinner (or, while compacting with
// the matrix theme, a strip of rain), how long the turn has run, and the
// checklist step it's on.
Item {
    id: bar

    property real now: Date.now()
    readonly property var current: Backend.todos.find(t => t.status === "in_progress")
    readonly property int done: Backend.todos.filter(t => t.status === "completed").length

    implicitHeight: visible ? row.implicitHeight + 8 : 0

    Timer {
        interval: 200
        repeat: true
        running: bar.visible && Backend.turnStartedAt > 0
        onTriggered: bar.now = Date.now()
    }

    function elapsed(ms) {
        if (ms < 1000) return Math.max(1, Math.round(ms)) + "ms"
        const s = ms / 1000
        if (s < 10) return s.toFixed(1) + "s"
        if (s < 60) return Math.floor(s) + "s"
        const m = Math.floor(s / 60)
        return m + "m " + String(Math.floor(s % 60)).padStart(2, "0") + "s"
    }

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        RainItem {
            visible: Backend.compacting && Theme.matrix
            Layout.preferredWidth: 150
            Layout.preferredHeight: 36
            endless: true
            running: visible
            glyphSize: 11
            fontFamily: Theme.mono
            headColor: Theme.text
            brightColor: Theme.accent
            color: Theme.accentSoft
            fadedColor: Theme.borderStrong
        }

        Spinner {
            visible: !(Backend.compacting && Theme.matrix)
            running: bar.visible
        }

        Text {
            text: Backend.compacting && Theme.matrix ? "Purging old memories…" : Backend.activity
            color: Theme.textDim
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
            elide: Text.ElideRight
            Layout.maximumWidth: bar.width * 0.6
        }

        Text {
            visible: Backend.turnStartedAt > 0
            text: "· " + bar.elapsed(bar.now - Backend.turnStartedAt)
            color: Theme.textFaint
            font.family: Theme.mono
            font.pixelSize: Theme.smallSize
        }

        Item { Layout.fillWidth: true }

        Text {
            visible: bar.current !== undefined
            text: "▶ " + (bar.done + 1) + "/" + Backend.todos.length
            color: Theme.warn
            font.family: Theme.mono
            font.pixelSize: Theme.smallSize
        }
        Text {
            visible: bar.current !== undefined
            text: bar.current ? bar.current.content : ""
            color: Theme.textDim
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
            elide: Text.ElideRight
            Layout.maximumWidth: bar.width * 0.35
        }
    }
}
