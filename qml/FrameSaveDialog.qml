/*!
    FrameSaveDialog.qml
    -------------------
    Lazily loaded Save As dialog for a captured Motion Photo frame.
*/

import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: dialog

    required property var formatSupport
    required property url suggestedFile

    signal fileChosen(url fileUrl)

    title: "Save current frame as"
    fileMode: FileDialog.SaveFile
    nameFilters: formatSupport.saveNameFilters
    defaultSuffix: "png"

    Component.onCompleted: selectedFile = suggestedFile
    onAccepted: fileChosen(selectedFile)
}
