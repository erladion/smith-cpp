import QtQuick 2.15
import Qt.labs.platform 1.1

// The system's dialog for choosing a project folder, for Qt 5: the same
// properties and signal as ../ProjectFolderDialog.qml. Where the desktop has
// no dialog of its own this falls back to the Qt Widgets one, which is why
// the Qt 5 build's main() makes a QApplication.
FolderDialog {
    // Where the dialog starts, as a file: URL.
    property string startFolder
    // The folder chosen, as a file: URL.
    signal picked(string folder)

    title: "Open a project folder"
    currentFolder: startFolder
    onAccepted: picked(folder.toString())
}
