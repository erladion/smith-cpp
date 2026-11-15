import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// A list moved through with ↑/↓ and narrowed down by typing, like the TUI's
// pickers. Items are {value, label, hint}; `current` is marked.
Popup {
    id: picker

    property string title: ""
    property var items: []
    property string current: ""
    property string emptyText: "Nothing here."
    signal picked(string value)

    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, parent ? parent.width - 80 : 560)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property var shown: {
        const needle = filter.text.toLowerCase()
        return needle.length === 0 ? items : items.filter(i => i.label.toLowerCase().indexOf(needle) >= 0)
    }

    onOpened: {
        filter.text = ""
        list.currentIndex = Math.max(0, items.findIndex(i => i.value === current))
        filter.forceActiveFocus()
    }

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.borderStrong
        radius: Theme.radius
    }

    contentItem: ColumnLayout {
        spacing: 0

        Text {
            textFormat: Text.PlainText
            text: picker.title
            color: Theme.accent
            font.family: Theme.textFamily
            font.pixelSize: Theme.fontSize
            font.bold: true
            Layout.margins: 14
            Layout.bottomMargin: 6
        }

        TextField {
            id: filter
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            placeholderText: "type to filter · ↑↓ move · enter select · esc cancel"
            placeholderTextColor: Theme.textFaint
            color: Theme.text
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
            selectionColor: Theme.selection
            background: Rectangle {
                implicitHeight: 32
                color: Theme.window
                radius: Theme.radius
                border.color: filter.activeFocus ? Theme.accentSoft : Theme.border
            }
            onTextChanged: list.currentIndex = 0
            Keys.onUpPressed: list.currentIndex = list.currentIndex <= 0 ? list.count - 1 : list.currentIndex - 1
            Keys.onDownPressed: list.currentIndex = list.currentIndex >= list.count - 1 ? 0 : list.currentIndex + 1
            Keys.onReturnPressed: picker.choose(list.currentIndex)
            Keys.onEnterPressed: picker.choose(list.currentIndex)
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 340)
            Layout.topMargin: 8
            Layout.bottomMargin: 8
            clip: true
            model: picker.shown
            highlightMoveDuration: 0
            ScrollIndicator.vertical: ScrollIndicator {}

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                width: list.width
                height: 34
                color: list.currentIndex === index ? Theme.accentWash : hover.hovered ? Theme.surfaceRaised : "transparent"

                HoverHandler { id: hover }
                TapHandler { onTapped: picker.choose(row.index) }

                Rectangle {
                    visible: list.currentIndex === row.index
                    width: 2
                    height: parent.height
                    color: Theme.accent
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 16
                    spacing: 10
                    Text {
                        textFormat: Text.PlainText
                        text: row.modelData.label
                        color: list.currentIndex === row.index ? Theme.accent : Theme.text
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.smallSize
                        font.bold: list.currentIndex === row.index
                    }
                    Text {
                        textFormat: Text.PlainText
                        visible: row.modelData.value === picker.current
                        text: "(current)"
                        color: Theme.added
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.tinySize
                        anchors.baseline: parent.children[0].baseline
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: row.modelData.hint || ""
                        color: Theme.textFaint
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.tinySize
                        elide: Text.ElideRight
                        width: Math.max(0, list.width - x - 30)
                        anchors.baseline: parent.children[0].baseline
                    }
                }
            }
        }

        Text {
            textFormat: Text.PlainText
            visible: list.count === 0
            text: picker.items.length === 0 ? picker.emptyText : "No matches."
            color: Theme.textFaint
            font.family: Theme.textFamily
            font.pixelSize: Theme.smallSize
            Layout.margins: 14
            Layout.topMargin: 0
        }
    }

    function choose(index) {
        if (index >= 0 && index < shown.length) {
            const value = shown[index].value
            close()
            picked(value)
        }
    }
}
