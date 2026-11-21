.import "FileUrls.js" as FileUrls

function destinationUrl(selection) {
    // Qt 6.4's fallback save picker returns an absolute path for typed names.
    const value = String(selection)
    return value.startsWith("/") ? FileUrls.fromLocalPath(value) : selection
}

// Probe the fallback dependency before Qt 6.4 FileDialog enters its native
// open path. Its C++ fallback dereferences a null implementation if this import
// fails, so Loader.Error and JavaScript exceptions around open() are too late.
// Keep this lazy: ordinary image viewing does not need the file-picker plugin.
function open(dialog) {
    let probe = null
    try {
        probe = Qt.createQmlObject(
            'import QtQml; import Qt.labs.folderlistmodel; FolderListModel { folder: "" }',
            dialog, "LicasaFileDialogPreflight")
    } catch (error) {
        const message = "The file picker could not be loaded. Your image and edits are safe. "
            + "Check the installation and restart Licasa before trying again. (Qt.labs.folderlistmodel)"
        console.warn("[Licasa] File dialog preflight failed:", String(error))
        dialog.openFailed(message)
        return false
    }
    probe.destroy()
    dialog.open()
    return true
}
