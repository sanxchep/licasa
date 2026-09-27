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
            motionExportDialogLoader.dialog.open()
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
            animationExportDialogLoader.dialog.open()
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
                console.info("Exported Motion Photo video:", destinationUrl.toString())
            }
        }

        function onExportFailed(sourceUrl, message) {
            if (FileUrls.isLocal(coordinator.sourceUrl)
                    && sourceUrl.toString() === String(coordinator.sourceUrl)) {
                console.warn("Motion Photo export failed:", message)
            }
        }
    }

    Connections {
        target: coordinator.animationExportService

        function onExportCompleted(sourceUrl, destinationUrl) {
            if (FileUrls.isLocal(coordinator.sourceUrl)
                    && sourceUrl.toString() === String(coordinator.sourceUrl)) {
                console.info("Exported animation copy:", destinationUrl.toString())
            }
        }

        function onExportFailed(sourceUrl, message) {
            if (FileUrls.isLocal(coordinator.sourceUrl)
                    && sourceUrl.toString() === String(coordinator.sourceUrl)) {
                console.warn("Animation export failed:", message)
            }
        }
    }

    Loader {
        id: motionExportDialogLoader
        readonly property var dialog: item
        active: false
        asynchronous: true

        onLoaded: {
            if (dialog) {
                dialog.fileChosen.connect(coordinator.exportMotionCopy)
                dialog.open()
            }
        }
    }

    Loader {
        id: animationExportDialogLoader
        readonly property var dialog: item
        active: false
        asynchronous: true

        onLoaded: {
            if (dialog) {
                dialog.fileChosen.connect(coordinator.exportAnimationCopy)
                dialog.open()
            }
        }
    }
}
