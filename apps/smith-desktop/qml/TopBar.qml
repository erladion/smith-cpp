import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// What the agent talks to and how: endpoint, model, reasoning, the sandbox's
// network; and the turn controls, undo and compact.
Rectangle {
    id: bar

    signal pick(string kind)

    implicitHeight: 52
    color: Theme.window

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }

    component Chip: SmithButton {
        property string label
        property string value
        text: value
        leftPadding: 10
        rightPadding: 10
        enabled: Backend.state === "ready" && !Backend.busy
        contentItem: Row {
            spacing: 6
            Text {
                textFormat: Text.PlainText
                text: parent.parent.label
                color: Theme.textFaint
                font.family: Theme.textFamily
                font.pixelSize: Theme.tinySize
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                textFormat: Text.PlainText
                text: parent.parent.value
                color: parent.parent.enabled ? Theme.text : Theme.textDim
                font.family: Theme.mono
                font.pixelSize: Theme.smallSize
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                width: Math.min(implicitWidth, 240)
            }
            Text {
                textFormat: Text.PlainText
                text: "▾"
                color: Theme.textFaint
                font.pixelSize: Theme.tinySize
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 8

        Chip {
            visible: Backend.endpoints.length > 0
            label: "endpoint"
            value: Backend.endpoint || "—"
            tip: Backend.serverLabel
            onClicked: bar.pick("endpoint")
        }
        Chip {
            label: "model"
            value: Backend.model || "…"
            tip: "Pick a model (ctrl+k)"
            onClicked: bar.pick("model")
        }
        Chip {
            label: "reasoning"
            value: Backend.reasoning || "default"
            onClicked: bar.pick("reasoning")
        }

        Item { Layout.fillWidth: true }

        // The sandbox's network, as a switch.
        Rectangle {
            visible: Backend.sandboxActive
            implicitWidth: netRow.implicitWidth + 20
            implicitHeight: 32
            radius: Theme.radius
            color: netHover.hovered ? Theme.surfaceRaised : "transparent"
            border.color: Theme.border
            HoverHandler { id: netHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: if (!Backend.busy) Backend.setNetwork(!Backend.network) }
            ToolTip.visible: netHover.hovered
            ToolTip.text: Backend.network ? "Sandboxed commands can use the network" : "Sandboxed commands have no network"
            ToolTip.delay: 600
            Row {
                id: netRow
                anchors.centerIn: parent
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    text: "network"
                    color: Theme.textFaint
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.tinySize
                    anchors.verticalCenter: parent.verticalCenter
                }
                Rectangle {
                    width: 30
                    height: 16
                    radius: 8
                    color: Backend.network ? Theme.accentWash : Theme.window
                    border.color: Backend.network ? Theme.accent : Theme.borderStrong
                    anchors.verticalCenter: parent.verticalCenter
                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        y: 3
                        x: Backend.network ? 17 : 3
                        color: Backend.network ? Theme.accent : Theme.textFaint
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }
                }
            }
        }

        SmithButton {
            text: "Undo"
            tip: "Undo the last turn: restore files it changed and edit your message again"
            enabled: Backend.canUndo
            onClicked: Backend.undo()
        }
        SmithButton {
            text: "Compact"
            tip: "Summarize the conversation now to free context"
            enabled: Backend.state === "ready" && !Backend.busy
            onClicked: Backend.compact()
        }
        SmithButton {
            text: "⋯"
            tip: "Skills, background jobs and help"
            onClicked: more.open()
            Menu {
                id: more
                y: parent.height + 4
                background: Rectangle {
                    implicitWidth: 220
                    color: Theme.surfaceRaised
                    border.color: Theme.borderStrong
                    radius: Theme.radius
                }
                delegate: MenuItem {
                    id: menuItem
                    contentItem: Text {
                        textFormat: Text.PlainText
                        text: menuItem.text
                        color: menuItem.enabled ? Theme.text : Theme.textFaint
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                    }
                    background: Rectangle { color: menuItem.highlighted ? Theme.accentWash : "transparent" }
                }
                MenuItem { text: "Skills…"; onTriggered: bar.pick("skills") }
                MenuItem { text: "Background jobs…"; onTriggered: bar.pick("jobs") }
                MenuItem { text: "Commands and keys"; onTriggered: Backend.submit("/help") }
            }
        }
    }
}
