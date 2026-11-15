import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// The bottom line: where commands run, how full the context is, background
// jobs, and (matrix theme, idle) a slow stream of glyphs at the right end.
Rectangle {
    id: bar

    signal showJobs()

    property int streamOffset: 0
    readonly property bool streaming: Theme.matrix && Backend.codeStream && !Backend.busy && Backend.state === "ready"
    readonly property real fill: Backend.contextLimit > 0 ? Math.min(1, Backend.contextTokens / Backend.contextLimit) : 0

    implicitHeight: 28
    color: Theme.surface

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.border
    }

    function k(n) {
        return n >= 1024 ? Math.round(n / 1024) + "k" : String(n)
    }

    Timer {
        interval: 320
        repeat: true
        running: bar.streaming
        onTriggered: bar.streamOffset++
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 14

        Text {
            visible: Backend.worktreeBranch.length > 0
            text: "⎇ " + Backend.worktreeBranch
            color: Theme.warn
            font.family: Theme.mono
            font.pixelSize: Theme.tinySize
            font.bold: true
            ToolTip.visible: branchHover.hovered
            ToolTip.text: "A task worktree: the agent's changes stay on this branch until you land them"
            HoverHandler { id: branchHover }
        }

        Text {
            text: Backend.sandboxActive ? "◉ " + (Backend.sandboxKind === "container" ? "container" : "sandbox") + " · net " + (Backend.network ? "on" : "off")
                                        : "⚠ unsandboxed"
            color: Backend.sandboxActive ? Theme.added : Theme.error
            font.family: Theme.mono
            font.pixelSize: Theme.tinySize
            ToolTip.visible: sandboxHover.hovered
            ToolTip.text: Backend.sandboxDescription
            HoverHandler { id: sandboxHover }
        }

        // Context use: a thin meter and the numbers.
        Row {
            visible: Backend.contextTokens > 0
            spacing: 8
            Rectangle {
                visible: Backend.contextLimit > 0
                width: 90
                height: 4
                radius: 2
                color: Theme.border
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    width: parent.width * bar.fill
                    height: parent.height
                    radius: 2
                    color: bar.fill > 0.75 ? Theme.warn : Theme.accentSoft
                    Behavior on width { NumberAnimation { duration: 250 } }
                }
            }
            Text {
                text: Backend.contextLimit > 0 ? Math.round(100 * bar.fill) + "% of " + bar.k(Backend.contextLimit) + " context"
                                               : bar.k(Backend.contextTokens) + " tokens"
                color: Theme.textDim
                font.family: Theme.mono
                font.pixelSize: Theme.tinySize
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Text {
            visible: Backend.jobsRunning > 0
            text: Backend.jobsRunning + " background job" + (Backend.jobsRunning === 1 ? "" : "s")
            color: Theme.warn
            font.family: Theme.mono
            font.pixelSize: Theme.tinySize
            font.underline: jobsHover.hovered
            HoverHandler { id: jobsHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: bar.showJobs() }
        }

        Text {
            visible: Backend.busy
            text: Backend.agentRunning ? "esc interrupts · enter queues" : "working…"
            color: Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.tinySize
        }

        Item { Layout.fillWidth: true }

        Text {
            visible: bar.streaming
            text: Backend.codeStreamText(bar.streamOffset, 28)
            color: Theme.borderStrong
            font.family: Theme.mono
            font.pixelSize: Theme.tinySize
        }
    }
}
