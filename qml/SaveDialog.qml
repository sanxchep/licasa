/*!
    SaveDialog.qml
    --------------
    Lazily loaded Save As dialog for edited images.
*/

import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: dialog

    required property var formatSupport
    required property url suggestedFile

    signal fileChosen(url fileUrl)

    title: "Save edited image as"
    fileMode: FileDialog.SaveFile
    nameFilters: formatSupport.saveNameFilters
    defaultSuffix: "png"

    Component.onCompleted: selectedFile = suggestedFile
    onAccepted: fileChosen(selectedFile)
}
