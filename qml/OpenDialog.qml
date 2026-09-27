/*!
    OpenDialog.qml
    --------------
    Lazily loaded file dialog wrapper for Licasa.
*/

import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: dialog

    required property var formatSupport

    signal fileChosen(url fileUrl)

    title: "Open image"
    fileMode: FileDialog.OpenFile
    nameFilters: formatSupport.nameFilters

    onAccepted: fileChosen(selectedFile)
}
