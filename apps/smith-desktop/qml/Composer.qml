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
    // Commands while typing one; files for an @ mention at the end of a message.
    readonly property var suggestions: !dismissed && (input.text.startsWith("/") || input.text.includes("@")) ? Backend.suggest(input.text, Backend.filesListed) : []
    property bool dismissed: false

    function focusInput() { input.forceActiveFocus() }
    function setText(value) {
        input.text = value
        input.cursorPosition = value.length
        input.forceActiveFocus()
    }
    function appendText(value) {
        setText(input.text.trim().length > 0 ? input.text.trim() + "\n\n" + value : value)
    }
    // Tab while the agent works: the next task, started once this one is done.
    function queueNext() {
        const text = input.text
        if (text.trim().length === 0) {
            return
        }
        input.text = ""
        Backend.queueNext(text)
    }

    implicitHeight: column.implicitHeight + 20
    color: Theme.surface
    border.color: Backend.planning ? Theme.accent : input.activeFocus ? Theme.borderStrong : Theme.border
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
                    textFormat: Text.PlainText
                    text: "⧗"
                    color: Theme.textFaint
                    font.pixelSize: Theme.smallSize
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: queuedRow.modelData
                    elide: Text.ElideRight
                    color: Theme.textDim
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.smallSize
                }
                Text {
                    textFormat: Text.PlainText
                    text: "queued for the next step"
                    color: Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    font.italic: true
                }
            }
        }

        // Tasks queued with tab, each started once the one before it is done. Click one to edit it.
        Repeater {
            objectName: "nextTasks"
            model: Backend.nextTasks
            RowLayout {
                id: nextRow
                required property string modelData
                required property int index
                Layout.fillWidth: true
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    text: (nextRow.index + 1) + "."
                    color: Theme.warn
                    font.family: Theme.mono
                    font.pixelSize: Theme.smallSize
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: nextRow.modelData
                    elide: Text.ElideRight
                    color: nextHover.hovered ? Theme.text : Theme.textDim
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.smallSize
                    HoverHandler { id: nextHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: Backend.editNext(nextRow.index) }
                    SmithToolTip {
                        visible: nextHover.hovered
                        text: "Take it back into the input box to change it"
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    text: nextRow.index === 0 ? "next, when this is done" : "after that"
                    color: Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    font.italic: true
                }
                Text {
                    textFormat: Text.PlainText
                    text: "×"
                    color: dropHover.hovered ? Theme.error : Theme.textFaint
                    font.pixelSize: Theme.fontSize
                    HoverHandler { id: dropHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: Backend.dropNext(nextRow.index) }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Text {
                textFormat: Text.PlainText
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
                    objectName: "composerInput"
                    enabled: Backend.state === "ready"
                    wrapMode: TextArea.Wrap
                    placeholderText: Backend.agentRunning ? "Add to what the agent is doing (enter), or queue the next task (tab)"
                                     : Backend.planning ? "Ask for a plan: the agent reads, changes nothing (shift+tab ends plan mode)"
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
                            const highlighted = suggestions[Math.min(composer.suggestionIndex, suggestions.length - 1)]
                            if (highlighted.insert && (event.key === Qt.Key_Tab || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                                composer.setText(highlighted.text)
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
                        if (event.key === Qt.Key_Tab && Backend.agentRunning && input.text.trim().length > 0) {
                            composer.queueNext()
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Backtab) {
                            if (!Backend.busy) {
                                Backend.setPlanning(!Backend.planning)
                            }
                            event.accepted = true
                            return
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
                text: Backend.agentRunning ? (input.text.trim().length > 0 ? "Add" : "Stop") : "Send"
                tip: Backend.agentRunning && input.text.trim().length > 0 ? "Give this to the agent at its next step" : ""
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

    // Command or file suggestions, above the box: eight rows, scrolling to the highlighted one.
    Rectangle {
        objectName: "suggestions"
        readonly property bool files: composer.suggestions.length > 0 && composer.suggestions[0].insert === true
        visible: composer.suggestions.length > 0 && input.activeFocus
        anchors.bottom: parent.top
        anchors.bottomMargin: 6
        anchors.left: parent.left
        width: Math.min(parent.width, 640)
        height: Math.min(composer.suggestions.length, 8) * 26 + 34
        color: Theme.surfaceRaised
        border.color: Theme.borderStrong
        radius: Theme.radius

        ListView {
            id: suggestionList
            anchors.fill: parent
            anchors.margins: 4
            anchors.bottomMargin: 28
            clip: true
            interactive: false
            model: composer.suggestions
            currentIndex: Math.min(composer.suggestionIndex, count - 1)
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
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
                        textFormat: Text.PlainText
                        // A file's path takes the row, and keeps its end (the name) when it's long.
                        width: suggestion.modelData.insert ? suggestion.width - 16 : 180
                        text: suggestion.modelData.label
                        color: composer.suggestionIndex === suggestion.index ? Theme.accent : Theme.text
                        font.family: Theme.mono
                        font.pixelSize: Theme.smallSize
                        font.bold: composer.suggestionIndex === suggestion.index
                        elide: suggestion.modelData.insert ? Text.ElideLeft : Text.ElideRight
                    }
                    Text {
                        textFormat: Text.PlainText
                        // What's left of the row, so a long hint ends in … inside the box.
                        visible: !suggestion.modelData.insert
                        width: suggestion.width - 8 - 180 - 12 - 8
                        text: suggestion.modelData.hint
                        color: Theme.textFaint
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.tinySize
                        elide: Text.ElideRight
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
            textFormat: Text.PlainText
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 8
            text: (parent.files && composer.suggestions.length > 8 ? composer.suggestions.length + " files · " : "")
                  + (parent.files ? "tab or enter attaches it · esc hides" : "tab completes · enter runs · esc hides")
            color: Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.tinySize
        }
    }
}
