import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

// What the agent talks to and how: endpoint, model, reasoning, plan mode, the
// sandbox's network; and the changes so far, undo and compact.
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

    component ThemedItem: MenuItem {
        id: menuItem
        implicitHeight: 32
        leftPadding: 14
        rightPadding: 14
        contentItem: Text {
            textFormat: Text.PlainText
            text: menuItem.text
            color: menuItem.enabled ? Theme.text : Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle { color: menuItem.highlighted ? Theme.accentWash : "transparent" }
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

    // A labelled on/off switch.
    component Toggle: Rectangle {
        id: toggle
        property string label
        property bool on: false
        property string tip
        signal toggled()
        implicitWidth: toggleRow.implicitWidth + 20
        implicitHeight: 32
        radius: Theme.radius
        color: toggleHover.hovered ? Theme.surfaceRaised : "transparent"
        border.color: toggle.on ? Theme.accent : Theme.border
        HoverHandler { id: toggleHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: if (!Backend.busy) toggle.toggled() }
        SmithToolTip {
            visible: toggleHover.hovered
            text: toggle.tip
        }
        Row {
            id: toggleRow
            anchors.centerIn: parent
            spacing: 8
            Text {
                textFormat: Text.PlainText
                text: toggle.label
                color: toggle.on ? Theme.text : Theme.textFaint
                font.family: Theme.textFamily
                font.pixelSize: Theme.tinySize
                anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
                width: 30
                height: 16
                radius: 8
                color: toggle.on ? Theme.accentWash : Theme.window
                border.color: toggle.on ? Theme.accent : Theme.borderStrong
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    y: 3
                    x: toggle.on ? 17 : 3
                    color: toggle.on ? Theme.accent : Theme.textFaint
                    Behavior on x { NumberAnimation { duration: 120 } }
                }
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

        // Plan mode, as a switch.
        Toggle {
            objectName: "planToggle"
            label: "plan"
            on: Backend.planning
            tip: Backend.planning ? "Plan mode: the agent reads and plans, but changes nothing (shift+tab)"
                                  : "Plan mode: let the agent only read and plan first (shift+tab)"
            onToggled: Backend.setPlanning(!Backend.planning)
        }

        // The sandbox's network, as a switch.
        Toggle {
            visible: Backend.sandboxActive
            label: "network"
            on: Backend.network
            tip: Backend.network ? "Sandboxed commands can use the network" : "Sandboxed commands have no network"
            onToggled: Backend.setNetwork(!Backend.network)
        }

        SmithButton {
            objectName: "changesButton"
            text: "Changes"
            tip: "What the agent changed in files this session, file by file (/diff)"
            enabled: Backend.state === "ready"
            onClicked: Backend.showChanges()
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
            tip: "Theme, skills, background jobs and help"
            onClicked: more.open()
            Menu {
                id: more
                objectName: "moreMenu"
                // Right-aligned under the button, so it stays in the window.
                x: parent.width - width
                y: parent.height + 4
                topPadding: 4
                bottomPadding: 4
                background: Rectangle {
                    implicitWidth: 220
                    color: Theme.surfaceRaised
                    border.color: Theme.borderStrong
                    radius: Theme.radius
                }
                // Written out, not from a model, so each is a ThemedItem: a Menu's delegate only makes the items it creates itself.
                ThemedItem { text: "Theme…"; onTriggered: Backend.pickTheme() }
                ThemedItem { text: "Skills…"; onTriggered: bar.pick("skills") }
                ThemedItem { text: "Background jobs…"; onTriggered: bar.pick("jobs") }
                ThemedItem { text: "Commands and keys"; onTriggered: Backend.submit("/help") }
            }
        }
    }
}
