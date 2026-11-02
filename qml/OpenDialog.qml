/*!
    OpenDialog.qml
    --------------
    Lazily loaded file dialog wrapper for Licasa.
*/

import QtQuick
import QtQuick.Dialogs
import "DialogRuntime.js" as DialogRuntime

FileDialog {
    id: dialog
    objectName: "openDialog"

    required property var formatSupport

    signal fileChosen(url fileUrl)
    signal openFailed(string message)

    function openSafely() {
        return DialogRuntime.open(dialog)
    }

    title: qsTr("Open image")
    fileMode: FileDialog.OpenFile
    nameFilters: formatSupport.nameFilters

    onAccepted: fileChosen(selectedFile)
}
