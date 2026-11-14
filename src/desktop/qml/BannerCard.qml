import QtQuick
import QtQuick.Layouts

// The session's opening card: Agent Smith's greeting, and what this session runs on.
Rectangle {
    id: card

    property var banner: ({})

    implicitHeight: content.implicitHeight + 32
    radius: Theme.radius
    color: Theme.surface
    border.color: Theme.matrix ? Theme.borderStrong : Theme.border

    // A thin bar of glyphs across the top, like a rain drop caught mid-fall.
    Text {
        visible: Theme.matrix
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        text: "ｱｶﾞﾝﾄ ｽﾐｽ"
        color: Theme.textFaint
        font.family: Theme.mono
        font.pixelSize: Theme.smallSize
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 16
        spacing: 4

        Text {
            text: Theme.matrix ? "AGENT SMITH" : "Smith"
            color: Theme.accent
            font.family: Theme.mono
            font.pixelSize: 18
            font.bold: true
            font.letterSpacing: Theme.matrix ? 4 : 0
        }

        Text {
            visible: (card.banner.greeting || "").length > 0
            text: card.banner.greeting || ""
            color: Theme.accentSoft
            font.family: Theme.textFamily
            font.pixelSize: Theme.fontSize
            font.italic: true
            Layout.bottomMargin: 8
        }

        GridLayout {
            columns: 2
            columnSpacing: 16
            rowSpacing: 3
            Layout.fillWidth: true

            Repeater {
                model: [
                    ["model", card.banner.model],
                    ["server", card.banner.server],
                    ["folder", card.banner.cwd],
                    ["session", (card.banner.session || "") + (card.banner.resumed ? "  (resumed)" : "")],
                    ["rules", (card.banner.instructions || []).join(", ")],
                    ["skills", (card.banner.skills || []).join(", ")],
                ].filter(row => row[1] && row[1].length > 0)

                delegate: Item {
                    id: field
                    required property var modelData
                    required property int index
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    implicitHeight: value.implicitHeight
                    Text {
                        id: key
                        width: 72
                        text: field.modelData[0]
                        color: Theme.textFaint
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                    }
                    Text {
                        id: value
                        x: 72
                        width: parent.width - 72
                        text: field.modelData[1]
                        color: Theme.textDim
                        font.family: Theme.mono
                        font.pixelSize: Theme.smallSize
                        elide: Text.ElideMiddle
                    }
                }
            }
        }

        Text {
            Layout.topMargin: 8
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: card.banner.sandboxActive ? "✓ " + card.banner.sandboxDescription : "⚠ shell commands are " + card.banner.sandboxDescription
            color: card.banner.sandboxActive ? Theme.added : Theme.error
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
        }
    }
}
