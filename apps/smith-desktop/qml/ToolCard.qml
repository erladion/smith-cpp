import QtQuick 2.15
import QtQuick.Layouts 1.15

// A tool call: a status dot, the tool and what it did, then its diff or the
// start of its output (click to see all of it).
Rectangle {
    id: card

    property string name
    property string summary
    property string output
    property int outputLines: 0
    property bool isError: false
    property var diff: []
    property var diffStats: ({})
    property bool expanded: false

    readonly property int previewLines: 4
    readonly property int diffPreviewLines: 16
    readonly property bool hasDiff: diff.length > 0 && !isError
    readonly property bool canExpand: hasDiff ? diff.length > diffPreviewLines : outputLines > previewLines
    readonly property int hiddenLines: hasDiff ? diff.length - diffPreviewLines : outputLines - previewLines

    implicitHeight: column.implicitHeight + 20
    radius: Theme.radius
    color: Theme.surface
    border.color: isError ? Qt.rgba(Theme.error.r, Theme.error.g, Theme.error.b, 0.45) : Theme.border

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 10
        anchors.leftMargin: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: card.isError ? Theme.error : Theme.added
            }
            Text {
                textFormat: Text.PlainText
                text: card.name
                color: Theme.text
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
                font.bold: true
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: card.summary
                color: Theme.textDim
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
                elide: Text.ElideRight
            }
            Text {
                textFormat: Text.PlainText
                visible: card.hasDiff
                text: "+" + (card.diffStats.added || 0)
                color: Theme.added
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
            }
            Text {
                textFormat: Text.PlainText
                visible: card.hasDiff
                text: "−" + (card.diffStats.removed || 0)
                color: Theme.removed
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
            }
        }

        DiffView {
            visible: card.hasDiff
            Layout.fillWidth: true
            rows: card.diff
            maxLines: card.expanded ? 2000 : card.diffPreviewLines
            moreHint: false
        }

        Rectangle {
            visible: !card.hasDiff && card.output.length > 0
            Layout.fillWidth: true
            implicitHeight: outputText.implicitHeight + 12
            color: Theme.window
            radius: Theme.radius

            TextEdit {
                id: outputText
                x: 8
                y: 6
                width: parent.width - 16
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
                color: card.isError ? Theme.error : Theme.textDim
                selectionColor: Theme.selection
                // read_file puts a tab after each line number.
                tabStopDistance: 40
                text: card.expanded ? card.output.split("\n").slice(0, 400).join("\n") : card.output.split("\n").slice(0, card.previewLines).join("\n")
            }
        }

        Text {
            textFormat: Text.PlainText
            visible: card.canExpand
            text: card.expanded ? "▴ show less" : "▾ " + card.hiddenLines + " more " + (card.hasDiff ? "diff " : "") + "line" + (card.hiddenLines === 1 ? "" : "s")
            color: Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.tinySize
            TapHandler { onTapped: card.expanded = !card.expanded }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }
}
