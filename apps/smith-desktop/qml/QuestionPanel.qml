import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// The agent's question (ask_user): pick a suggested answer (click, ↑↓ and
// enter, or its number), or type your own. Esc dismisses it and interrupts.
Rectangle {
    id: panel

    readonly property var asked: Backend.question
    readonly property var options: asked.options || []
    property int selected: 0

    implicitHeight: column.implicitHeight + 28
    color: Theme.surface
    radius: Theme.radius
    border.color: Theme.accentSoft

    onVisibleChanged: if (visible) {
        selected = 0
        answer.text = ""
        answer.forceActiveFocus()
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        RowLayout {
            spacing: 10
            Text {
                text: "?"
                color: Theme.accent
                font.family: Theme.mono
                font.pixelSize: 16
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: panel.asked.question || ""
                color: Theme.text
                font.family: Theme.textFamily
                font.pixelSize: Theme.fontSize
                font.bold: true
            }
        }

        Repeater {
            model: panel.options
            Rectangle {
                id: option
                required property string modelData
                required property int index
                Layout.fillWidth: true
                implicitHeight: label.implicitHeight + 14
                radius: Theme.radius
                color: panel.selected === index ? Theme.accentWash : hover.hovered ? Theme.surfaceRaised : "transparent"
                border.color: panel.selected === index ? Theme.accentSoft : Theme.border
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: Backend.answerOption(option.index) }
                Row {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Text {
                        text: (option.index + 1) + "."
                        color: Theme.textFaint
                        font.family: Theme.mono
                        font.pixelSize: Theme.smallSize
                    }
                    Text {
                        id: label
                        text: option.modelData
                        color: panel.selected === option.index ? Theme.accent : Theme.text
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                        font.bold: panel.selected === option.index
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextField {
                id: answer
                Layout.fillWidth: true
                placeholderText: panel.options.length > 0 ? "Or type your own answer" : "Your answer"
                placeholderTextColor: Theme.textFaint
                color: Theme.text
                selectionColor: Theme.selection
                font.family: Theme.textFamily
                font.pixelSize: Theme.smallSize
                background: Rectangle {
                    implicitHeight: 32
                    color: Theme.window
                    radius: Theme.radius
                    border.color: answer.activeFocus ? Theme.accentSoft : Theme.border
                }
                Keys.onPressed: event => {
                    const typing = answer.text.length > 0
                    const n = panel.options.length
                    if (event.key === Qt.Key_Escape) {
                        Backend.interrupt()
                    } else if (!typing && n > 0 && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                        panel.selected = (panel.selected + (event.key === Qt.Key_Up ? n - 1 : 1)) % n
                    } else if (!typing && event.text >= "1" && event.text <= "9" && Number(event.text) <= n) {
                        Backend.answerOption(Number(event.text) - 1)
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (typing) {
                            Backend.answerText(answer.text)
                        } else if (n > 0) {
                            Backend.answerOption(panel.selected)
                        }
                    } else {
                        return
                    }
                    event.accepted = true
                }
            }
            SmithButton { text: "Answer"; kind: "primary"; enabled: answer.text.trim().length > 0; onClicked: Backend.answerText(answer.text) }
            SmithButton { text: "Dismiss"; keyHint: "esc"; onClicked: Backend.interrupt() }
        }
    }
}
