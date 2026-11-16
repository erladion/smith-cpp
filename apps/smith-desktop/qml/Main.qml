import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Smith.Desktop 1.0

ApplicationWindow {
    id: window

    width: 1320
    height: 860
    minimumWidth: 820
    minimumHeight: 560
    visible: true
    title: Backend.projectName ? "Smith — " + Backend.projectName : "Smith"
    color: Theme.window
    font.family: Theme.textFamily
    font.pixelSize: Theme.fontSize

    property bool sidebarVisible: true

    // The picker in use and what choosing from it does.
    property string pickerKind: ""

    function openPicker(kind) {
        if (Backend.state !== "ready") {
            return
        }
        if (kind === "folder") {
            folderDialog.open()
            return
        }
        if (kind === "sessions") {
            sidebarVisible = true
            return
        }
        pickerKind = kind
        if (kind === "model") {
            Backend.refreshModels()
        }
        picker.open()
    }

    readonly property var pickerSpec: {
        switch (pickerKind) {
        case "model": {
            const items = Backend.models.map(m => ({ value: m.id, label: m.id, hint: m.context > 0 ? m.context.toLocaleString(Qt.locale("en_US"), "f", 0) + " ctx" : "" }))
            if (!items.some(i => i.value === Backend.model)) {
                items.unshift({ value: Backend.model, label: Backend.model, hint: "" })
            }
            return { title: "Select a model", items: items, current: Backend.model, empty: "The server lists no models." }
        }
        case "endpoint":
            return { title: "Select an endpoint", items: Backend.endpoints.map(e => ({ value: e, label: e, hint: "" })), current: Backend.endpoint,
                     empty: "No endpoints are configured in ~/.config/smith/config.json." }
        case "reasoning": {
            const hints = { "default": "send nothing; the server and model decide", "off": "turn thinking off (models with an enable_thinking switch)",
                            "low": "brief reasoning", "medium": "moderate reasoning", "high": "thorough reasoning, slower" }
            return { title: "Select reasoning level", items: Backend.reasoningLevels.map(l => ({ value: l, label: l, hint: hints[l] || "" })),
                     current: Backend.reasoning, empty: "" }
        }
        case "theme":
            return { title: "Select theme", items: Backend.themeChoices(), current: Backend.theme, empty: "" }
        case "plan":
            return { title: "The plan is ready", items: [
                         { value: "go", label: "Carry out the plan", hint: "plan mode off, and the agent starts" },
                         { value: "keep", label: "Keep planning", hint: "stay in plan mode and say what to change" }],
                     current: "", empty: "" }
        case "recent":
            return { title: "Open a recent project", items: Backend.recentProjects.map(p => ({ value: p.path, label: p.name, hint: p.hint + " · " + p.where })),
                     current: "", empty: "No other folders have sessions yet." }
        case "skills":
            return { title: "Skills", items: Backend.skills.map(s => ({ value: s.name, label: s.name, hint: s.description })), current: "",
                     empty: "No skills found. Add folders with a SKILL.md to .agents/skills/ in the project or ~/.config/smith/skills/." }
        case "worktrees":
            return { title: "Worktrees", items: Backend.worktrees.map(w => ({ value: w.path, label: (w.main ? "" : "⎇ ") + w.label, hint: w.hint })),
                     current: (Backend.worktrees.find(w => w.current) || {}).path || "",
                     empty: "Not in a git repository." }
        case "jobs":
            return { title: "Background jobs (enter stops one)", items: Backend.jobs.map(j => ({ value: j.id, label: j.id + "  " + j.command, hint: j.status + " · started " + j.started + " ago" })),
                     current: "", empty: "No background jobs. The agent starts them with bash's run_in_background." }
        }
        return { title: "", items: [], current: "", empty: "" }
    }

    Picker {
        id: picker
        objectName: "picker"
        title: window.pickerSpec.title
        items: window.pickerSpec.items
        current: window.pickerSpec.current
        emptyText: window.pickerSpec.empty
        onPicked: value => {
            switch (window.pickerKind) {
            case "model": Backend.setModel(value); break
            case "endpoint": Backend.setEndpoint(value); break
            case "reasoning": Backend.setReasoning(value); break
            case "theme": Backend.setTheme(value); break
            case "plan": if (value === "go") Backend.carryOutPlan(); break
            case "recent": Backend.openProject(value); break
            case "jobs": Backend.stopJob(value); break
            case "worktrees": Backend.switchToWorktree(value); break
            }
            composer.focusInput()
        }
        onClosed: composer.focusInput()
    }

    ProjectFolderDialog {
        id: folderDialog
        startFolder: "file://" + Backend.projectPath
        onPicked: folder => Backend.openProject(folder)
    }

    Connections {
        target: Backend
        function onRestoreInput(text) { composer.setText(text) }
        function onOpenPicker(kind) { window.openPicker(kind) }
        function onRedPill() { redPill.visible = true }
        function onConfirmDrop(path) { sidebar.askDrop(path) }
    }

    Shortcut { sequence: "Ctrl+N"; enabled: !Backend.busy; onActivated: Backend.newSession("") }
    Shortcut { sequence: "Ctrl+O"; onActivated: window.openPicker("folder") }
    Shortcut { sequence: "Ctrl+K"; enabled: !Backend.busy; onActivated: window.openPicker("model") }
    Shortcut { sequence: "Ctrl+B"; onActivated: window.sidebarVisible = !window.sidebarVisible }
    Shortcut { sequence: "Ctrl+Q"; onActivated: Backend.quit() }
    Shortcut { sequence: "Ctrl+Shift+R"; enabled: Theme.matrix; onActivated: redPill.visible = true }

    onClosing: Backend.quit()

    RowLayout {
        anchors.fill: parent
        spacing: 0
        visible: Backend.state === "ready"

        Sidebar {
            id: sidebar
            visible: window.sidebarVisible
            Layout.preferredWidth: 290
            Layout.fillHeight: true
            onOpenFolder: window.openPicker("folder")
            onOpenRecent: window.openPicker("recent")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            TopBar {
                Layout.fillWidth: true
                onPick: kind => window.openPicker(kind)
            }

            TranscriptView {
                id: transcript
                objectName: "transcript"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            // The bottom: what's running, then the input or the question waiting for you.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.maximumWidth: transcript.column + 32
                Layout.alignment: Qt.AlignHCenter
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 14
                spacing: 8

                ActivityBar {
                    Layout.fillWidth: true
                    visible: Backend.busy && Object.keys(Backend.approval).length === 0 && Object.keys(Backend.question).length === 0
                }

                ApprovalPanel {
                    objectName: "approvalPanel"
                    Layout.fillWidth: true
                    visible: Object.keys(Backend.approval).length > 0
                }

                QuestionPanel {
                    Layout.fillWidth: true
                    visible: Object.keys(Backend.question).length > 0
                }

                Composer {
                    id: composer
                    Layout.fillWidth: true
                    visible: Object.keys(Backend.approval).length === 0 && Object.keys(Backend.question).length === 0
                    Component.onCompleted: focusInput()
                }
            }

            StatusBar {
                Layout.fillWidth: true
                onShowJobs: window.openPicker("jobs")
            }
        }
    }

    LandPanel {
        id: landPanel
        objectName: "landPanel"
    }

    Splash {
        id: splash
        objectName: "splash"
        anchors.fill: parent
        onDismissed: {
            visible = false
            composer.focusInput()
        }
    }

    // /redpill: digital rain over everything until a click or a key.
    Rectangle {
        id: redPill
        anchors.fill: parent
        visible: false
        color: Theme.window
        focus: visible
        Keys.onPressed: visible = false
        onVisibleChanged: if (visible) forceActiveFocus(); else composer.focusInput()
        RainItem {
            anchors.fill: parent
            endless: true
            running: redPill.visible
            glyphSize: 18
            fontFamily: Theme.mono
            headColor: Theme.rainHead
            brightColor: Theme.accent
            color: Theme.accentSoft
            fadedColor: Theme.borderStrong
        }
        TapHandler { onTapped: redPill.visible = false }
    }
}
