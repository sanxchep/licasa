/*!
    AnimationExportDialog.qml
    -------------------------
    Lazily loaded destination picker for byte-identical animation export.
*/

import QtQuick
import QtQuick.Dialogs
import "DialogRuntime.js" as DialogRuntime

FileDialog {
    id: dialog
    objectName: "animationExportDialog"

    required property url suggestedFile
    required property string suggestedSuffix

    signal fileChosen(url fileUrl)
    signal openFailed(string message)

    function openSafely() {
        return DialogRuntime.open(dialog)
    }

    title: qsTr("Export animation as")
    fileMode: FileDialog.SaveFile
    nameFilters: [qsTr("Animation copy (*.") + suggestedSuffix.toLowerCase() + ")"]
    defaultSuffix: suggestedSuffix.toLowerCase()

    Component.onCompleted: selectedFile = suggestedFile
    onAccepted: fileChosen(selectedFile)
}
