/*!
    AnimationExportDialog.qml
    -------------------------
    Lazily loaded destination picker for byte-identical animation export.
*/

import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: dialog

    required property url suggestedFile
    required property string suggestedSuffix

    signal fileChosen(url fileUrl)

    title: "Export animation as"
    fileMode: FileDialog.SaveFile
    nameFilters: ["Animation copy (*." + suggestedSuffix.toLowerCase() + ")"]
    defaultSuffix: suggestedSuffix.toLowerCase()

    Component.onCompleted: selectedFile = suggestedFile
    onAccepted: fileChosen(selectedFile)
}
