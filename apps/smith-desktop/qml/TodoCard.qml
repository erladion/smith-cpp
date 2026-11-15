import QtQuick 2.15
import QtQuick.Layouts 1.15

// The agent's checklist after it changed: done steps dim, the current one bold.
Rectangle {
    id: card

    property var todos: []

    readonly property int done: todos.filter(t => t.status === "completed").length

    implicitHeight: column.implicitHeight + 20
    radius: Theme.radius
    color: Theme.surface
    border.color: Theme.border

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 10
        anchors.leftMargin: 12
        spacing: 5

        RowLayout {
            spacing: 8
            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: Theme.added
            }
            Text {
                textFormat: Text.PlainText
                text: "Todos"
                color: Theme.text
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
                font.bold: true
            }
            Text {
                textFormat: Text.PlainText
                text: card.todos.length === 0 ? "cleared" : card.done + "/" + card.todos.length + " done"
                color: Theme.textDim
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
            }
            // Progress, as a thin bar.
            Rectangle {
                visible: card.todos.length > 0
                Layout.preferredWidth: 80
                height: 3
                radius: 1
                color: Theme.border
                Rectangle {
                    width: parent.width * card.done / Math.max(1, card.todos.length)
                    height: parent.height
                    radius: 1
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: 200 } }
                }
            }
        }

        Repeater {
            model: card.todos
            RowLayout {
                id: item
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 4
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    Layout.alignment: Qt.AlignTop
                    text: item.modelData.status === "completed" ? "✔" : item.modelData.status === "in_progress" ? "▶" : "○"
                    color: item.modelData.status === "completed" ? Theme.added : item.modelData.status === "in_progress" ? Theme.warn : Theme.textFaint
                    font.family: Theme.mono
                    font.pixelSize: Theme.smallSize
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: item.modelData.content
                    color: item.modelData.status === "completed" ? Theme.textFaint : Theme.text
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.smallSize
                    font.bold: item.modelData.status === "in_progress"
                    font.strikeout: item.modelData.status === "completed" && !Theme.matrix
                }
            }
        }
    }
}
