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

    SmithToolTip {
        visible: control.tip.length > 0 && control.hovered
        text: control.tip
    }

    // Its full width asked for; given less, the label is shortened in the middle.
    contentItem: Item {
        implicitWidth: label.implicitWidth + (hint.visible ? hint.implicitWidth + 8 : 0)
        implicitHeight: label.implicitHeight
        Text {
            id: label
            textFormat: Text.PlainText
            width: Math.min(implicitWidth, parent.width - (hint.visible ? hint.implicitWidth + 8 : 0))
            elide: Text.ElideMiddle
            text: control.text
            font: control.font
            color: !control.enabled ? Theme.textFaint : control.kind === "normal" || control.kind === "ghost" ? Theme.text : control.tone
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            id: hint
            textFormat: Text.PlainText
            visible: control.keyHint.length > 0
            x: label.width + 8
            text: control.keyHint
            font.family: Theme.mono
            font.pixelSize: Theme.tinySize
            color: Theme.textFaint
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    background: Rectangle {
        radius: Theme.radius
        // Hovering lightens a dark fill and darkens a light one.
        color: control.down ? Qt.darker(fill, Theme.light ? 1.12 : 1.2)
               : control.hovered ? (Theme.light ? Qt.darker(fill, 1.06) : Qt.lighter(fill, Theme.matrix ? 1.6 : 1.25)) : fill
        border.width: control.kind === "ghost" ? 0 : 1
        border.color: control.kind === "normal" ? (control.hovered ? Theme.borderStrong : Theme.border) : control.tone
        readonly property color fill: control.kind === "primary" ? Theme.accentWash
                                      : control.kind === "danger" ? Theme.removedWash
                                      : control.kind === "ghost" ? "transparent" : Theme.surfaceRaised
        Behavior on color { ColorAnimation { duration: 90 } }
    }
}
