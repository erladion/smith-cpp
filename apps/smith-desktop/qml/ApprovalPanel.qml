import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Smith.Desktop

// "Allow bash?": what the call does, why it asks, the change it would make,
// and where it runs. y / a / n / esc answer it, as in the terminal.
Rectangle {
    id: panel

    readonly property var request: Backend.approval
    readonly property var warnings: (request.warning || "").length > 0 ? request.warning.split("\n") : []

    implicitHeight: column.implicitHeight + 28
    color: Theme.surface
    radius: Theme.radius
    border.width: 1
    border.color: warnings.length > 0 ? Theme.error : Theme.warn

    focus: visible
    onVisibleChanged: if (visible) forceActiveFocus()

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Y) {
            Backend.decide("allow")
        } else if (event.key === Qt.Key_A && panel.request.canAllowAlways) {
            Backend.decide("always")
        } else if (event.key === Qt.Key_N) {
            Backend.decide("deny")
        } else if (event.key === Qt.Key_Escape) {
            Backend.decide("interrupt")
        } else {
            return
        }
        event.accepted = true
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        RowLayout {
            spacing: 10
            Text {
                text: "⚠"
                color: panel.warnings.length > 0 ? Theme.error : Theme.warn
                font.pixelSize: 16
            }
            Text {
                text: "Allow " + (panel.request.tool || "") + "?"
                color: Theme.text
                font.family: Theme.textFamily
                font.pixelSize: Theme.fontSize
                font.bold: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.min(summary.implicitHeight, 220) + 14
            color: Theme.window
            radius: Theme.radius
            clip: true
            TextEdit {
                id: summary
                x: 10
                y: 7
                width: parent.width - 20
                text: panel.request.summary || ""
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
                color: Theme.code
                selectionColor: Theme.selection
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
            }
        }

        Text {
            visible: (panel.request.note || "").length > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: panel.request.note || ""
            color: Theme.warn
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
        }

        Repeater {
            model: panel.warnings
            Text {
                required property string modelData
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "⚠ " + modelData
                color: Theme.error
                font.family: Theme.textFamily
                font.pixelSize: Theme.smallSize
                font.bold: true
            }
        }

        ScrollView {
            visible: (panel.request.diff || []).length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(diff.implicitHeight, 300)
            DiffView {
                id: diff
                width: parent.width
                rows: panel.request.diff || []
                maxLines: 200
            }
        }

        Text {
            visible: panel.request.hasSandbox === true
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: (panel.request.sandboxActive ? "✓ " : "⚠ ") + (panel.request.sandboxDescription || "")
            color: panel.request.sandboxActive ? Theme.added : Theme.error
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
        }

        Flow {
            Layout.fillWidth: true
            spacing: 8
            SmithButton { text: "Yes"; keyHint: "y"; kind: "primary"; onClicked: Backend.decide("allow") }
            SmithButton {
                visible: !!panel.request.canAllowAlways
                text: "Always allow " + (panel.request.tool || "") + " this session"
                keyHint: "a"
                onClicked: Backend.decide("always")
            }
            SmithButton { text: "No"; keyHint: "n"; onClicked: Backend.decide("deny") }
            SmithButton { text: "No, and interrupt"; keyHint: "esc"; kind: "danger"; onClicked: Backend.decide("interrupt") }
        }
    }
}
