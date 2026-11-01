import QtQuick
import "FileUrls.js" as FileUrls

// Lazy Item 12 presentation coordinator. Main.qml instantiates this only on
// an explicit export request for a Motion Photo or animated image, keeping
// the ordinary JPEG/PNG startup path free of export-specific QML objects.
Item {
    id: coordinator

    required property url sourceUrl
    required property bool imageAnimated
    required property var motionExportService
    required property var animationExportService

    readonly property bool dialogVisible: Boolean(
        (motionExportDialogLoader.dialog && motionExportDialogLoader.dialog.visible)
        || (animationExportDialogLoader.dialog && animationExportDialogLoader.dialog.visible))

    signal dialogFailed(string message)
    signal dialogClosed()

    function resetFailedDialog(loader: Loader) {
        if (loader.status !== Loader.Error)
            return
        dialogFailed(qsTr("The export picker could not be loaded. Check the installation and try again."))
        failedDialogResetTimer.pendingLoaders = failedDialogResetTimer.pendingLoaders.concat([loader])
        failedDialogResetTimer.restart()
    }

    Timer {
        id: failedDialogResetTimer
        property var pendingLoaders: []
        interval: 0
        repeat: false
        onTriggered: {
            const failed = pendingLoaders
            pendingLoaders = []
            for (let index = 0; index < failed.length; ++index) {
                const loader = failed[index]
                if (loader && loader.status === Loader.Error) {
                    loader.active = false
                    loader.source = ""
                }
            }
        }
    }

    visible: false

    function suggestedMotionExportUrl() {
        const service = motionExportService
        if (!FileUrls.isLocal(sourceUrl)
                || !service
                || !service.available
                || service.suggestedSuffix.length === 0) {
            return ""
        }

        return FileUrls.suggestedCopy(sourceUrl, "motion", service.suggestedSuffix)
    }

    function exportMotionCopy(destinationUrl) {
        const service = motionExportService
        if (!FileUrls.isLocal(destinationUrl)
                || !service
                || !service.available
                || service.busy) {
            return
        }
        service.exportCopy(destinationUrl)
    }

    function openMotionExportDialog() {
        const service = motionExportService
        if (!FileUrls.isLocal(sourceUrl)
                || !service
                || !service.available
                || service.busy
                || service.suggestedSuffix.length === 0) {
            return
        }

        const suggestion = suggestedMotionExportUrl()
        const suffix = service.suggestedSuffix
        if (motionExportDialogLoader.dialog) {
            motionExportDialogLoader.dialog.suggestedFile = suggestion
            motionExportDialogLoader.dialog.suggestedSuffix = suffix
            motionExportDialogLoader.dialog.openSafely()
            return
        }

        if (!motionExportDialogLoader.active) {
            motionExportDialogLoader.setSource(
                "qrc:/Licasa/qml/MotionExportDialog.qml",
                {
                    "suggestedFile": suggestion,
                    "suggestedSuffix": suffix
                }
            )
            motionExportDialogLoader.active = true
        }
    }

    function currentAnimationExportSuffix() {
        if (!imageAnimated
                || !FileUrls.isLocal(sourceUrl)) {
            return ""
        }

        return FileUrls.parts(sourceUrl).suffix
    }

    function suggestedAnimationExportUrl() {
        const suffix = currentAnimationExportSuffix()
        if (suffix.length === 0)
            return ""

        return FileUrls.suggestedCopy(sourceUrl, "animation", suffix)
    }

    function exportAnimationCopy(destinationUrl) {
        const service = animationExportService
        if (!imageAnimated
                || !FileUrls.isLocal(sourceUrl)
                || !FileUrls.isLocal(destinationUrl)
                || !service
                || service.busy) {
            return
        }
        service.exportCopy(sourceUrl, destinationUrl)
    }

    function openAnimationExportDialog() {
        const service = animationExportService
        if (!imageAnimated || !service || service.busy)
            return

        const suffix = currentAnimationExportSuffix()
        const suggestion = suggestedAnimationExportUrl()
        if (suffix.length === 0 || !FileUrls.isLocal(suggestion))
            return

        if (animationExportDialogLoader.dialog) {
            animationExportDialogLoader.dialog.suggestedFile = suggestion
            animationExportDialogLoader.dialog.suggestedSuffix = suffix
            animationExportDialogLoader.dialog.openSafely()
            return
        }

        if (!animationExportDialogLoader.active) {
            animationExportDialogLoader.setSource(
                "qrc:/Licasa/qml/AnimationExportDialog.qml",
                {
                    "suggestedFile": suggestion,
                    "suggestedSuffix": suffix
                }
            )
            animationExportDialogLoader.active = true
        }
    }

    Connections {
        target: coordinator.motionExportService

        function onExportCompleted(sourceUrl, destinationUrl) {
            if (FileUrls.isLocal(coordinator.sourceUrl)
                    && sourceUrl.toString() === String(coordinator.sourceUrl)) {
                console.info(qsTr("Exported Motion Photo video:"), destinationUrl.toString())
            }
        }

        function onExportFailed(sourceUrl, message) {
            if (FileUrls.isLocal(coordinator.sourceUrl)
                    && sourceUrl.toString() === String(coordinator.sourceUrl)) {
                console.warn(qsTr("Motion Photo export failed:"), message)
            }
        }
    }

    Connections {
        target: coordinator.animationExportService

        function onExportCompleted(sourceUrl, destinationUrl) {
            if (FileUrls.isLocal(coordinator.sourceUrl)
                    && sourceUrl.toString() === String(coordinator.sourceUrl)) {
                console.info(qsTr("Exported animation copy:"), destinationUrl.toString())
            }
        }

        function onExportFailed(sourceUrl, message) {
            if (FileUrls.isLocal(coordinator.sourceUrl)
                    && sourceUrl.toString() === String(coordinator.sourceUrl)) {
                console.warn(qsTr("Animation export failed:"), message)
            }
        }
    }

    Loader {
        id: motionExportDialogLoader
        objectName: "motionExportDialogLoader"
        onStatusChanged: coordinator.resetFailedDialog(motionExportDialogLoader)
        readonly property var dialog: item
        active: false
        asynchronous: true

        onLoaded: {
            if (dialog) {
                dialog.fileChosen.connect(coordinator.exportMotionCopy)
                dialog.openFailed.connect(coordinator.dialogFailed)
                dialog.accepted.connect(coordinator.dialogClosed)
                dialog.rejected.connect(coordinator.dialogClosed)
                dialog.openSafely()
            }
        }
    }

    Loader {
        id: animationExportDialogLoader
        objectName: "animationExportDialogLoader"
        onStatusChanged: coordinator.resetFailedDialog(animationExportDialogLoader)
        readonly property var dialog: item
        active: false
        asynchronous: true

        onLoaded: {
            if (dialog) {
                dialog.fileChosen.connect(coordinator.exportAnimationCopy)
                dialog.openFailed.connect(coordinator.dialogFailed)
                dialog.accepted.connect(coordinator.dialogClosed)
                dialog.rejected.connect(coordinator.dialogClosed)
                dialog.openSafely()
            }
        }
    }
}
