/*!
    MotionExportDialog.qml
    ----------------------
    Lazily loaded destination picker for byte-identical Motion Photo video export.
*/

import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: dialog

    required property url suggestedFile
    required property string suggestedSuffix

    signal fileChosen(url fileUrl)

    title: "Export motion as"
    fileMode: FileDialog.SaveFile
    nameFilters: suggestedSuffix.toLowerCase() === "mov"
        ? ["QuickTime movie (*.mov)"]
        : ["MPEG-4 video (*.mp4)"]
    defaultSuffix: suggestedSuffix.toLowerCase()

    Component.onCompleted: selectedFile = suggestedFile
    onAccepted: fileChosen(selectedFile)
}
