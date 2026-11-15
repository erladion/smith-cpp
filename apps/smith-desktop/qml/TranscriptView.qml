import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Qt.labs.qmlmodels 1.0
import Smith.Desktop 1.0

// The conversation: one card per entry, following new output unless you scrolled up.
ListView {
    id: view

    property bool follow: true
    property bool scrolling: false

    model: Backend.transcript
    clip: true
    spacing: 14
    topMargin: 18
    bottomMargin: 18
    boundsBehavior: Flickable.StopAtBounds
    reuseItems: false
    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
        contentItem: Rectangle {
            implicitWidth: 6
            radius: 3
            color: Theme.borderStrong
            opacity: 0.8
        }
    }

    function toEnd() {
        scrolling = true
        positionViewAtEnd()
        scrolling = false
        follow = true
    }

    onContentYChanged: if (!scrolling && (moving || dragging)) follow = atYEnd
    onMovementEnded: follow = atYEnd
    onCountChanged: if (follow) Qt.callLater(toEnd)
    onContentHeightChanged: if (follow) Qt.callLater(toEnd)

    WheelHandler {
        onWheel: event => {
            if (event.angleDelta.y > 0) {
                view.follow = false
            } else {
                Qt.callLater(() => view.follow = view.atYEnd)
            }
        }
    }

    readonly property int column: Math.min(width - 48, 980)

    delegate: DelegateChooser {
        role: "kind"

        DelegateChoice {
            roleValue: "banner"
            delegate: Item {
                required property var banner
                width: ListView.view.width
                implicitHeight: card.implicitHeight
                BannerCard {
                    id: card
                    banner: parent.banner
                    width: view.column
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }

        DelegateChoice {
            roleValue: "user"
            delegate: Item {
                id: userRow
                required property string text
                width: ListView.view.width
                implicitHeight: bubble.implicitHeight + 6
                Rectangle {
                    id: bubble
                    x: (parent.width - view.column) / 2
                    y: 6
                    width: view.column
                    implicitHeight: message.implicitHeight + 20
                    color: Theme.userWash
                    radius: Theme.radius
                    border.color: Theme.matrix ? Theme.border : "transparent"
                    Rectangle {
                        width: 3
                        height: parent.height
                        radius: Theme.radius
                        color: Theme.accent
                    }
                    Text {
                        textFormat: Text.PlainText
                        id: prompt
                        x: 14
                        y: 10
                        text: "›"
                        color: Theme.accent
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize
                        font.bold: true
                    }
                    TextEdit {
                        id: message
                        x: 32
                        y: 10
                        width: parent.width - 46
                        text: userRow.text
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        textFormat: TextEdit.PlainText
                        color: Theme.text
                        selectionColor: Theme.selection
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.fontSize
                    }
                }
            }
        }

        DelegateChoice {
            roleValue: "text"
            delegate: Item {
                id: textRow
                required property string html
                required property bool live
                width: ListView.view.width
                implicitHeight: body.implicitHeight
                TextEdit {
                    id: body
                    x: (parent.width - view.column) / 2 + 4
                    width: view.column - 8
                    text: textRow.html
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.RichText
                    color: Theme.text
                    selectionColor: Theme.selection
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.fontSize
                }
                Rectangle {
                    visible: textRow.live
                    width: 8
                    height: 15
                    color: Theme.accent
                    x: body.x + body.positionToRectangle(body.length).x + 2
                    y: body.positionToRectangle(body.length).y + 2
                    SequentialAnimation on opacity {
                        loops: Animation.Infinite
                        running: textRow.live
                        NumberAnimation { to: 0.1; duration: 450 }
                        NumberAnimation { to: 1; duration: 450 }
                    }
                }
            }
        }

        DelegateChoice {
            roleValue: "reasoning"
            delegate: Item {
                id: reasoningRow
                required property string text
                required property bool live
                property bool open: live
                width: ListView.view.width
                implicitHeight: thought.implicitHeight
                ColumnLayout {
                    id: thought
                    x: (parent.width - view.column) / 2 + 4
                    width: view.column - 8
                    spacing: 4
                    Text {
                        textFormat: Text.PlainText
                        text: (reasoningRow.open ? "▾ " : "▸ ") + (reasoningRow.live ? "thinking…" : "thought")
                        color: Theme.textFaint
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.tinySize
                        font.letterSpacing: 1
                        TapHandler { onTapped: reasoningRow.open = !reasoningRow.open }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: reasoningRow.open
                        text: reasoningRow.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                        font.italic: true
                        color: Theme.textDim
                        leftPadding: 10
                        Rectangle {
                            width: 1
                            height: parent.height
                            color: Theme.border
                        }
                    }
                }
            }
        }

        DelegateChoice {
            roleValue: "tool"
            delegate: Item {
                id: toolRow
                required property string name
                required property string summary
                required property string output
                required property int outputLines
                required property bool isError
                required property var diff
                required property var diffStats
                width: ListView.view.width
                implicitHeight: card.implicitHeight
                ToolCard {
                    id: card
                    x: (parent.width - view.column) / 2
                    width: view.column
                    name: toolRow.name
                    summary: toolRow.summary
                    output: toolRow.output
                    outputLines: toolRow.outputLines
                    isError: toolRow.isError
                    diff: toolRow.diff
                    diffStats: toolRow.diffStats
                }
            }
        }

        DelegateChoice {
            roleValue: "todos"
            delegate: Item {
                id: todoRow
                required property var todos
                width: ListView.view.width
                implicitHeight: card.implicitHeight
                TodoCard {
                    id: card
                    x: (parent.width - view.column) / 2
                    width: view.column
                    todos: todoRow.todos
                }
            }
        }

        DelegateChoice {
            roleValue: "notice"
            delegate: Item {
                id: noticeRow
                required property string text
                required property string level
                width: ListView.view.width
                implicitHeight: box.implicitHeight
                Rectangle {
                    id: box
                    x: (parent.width - view.column) / 2
                    width: view.column
                    implicitHeight: noticeText.implicitHeight + 16
                    radius: Theme.radius
                    color: Qt.rgba(Theme.levelColor(noticeRow.level).r, Theme.levelColor(noticeRow.level).g, Theme.levelColor(noticeRow.level).b, 0.07)
                    border.color: Qt.rgba(Theme.levelColor(noticeRow.level).r, Theme.levelColor(noticeRow.level).g, Theme.levelColor(noticeRow.level).b, 0.35)
                    Text {
                        id: noticeText
                        x: 12
                        y: 8
                        width: parent.width - 24
                        text: noticeRow.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.levelColor(noticeRow.level)
                        font.family: noticeRow.text.indexOf("\n  /") >= 0 ? Theme.mono : Theme.textFamily
                        font.pixelSize: Theme.smallSize
                    }
                }
            }
        }

        DelegateChoice {
            roleValue: "meta"
            delegate: Item {
                required property string text
                width: ListView.view.width
                implicitHeight: metaText.implicitHeight
                Text {
                    textFormat: Text.PlainText
                    id: metaText
                    x: (parent.width - view.column) / 2 + 4
                    text: "— " + parent.text
                    color: Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                }
            }
        }

        DelegateChoice {
            roleValue: "quote"
            delegate: Item {
                required property string text
                width: ListView.view.width
                implicitHeight: quoteText.implicitHeight
                Text {
                    textFormat: Text.PlainText
                    id: quoteText
                    x: (parent.width - view.column) / 2 + 4
                    text: parent.text
                    color: Theme.accentSoft
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.smallSize
                    font.italic: true
                }
            }
        }
    }

    // Back to the newest output, when you've scrolled away from it.
    SmithButton {
        visible: !view.follow && view.count > 0
        text: "↓ latest"
        kind: "primary"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        onClicked: view.toEnd()
    }
}
