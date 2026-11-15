import QtQuick 2.15
import QtQuick.Layouts 1.15

// A diff with line numbers: green additions, red removals, dim context,
// and "⋯ n unchanged lines" between hunks. Shows at most `maxLines`.
Rectangle {
    id: view

    property var rows: []
    property int maxLines: 40

    readonly property int numberWidth: {
        let largest = 1
        for (const row of rows) {
            largest = Math.max(largest, row.number)
        }
        return String(largest).length
    }

    implicitHeight: column.implicitHeight + 12
    color: Theme.window
    radius: Theme.radius
    border.color: Theme.border

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 6
        spacing: 0

        Repeater {
            model: view.rows.slice(0, view.maxLines)

            Rectangle {
                id: line
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: content.implicitHeight + 2
                color: modelData.kind === "add" ? Theme.addedWash : modelData.kind === "remove" ? Theme.removedWash : "transparent"

                Text {
                    id: content
                    width: parent.width - 8
                    x: 4
                    y: 1
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    font.family: Theme.mono
                    font.pixelSize: Theme.smallSize
                    color: line.modelData.kind === "add" ? Theme.added
                           : line.modelData.kind === "remove" ? Theme.removed
                           : Theme.textDim
                    text: line.modelData.kind === "gap"
                          ? " ".repeat(view.numberWidth) + "   ⋯ " + line.modelData.count + " unchanged line" + (line.modelData.count === 1 ? "" : "s")
                          : String(line.modelData.number).padStart(view.numberWidth) + " "
                            + (line.modelData.kind === "add" ? "+ " : line.modelData.kind === "remove" ? "- " : "  ")
                            + line.modelData.text
                }
            }
        }

        Text {
            visible: view.rows.length > view.maxLines
            text: "… " + (view.rows.length - view.maxLines) + " more diff lines"
            color: Theme.textFaint
            font.family: Theme.mono
            font.pixelSize: Theme.smallSize
            Layout.leftMargin: 4
        }
    }
}
