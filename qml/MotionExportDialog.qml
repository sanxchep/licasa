/*!
    MotionExportDialog.qml
    ----------------------
    Lazily loaded destination picker for byte-identical Motion Photo video export.
*/

import QtQuick
import QtQuick.Dialogs
import "DialogRuntime.js" as DialogRuntime

FileDialog {
    id: dialog
    objectName: "motionExportDialog"

    required property url suggestedFile
    required property string suggestedSuffix

    signal fileChosen(url fileUrl)
    signal openFailed(string message)

    function openSafely() {
        return DialogRuntime.open(dialog)
    }

    title: qsTr("Export motion as")
    fileMode: FileDialog.SaveFile
    nameFilters: suggestedSuffix.toLowerCase() === "mov"
        ? [qsTr("QuickTime movie (*.mov)")]
        : [qsTr("MPEG-4 video (*.mp4)")]
    defaultSuffix: suggestedSuffix.toLowerCase()

    Component.onCompleted: selectedFile = suggestedFile
    onAccepted: fileChosen(selectedFile)
}
