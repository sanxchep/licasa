/*!
    SaveDialog.qml
    --------------
    Lazily loaded Save As dialog for edited images.
*/

import QtQuick
import QtQuick.Dialogs
import "DialogRuntime.js" as DialogRuntime
import "FileUrls.js" as FileUrls

FileDialog {
    id: dialog
    objectName: "saveDialog"

    required property var formatSupport
    required property url suggestedFile
    property string fileFormat: ""

    signal fileChosen(url fileUrl)
    signal openFailed(string message)

    function openSafely() {
        return DialogRuntime.open(dialog)
    }

    function selectSuggestedFormat() {
        const suffix = FileUrls.parts(suggestedFile).suffix
        for (let index = 0; index < nameFilters.length; ++index) {
            if (String(nameFilters[index]).split(/[\s()]+/).indexOf("*." + suffix) >= 0) {
                selectedNameFilter.index = index
                break
            }
        }
    }

    title: "Save as a copy"
    fileMode: FileDialog.SaveFile
    nameFilters: fileFormat.length > 0
        ? formatSupport.saveNameFilters.filter(function(filter) {
            return String(filter).indexOf("*." + FileUrls.parts(suggestedFile).suffix) >= 0
                && String(filter).indexOf("All writable") < 0
        }) : formatSupport.saveNameFilters
    defaultSuffix: FileUrls.parts(suggestedFile).suffix || "png"

    Component.onCompleted: {
        selectedFile = suggestedFile
        selectSuggestedFormat()
    }
    onSuggestedFileChanged: {
        selectedFile = suggestedFile
        selectSuggestedFormat()
    }
    onAccepted: fileChosen(selectedFile)
}
