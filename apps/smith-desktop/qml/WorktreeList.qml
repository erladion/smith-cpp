import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// The repository's checkouts: yours and the task worktrees, where an agent
// works on a smith/<task> branch of its own. Click one to work in it; hover a
// task for land and drop; "+ task" makes a new one.
ColumnLayout {
    id: list

    signal drop(var item)

    property bool adding: false
    readonly property bool idle: Backend.state === "ready" && !Backend.busy

    spacing: 2

    RowLayout {
        Layout.fillWidth: true
        Text {
            textFormat: Text.PlainText
            text: "WORKTREES"
            color: Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.tinySize
            font.letterSpacing: 1.5
        }
        Item { Layout.fillWidth: true }
        Text {
            textFormat: Text.PlainText
            objectName: "newTask"
            text: "+ task"
            color: !list.idle ? Theme.textFaint : addHover.hovered ? Theme.accent : Theme.textDim
            font.family: Theme.textFamily
            font.pixelSize: Theme.tinySize
            HoverHandler { id: addHover; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                enabled: list.idle
                onTapped: {
                    list.adding = true
                    taskName.forceActiveFocus()
                }
            }
            ToolTip.visible: addHover.hovered
            ToolTip.text: "A worktree for a task: the agent works on its own branch while you keep working here"
            ToolTip.delay: 600
        }
    }

    TextField {
        id: taskName
        objectName: "taskName"
        visible: list.adding
        Layout.fillWidth: true
        Layout.topMargin: 2
        placeholderText: "task name, e.g. fix-login"
        placeholderTextColor: Theme.textFaint
        color: Theme.text
        font.family: Theme.mono
        font.pixelSize: Theme.smallSize
        padding: 6
        background: Rectangle { color: Theme.window; border.color: Theme.accentSoft; radius: Theme.radius }
        onAccepted: {
            if (text.trim().length > 0) {
                Backend.createWorktree(text.trim())
            }
            text = ""
            list.adding = false
        }
        Keys.onEscapePressed: {
            text = ""
            list.adding = false
        }
        onActiveFocusChanged: if (!activeFocus && text.length === 0) list.adding = false
    }

    Repeater {
        model: Backend.worktrees.length > 1 ? Backend.worktrees : []

        Rectangle {
            id: row
            required property var modelData
            Layout.fillWidth: true
            implicitHeight: info.implicitHeight + 12
            radius: Theme.radius
            color: modelData.current ? Theme.accentWash : hover.hovered ? Theme.surfaceRaised : "transparent"

            HoverHandler { id: hover }
            TapHandler {
                enabled: !row.modelData.current && list.idle
                onTapped: Backend.switchToWorktree(row.modelData.path)
            }

            Rectangle {
                visible: row.modelData.current
                width: 2
                height: parent.height - 8
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.accent
            }

            ColumnLayout {
                id: info
                x: 10
                y: 6
                width: parent.width - 20 - (actions.visible ? actions.width + 6 : 0)
                spacing: 1
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: (row.modelData.main ? "" : "⎇ ") + row.modelData.label
                    color: row.modelData.current ? Theme.accent : Theme.text
                    font.family: Theme.mono
                    font.pixelSize: Theme.smallSize
                    font.bold: row.modelData.current
                    elide: Text.ElideRight
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: row.modelData.hint
                    color: row.modelData.openElsewhere ? Theme.warn : Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    elide: Text.ElideRight
                }
            }

            Row {
                id: actions
                visible: hover.hovered && !row.modelData.main && !row.modelData.openElsewhere && list.idle
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                Text {
                    textFormat: Text.PlainText
                    text: "land"
                    color: landHover.hovered ? Theme.accent : Theme.textDim
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    HoverHandler { id: landHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: Backend.reviewLanding(row.modelData.path) }
                }
                Text {
                    textFormat: Text.PlainText
                    text: "drop"
                    color: dropHover.hovered ? Theme.error : Theme.textDim
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    HoverHandler { id: dropHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: list.drop(row.modelData) }
                }
            }
        }
    }

    Text {
        textFormat: Text.PlainText
        visible: Backend.worktrees.length <= 1 && !list.adding
        Layout.fillWidth: true
        text: "None yet. With one, the agent works on its own branch while you keep working here."
        color: Theme.textFaint
        font.family: Theme.textFamily
        font.pixelSize: Theme.tinySize
        wrapMode: Text.WordWrap
    }
}
