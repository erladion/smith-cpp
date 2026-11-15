import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// The project, its task worktrees and its sessions: start a new session,
// switch, rename the current one (double-click), delete others; "all folders"
// lists every project's.
Rectangle {
    id: sidebar

    signal openFolder()
    signal openRecent()

    // Asks before dropping the worktree at `path` (/drop, or "drop" in the list).
    function askDrop(path) {
        const item = Backend.worktrees.find(w => w.path === path)
        if (item) {
            dropConfirm.target = item
            dropConfirm.open()
        }
    }

    color: Theme.surface

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        anchors.rightMargin: 15
        spacing: 12

        // The wordmark.
        ColumnLayout {
            spacing: 0
            Text {
                textFormat: Text.PlainText
                text: Theme.matrix ? "AGENT SMITH" : "Smith"
                color: Theme.accent
                font.family: Theme.mono
                font.pixelSize: 17
                font.bold: true
                font.letterSpacing: Theme.matrix ? 3 : 0
            }
            Text {
                textFormat: Text.PlainText
                text: Theme.matrix ? "ｴｰｼﾞｪﾝﾄ · coding agent" : "coding agent"
                color: Theme.textFaint
                font.family: Theme.mono
                font.pixelSize: Theme.tinySize
            }
        }

        // The project.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: project.implicitHeight + 18
            color: Theme.window
            radius: Theme.radius
            border.color: Theme.border
            ColumnLayout {
                id: project
                anchors.fill: parent
                anchors.margins: 9
                spacing: 2
                Text {
                    textFormat: Text.PlainText
                    text: "PROJECT"
                    color: Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    font.letterSpacing: 1.5
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: Backend.projectName || "…"
                    color: Theme.text
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: Backend.projectPath
                    color: Theme.textFaint
                    font.family: Theme.mono
                    font.pixelSize: Theme.tinySize
                    elide: Text.ElideLeft
                }
                RowLayout {
                    Layout.topMargin: 6
                    spacing: 6
                    SmithButton { text: "Open…"; tip: "Work in another folder (ctrl+o)"; onClicked: sidebar.openFolder() }
                    SmithButton { text: "Recent"; enabled: Backend.recentProjects.length > 0; onClicked: sidebar.openRecent() }
                }
            }
        }

        WorktreeList {
            Layout.fillWidth: true
            visible: Backend.inRepository
            onDrop: item => sidebar.askDrop(item.path)
        }

        SmithButton {
            Layout.fillWidth: true
            text: "+  New session"
            kind: "primary"
            keyHint: "ctrl+n"
            enabled: Backend.state === "ready" && !Backend.busy
            onClicked: Backend.newSession("")
        }

        RowLayout {
            Layout.fillWidth: true
            Text {
                textFormat: Text.PlainText
                text: "SESSIONS"
                color: Theme.textFaint
                font.family: Theme.textFamily
                font.pixelSize: Theme.tinySize
                font.letterSpacing: 1.5
            }
            Item { Layout.fillWidth: true }
            Repeater {
                model: [["this folder", false], ["all", true]]
                Text {
                    textFormat: Text.PlainText
                    required property var modelData
                    text: modelData[0]
                    color: Backend.allSessions === modelData[1] ? Theme.accent : Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    font.underline: Backend.allSessions === modelData[1]
                    TapHandler { onTapped: Backend.allSessions = parent.modelData[1] }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }
            }
        }

        ListView {
            id: sessions
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: Backend.sessions
            ScrollIndicator.vertical: ScrollIndicator {}

            delegate: Rectangle {
                id: entry
                required property var modelData
                property bool renaming: false
                width: sessions.width
                implicitHeight: info.implicitHeight + 14
                radius: Theme.radius
                color: modelData.current ? Theme.accentWash : hover.hovered ? Theme.surfaceRaised : "transparent"

                HoverHandler { id: hover }
                TapHandler {
                    enabled: !entry.modelData.current && !Backend.busy
                    onTapped: Backend.openSession(entry.modelData.id)
                }
                TapHandler {
                    enabled: entry.modelData.current
                    onDoubleTapped: {
                        entry.renaming = true
                        rename.text = entry.modelData.name
                        rename.forceActiveFocus()
                        rename.selectAll()
                    }
                }

                Rectangle {
                    visible: entry.modelData.current
                    width: 2
                    height: parent.height - 8
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.accent
                }

                ColumnLayout {
                    id: info
                    x: 10
                    y: 7
                    width: parent.width - 20 - (remove.visible ? remove.width + 4 : 0)
                    spacing: 1
                    Text {
                        textFormat: Text.PlainText
                        visible: !entry.renaming
                        Layout.fillWidth: true
                        text: entry.modelData.name
                        color: entry.modelData.current ? Theme.accent : Theme.text
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                        font.bold: entry.modelData.current
                        elide: Text.ElideRight
                    }
                    TextField {
                        id: rename
                        visible: entry.renaming
                        Layout.fillWidth: true
                        color: Theme.text
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                        padding: 2
                        background: Rectangle { color: Theme.window; border.color: Theme.accentSoft; radius: Theme.radius }
                        onAccepted: {
                            Backend.renameSession(text)
                            entry.renaming = false
                        }
                        onActiveFocusChanged: if (!activeFocus) entry.renaming = false
                        Keys.onEscapePressed: entry.renaming = false
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: entry.modelData.hint + (Backend.allSessions ? " · " + entry.modelData.folder : "")
                        color: Theme.textFaint
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.tinySize
                        elide: Text.ElideRight
                    }
                }

                Text {
                    textFormat: Text.PlainText
                    id: remove
                    visible: hover.hovered && !entry.modelData.current
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "✕"
                    color: removeHover.hovered ? Theme.error : Theme.textFaint
                    font.pixelSize: Theme.smallSize
                    HoverHandler { id: removeHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            confirm.target = entry.modelData
                            confirm.open()
                        }
                    }
                }
            }

            Text {
                textFormat: Text.PlainText
                visible: sessions.count === 0
                text: "No saved sessions yet."
                color: Theme.textFaint
                font.family: Theme.textFamily
                font.pixelSize: Theme.smallSize
            }
        }

        Text {
            textFormat: Text.PlainText
            text: "smith " + Backend.version + (Backend.sessionId ? "  ·  " + Backend.sessionId : "")
            color: Theme.textFaint
            font.family: Theme.mono
            font.pixelSize: Theme.tinySize
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    Popup {
        id: dropConfirm
        objectName: "dropConfirm"
        property var target: ({})
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        padding: 18
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }
        background: Rectangle { color: Theme.surface; border.color: Theme.error; radius: Theme.radius }
        contentItem: ColumnLayout {
            spacing: 14
            Text {
                textFormat: Text.PlainText
                text: "Drop " + (dropConfirm.target.branch || "") + " and its worktree?"
                color: Theme.text
                font.family: Theme.textFamily
                font.pixelSize: Theme.fontSize
            }
            Text {
                textFormat: Text.PlainText
                text: (dropConfirm.target.summary || "") === "no changes" ? "Nothing in it is unlanded."
                      : "This throws away what isn't landed: " + (dropConfirm.target.summary || "") + "."
                color: (dropConfirm.target.summary || "") === "no changes" ? Theme.textDim : Theme.warn
                font.family: Theme.textFamily
                font.pixelSize: Theme.smallSize
            }
            Text {
                textFormat: Text.PlainText
                text: dropConfirm.target.path || ""
                color: Theme.textFaint
                font.family: Theme.mono
                font.pixelSize: Theme.tinySize
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                SmithButton { text: "Keep it"; onClicked: dropConfirm.close() }
                SmithButton {
                    text: "Drop it"
                    kind: "danger"
                    onClicked: {
                        Backend.removeWorktree(dropConfirm.target.path, true)
                        dropConfirm.close()
                    }
                }
            }
        }
    }

    Popup {
        id: confirm
        property var target: ({})
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        padding: 18
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }
        background: Rectangle { color: Theme.surface; border.color: Theme.error; radius: Theme.radius }
        contentItem: ColumnLayout {
            spacing: 14
            Text {
                textFormat: Text.PlainText
                text: "Delete \"" + (confirm.target.name || "") + "\" and its tool log?"
                color: Theme.text
                font.family: Theme.textFamily
                font.pixelSize: Theme.fontSize
            }
            Text {
                textFormat: Text.PlainText
                text: (confirm.target.hint || "") + "  ·  " + (confirm.target.id || "")
                color: Theme.textFaint
                font.family: Theme.mono
                font.pixelSize: Theme.tinySize
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                SmithButton { text: "Cancel"; onClicked: confirm.close() }
                SmithButton {
                    text: "Delete"
                    kind: "danger"
                    onClicked: {
                        Backend.deleteSession(confirm.target.id)
                        confirm.close()
                    }
                }
            }
        }
    }
}
