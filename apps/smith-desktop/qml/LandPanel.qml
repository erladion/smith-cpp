import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Smith.Desktop

// Landing a task worktree: what changed (as diffs), the commit message, and
// where it goes: cherry-picked onto your branch, merged, or left on the task
// branch. Afterwards, an offer to remove the worktree and its branch.
Popup {
    id: panel

    readonly property var landing: Backend.landing
    readonly property string stage: landing.stage || ""
    readonly property bool working: stage === "landing" || stage === "loading"
    readonly property string target: landing.target || ""

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 80, 920)
    height: stage === "review" || stage === "landing" ? Math.min(parent.height - 80, 780) : implicitHeight
    modal: true
    padding: 20
    closePolicy: working ? Popup.NoAutoClose : Popup.CloseOnEscape
    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }
    background: Rectangle { color: Theme.surface; border.color: Theme.accentSoft; radius: Theme.radius }

    // The stage before this one: the suggested message is filled in once, when the review first shows.
    property string previousStage: ""

    onStageChanged: {
        if (stage === "review" && previousStage !== "review") {
            message.text = landing.message || ""
            message.forceActiveFocus()
        }
        previousStage = stage
        if (stage.length > 0) {
            open()
        } else {
            close()
        }
    }
    onClosed: if (stage.length > 0 && !working) Backend.closeLanding()

    contentItem: ColumnLayout {
        spacing: 12

        // Title: the branch and what's in it.
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Spinner { visible: panel.working; running: panel.working }
            Text {
                text: (panel.stage === "landed" ? "Landed " : "Land ") + (panel.landing.branch || "")
                color: Theme.accent
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize + 1
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: panel.stage === "loading" ? "reading the changes…" : panel.stage === "landing" ? "landing…" : panel.landing.summary || ""
                color: Theme.textDim
                font.family: Theme.textFamily
                font.pixelSize: Theme.smallSize
                elide: Text.ElideRight
            }
        }

        // The changes, file by file.
        ScrollView {
            visible: panel.stage === "review" || panel.stage === "landing"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 10

                Repeater {
                    model: panel.landing.files || []
                    ColumnLayout {
                        id: file
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            Layout.fillWidth: true
                            text: "● " + file.modelData.name + "  " + file.modelData.summary
                            color: Theme.text
                            font.family: Theme.mono
                            font.pixelSize: Theme.smallSize
                            elide: Text.ElideMiddle
                        }
                        DiffView {
                            visible: file.modelData.diff.length > 0
                            Layout.fillWidth: true
                            rows: file.modelData.diff
                            maxLines: 400
                        }
                    }
                }

                ColumnLayout {
                    visible: (panel.landing.pending || []).length > 0
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: "Committed already, not on " + (panel.target || "your branch") + " yet:"
                        color: Theme.textDim
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                    }
                    Repeater {
                        model: panel.landing.pending || []
                        Text {
                            required property var modelData
                            text: "  " + modelData
                            color: Theme.text
                            font.family: Theme.mono
                            font.pixelSize: Theme.smallSize
                        }
                    }
                }
            }
        }

        // The commit message, suggested by the model.
        ColumnLayout {
            visible: (panel.stage === "review" || panel.stage === "landing") && panel.landing.uncommitted === true
            Layout.fillWidth: true
            spacing: 4
            Text {
                text: "Commit message"
                color: Theme.textDim
                font.family: Theme.textFamily
                font.pixelSize: Theme.smallSize
            }
            TextField {
                id: message
                objectName: "commitMessage"
                Layout.fillWidth: true
                enabled: !panel.working
                color: Theme.text
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize
                padding: 8
                selectByMouse: true
                background: Rectangle { color: Theme.window; border.color: message.activeFocus ? Theme.accentSoft : Theme.border; radius: Theme.radius }
                onAccepted: if (panel.target.length > 0) Backend.land("cherry-pick", text)
            }
            Text {
                visible: (panel.landing.error || "").length > 0
                text: panel.landing.error || ""
                color: Theme.error
                font.family: Theme.textFamily
                font.pixelSize: Theme.smallSize
            }
        }

        Text {
            visible: panel.stage === "review" || panel.stage === "landing"
            Layout.fillWidth: true
            text: panel.target.length > 0
                  ? "Lands on " + panel.target + " in " + (panel.landing.mainPath || "") + ". Your own uncommitted changes there are left alone; "
                    + "if they touch the same files, nothing happens and you're told."
                  : "Your checkout (" + (panel.landing.mainPath || "") + ") isn't on a branch, so this can only be committed on "
                    + (panel.landing.branch || "") + "."
            color: Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.tinySize
            wrapMode: Text.WordWrap
        }

        Text {
            visible: panel.stage === "landed"
            Layout.fillWidth: true
            Layout.maximumWidth: 640
            text: panel.landing.report || ""
            color: Theme.text
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
            wrapMode: Text.WordWrap
        }

        RowLayout {
            visible: panel.stage === "review" || panel.stage === "landing"
            Layout.fillWidth: true
            spacing: 8
            SmithButton { text: "Cancel"; kind: "ghost"; enabled: !panel.working; onClicked: Backend.closeLanding() }
            Item { Layout.fillWidth: true }
            SmithButton {
                visible: panel.landing.uncommitted === true
                text: "Commit on " + (panel.landing.branch || "") + " only"
                enabled: !panel.working
                tip: "Commit it on the task branch and land it yourself later"
                onClicked: Backend.land("branch", message.text)
            }
            SmithButton {
                visible: panel.target.length > 0
                text: "Merge into " + panel.target
                enabled: !panel.working
                tip: "A merge commit, or a fast-forward"
                onClicked: Backend.land("merge", message.text)
            }
            SmithButton {
                objectName: "cherryPick"
                visible: panel.target.length > 0
                text: "Cherry-pick onto " + panel.target
                kind: "primary"
                enabled: !panel.working
                tip: "A commit on top of " + panel.target + " (enter)"
                onClicked: Backend.land("cherry-pick", message.text)
            }
        }

        RowLayout {
            visible: panel.stage === "landed"
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            SmithButton { text: "Keep them"; onClicked: Backend.closeLanding() }
            SmithButton {
                objectName: "removeWorktree"
                text: "Remove the worktree and " + (panel.landing.branch || "")
                kind: "primary"
                onClicked: Backend.removeWorktree(panel.landing.path, false)
            }
        }
    }
}
