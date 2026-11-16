import QtQuick 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// The start: with the matrix theme, digital rain that settles into
// "AGENT SMITH" while Smith connects; any click skips it. If startup fails,
// it says why and offers to try again.
Rectangle {
    id: splash

    property bool rainDone: !Theme.matrix
    readonly property bool finished: rainDone && Backend.state === "ready"
    signal dismissed()

    color: Theme.window

    onFinishedChanged: if (finished) fade.start()

    NumberAnimation {
        id: fade
        target: splash
        property: "opacity"
        to: 0
        duration: 350
        onFinished: splash.dismissed()
    }

    RainItem {
        anchors.fill: parent
        visible: Theme.matrix
        title: "AGENT SMITH"
        running: Theme.matrix
        glyphSize: 18
        fontFamily: Theme.mono
        headColor: Theme.rainHead
        brightColor: Theme.accent
        color: Theme.accentSoft
        fadedColor: Theme.borderStrong
        onFinished: splash.rainDone = true
    }

    TapHandler { onTapped: splash.rainDone = true }

    ColumnLayout {
        anchors.centerIn: parent
        visible: !Theme.matrix
        spacing: 10
        Text {
            textFormat: Text.PlainText
            Layout.alignment: Qt.AlignHCenter
            text: "Smith"
            color: Theme.accent
            font.family: Theme.mono
            font.pixelSize: 32
            font.bold: true
        }
        Spinner { Layout.alignment: Qt.AlignHCenter; running: Backend.state === "starting" }
    }

    // Connecting, or what went wrong.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 48
        visible: Backend.state !== "ready" && (splash.rainDone || Backend.state === "failed")
        width: Math.min(parent.width - 80, 720)
        implicitHeight: status.implicitHeight + 32
        color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, 0.94)
        border.color: Backend.state === "failed" ? Theme.error : Theme.borderStrong
        radius: Theme.radius

        ColumnLayout {
            id: status
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                spacing: 10
                Spinner { visible: Backend.state === "starting"; running: visible }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: Backend.state === "failed" ? "Smith couldn't start." : "Connecting to the model server…"
                    color: Backend.state === "failed" ? Theme.error : Theme.textDim
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.fontSize
                    font.bold: Backend.state === "failed"
                }
            }
            TextEdit {
                visible: Backend.state === "failed"
                Layout.fillWidth: true
                text: Backend.startupError
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.text
                selectionColor: Theme.selection
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
            }
            RowLayout {
                visible: Backend.state === "failed"
                Layout.alignment: Qt.AlignRight
                SmithButton { text: "Quit"; onClicked: Backend.quit() }
                SmithButton { text: "Try again"; kind: "primary"; onClicked: Backend.start() }
            }
        }
    }
}
