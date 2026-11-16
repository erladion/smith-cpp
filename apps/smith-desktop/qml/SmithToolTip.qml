import QtQuick 2.15
import QtQuick.Controls 2.15

// A tooltip in the theme. The attached ToolTip shares one instance in the
// style's own look (white, and the window's full-size font), so it isn't used.
ToolTip {
    id: tip

    objectName: "smithToolTip"
    delay: 600
    font.family: Theme.textFamily
    font.pixelSize: Theme.smallSize
    leftPadding: 8
    rightPadding: 8
    topPadding: 5
    bottomPadding: 5
    // Long ones wrap.
    width: Math.min(implicitWidth, 420)

    contentItem: Text {
        textFormat: Text.PlainText
        text: tip.text
        font: tip.font
        color: Theme.text
        wrapMode: Text.Wrap
    }
    background: Rectangle {
        color: Theme.surfaceRaised
        border.color: Theme.borderStrong
        radius: Theme.radius
    }
}
