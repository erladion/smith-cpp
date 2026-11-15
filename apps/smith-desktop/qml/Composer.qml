import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// The input box. Enter sends (or, while the agent works, queues the message
// for its next step); shift+enter adds a line; esc interrupts; ↑/↓ in an
// empty box recall earlier input. Typing / shows the commands, as in the TUI.
Rectangle {
    id: composer

    property var history: []
    property int browsing: -1
    property int suggestionIndex: 0
    readonly property var suggestions: input.text.startsWith("/") && !input.text.includes("\n") && !dismissed ? Backend.suggest(input.text) : []
    property bool dismissed: false

    function focusInput() { input.forceActiveFocus() }
    function setText(value) {
        input.text = value
        input.cursorPosition = value.length
        input.forceActiveFocus()
    }

    implicitHeight: column.implicitHeight + 20
    color: Theme.surface
    border.color: input.activeFocus ? Theme.borderStrong : Theme.border
    radius: Theme.radius

    function send() {
        const text = input.text
        if (text.trim().length === 0) {
            return
        }
        if (history.length === 0 || history[history.length - 1] !== text) {
            history.push(text)
        }
        browsing = -1
        input.text = ""
        Backend.submit(text)
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // Messages typed while the agent works, waiting for its next step.
        Repeater {
            model: Backend.queued
            RowLayout {
                id: queuedRow
                required property string modelData
                Layout.fillWidth: true
                spacing: 8
                Text {
                    text: "⧗"
                    color: Theme.textFaint
                    font.pixelSize: Theme.smallSize
                }
                Text {
                    Layout.fillWidth: true
                    text: queuedRow.modelData
                    elide: Text.ElideRight
                    color: Theme.textDim
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.smallSize
                }
                Text {
                    text: "queued for the next step"
                    color: Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    font.italic: true
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Text {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 6
                text: "›"
                color: Theme.accent
                font.family: Theme.mono
                font.pixelSize: 16
                font.bold: true
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Math.max(input.implicitHeight, 34), 200)

                TextArea {
                    id: input
                    enabled: Backend.state === "ready"
                    wrapMode: TextArea.Wrap
                    placeholderText: Backend.agentRunning ? "Type to add to what the agent is doing (enter queues it)"
                                                          : "Ask for something, or type / for commands"
                    placeholderTextColor: Theme.textFaint
                    color: Theme.text
                    selectionColor: Theme.selection
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.fontSize
                    background: null
                    padding: 6

                    onTextChanged: {
                        composer.dismissed = false
                        composer.suggestionIndex = 0
                    }

                    Keys.onPressed: event => {
                        const suggestions = composer.suggestions
                        if (suggestions.length > 0 && !(event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))) {
                            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                                const step = event.key === Qt.Key_Up ? suggestions.length - 1 : 1
                                composer.suggestionIndex = (composer.suggestionIndex + step) % suggestions.length
                                event.accepted = true
                                return
                            }
                            if (event.key === Qt.Key_Tab) {
                                const chosen = suggestions[Math.min(composer.suggestionIndex, suggestions.length - 1)]
                                const takesArgs = !chosen.text.includes(" ") && /[\[|]/.test(chosen.label)
                                composer.setText(chosen.text + (takesArgs ? " " : ""))
                                event.accepted = true
                                return
                            }
                            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                input.text = suggestions[Math.min(composer.suggestionIndex, suggestions.length - 1)].text
                                composer.send()
                                event.accepted = true
                                return
                            }
                            if (event.key === Qt.Key_Escape) {
                                composer.dismissed = true
                                event.accepted = true
                                return
                            }
                        }
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
                            composer.send()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape && Backend.busy) {
                            Backend.interrupt()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up && (input.text.length === 0 || composer.browsing >= 0) && composer.history.length > 0) {
                            composer.browsing = composer.browsing < 0 ? composer.history.length - 1 : Math.max(0, composer.browsing - 1)
                            input.text = composer.history[composer.browsing]
                            event.accepted = true
                        } else if (event.key === Qt.Key_Down && composer.browsing >= 0) {
                            composer.browsing++
                            input.text = composer.browsing < composer.history.length ? composer.history[composer.browsing] : ""
                            if (composer.browsing >= composer.history.length) {
                                composer.browsing = -1
                            }
                            event.accepted = true
                        }
                    }
                }
            }

            SmithButton {
                Layout.alignment: Qt.AlignBottom
                visible: !Backend.busy || Backend.agentRunning
                text: Backend.agentRunning ? (input.text.trim().length > 0 ? "Queue" : "Stop") : "Send"
                kind: Backend.agentRunning && input.text.trim().length === 0 ? "danger" : "primary"
                keyHint: Backend.agentRunning && input.text.trim().length === 0 ? "esc" : "⏎"
                enabled: Backend.state === "ready"
                onClicked: {
                    if (Backend.agentRunning && input.text.trim().length === 0) {
                        Backend.interrupt()
                    } else {
                        composer.send()
                    }
                }
            }
        }
    }

    // Command suggestions, above the box.
    Rectangle {
        visible: composer.suggestions.length > 0 && input.activeFocus
        anchors.bottom: parent.top
        anchors.bottomMargin: 6
        anchors.left: parent.left
        width: Math.min(parent.width, 640)
        height: suggestionList.contentHeight + 34
        color: Theme.surfaceRaised
        border.color: Theme.borderStrong
        radius: Theme.radius

        ListView {
            id: suggestionList
            anchors.fill: parent
            anchors.margins: 4
            anchors.bottomMargin: 28
            interactive: false
            model: composer.suggestions.slice(0, 8)
            delegate: Rectangle {
                id: suggestion
                required property var modelData
                required property int index
                width: suggestionList.width
                height: 26
                color: composer.suggestionIndex === index ? Theme.accentWash : "transparent"
                radius: Theme.radius
                Row {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12
                    Text {
                        width: 180
                        text: suggestion.modelData.label
                        color: composer.suggestionIndex === suggestion.index ? Theme.accent : Theme.text
                        font.family: Theme.mono
                        font.pixelSize: Theme.smallSize
                        font.bold: composer.suggestionIndex === suggestion.index
                        elide: Text.ElideRight
                    }
                    Text {
                        text: suggestion.modelData.hint
                        color: Theme.textFaint
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.tinySize
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                TapHandler {
                    onTapped: {
                        composer.suggestionIndex = suggestion.index
                        composer.setText(suggestion.modelData.text + (suggestion.modelData.label.includes("[") ? " " : ""))
                    }
                }
            }
        }
        Text {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 8
            text: "tab completes · enter runs · esc hides"
            color: Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.tinySize
        }
    }
}
