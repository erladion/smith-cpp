import QtQuick 2.15
import QtQuick.Dialogs

// The system's dialog for choosing a project folder. Qt 5 builds use the
// file of the same name in qt5/, where the dialog's type is a different one.
FolderDialog {
    // Where the dialog starts, as a file: URL.
    property string startFolder
    // The folder chosen, as a file: URL.
    signal picked(string folder)

    title: "Open a project folder"
    currentFolder: startFolder
    onAccepted: picked(selectedFolder.toString())
}
