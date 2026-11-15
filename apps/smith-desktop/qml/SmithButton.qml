import QtQuick 2.15
import QtQuick.Controls 2.15

// A flat button in the theme: "normal", "primary" (accent), "danger", or "ghost" (no border).
Button {
    id: control

    property string kind: "normal"
    property string keyHint: ""
    property string tip: ""

    readonly property color tone: kind === "primary" ? Theme.accent : kind === "danger" ? Theme.error : Theme.text

    implicitHeight: 32
    leftPadding: 14
    rightPadding: 14
    font.family: Theme.textFamily
    font.pixelSize: Theme.smallSize
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    ToolTip.visible: tip.length > 0 && hovered
    ToolTip.text: tip
    ToolTip.delay: 600

    contentItem: Row {
        spacing: 8
        Text {
            textFormat: Text.PlainText
            text: control.text
            font: control.font
            color: !control.enabled ? Theme.textFaint : control.kind === "normal" || control.kind === "ghost" ? Theme.text : control.tone
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            textFormat: Text.PlainText
            visible: control.keyHint.length > 0
            text: control.keyHint
            font.family: Theme.mono
            font.pixelSize: Theme.tinySize
            color: Theme.textFaint
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Qt.darker(fill, 1.2) : control.hovered ? Qt.lighter(fill, Theme.matrix ? 1.6 : 1.25) : fill
        border.width: control.kind === "ghost" ? 0 : 1
        border.color: control.kind === "normal" ? (control.hovered ? Theme.borderStrong : Theme.border) : control.tone
        readonly property color fill: control.kind === "primary" ? Theme.accentWash
                                      : control.kind === "danger" ? Theme.removedWash
                                      : control.kind === "ghost" ? "transparent" : Theme.surfaceRaised
        Behavior on color { ColorAnimation { duration: 90 } }
    }
}
