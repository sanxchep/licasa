import QtQuick
import "ViewerMath.js" as ViewerMath

QtObject {
    id: controller

    required property var hostWindow
    required property var nativeOps
    property var gestureSurface: null
    property bool gestureActive: false
    property bool gesturePreparing: false
    property int gesturePrepareFrames: 0
    property real gesturePrepareStartedAt: 0
    property rect gestureScreenGeometry: Qt.rect(0, 0, 1, 1)
    readonly property rect gestureImageGeometry: exactGeometryForScale(scale)

    readonly property real openScreenFraction: 0.30
    readonly property real minimumScreenFraction: 0.10
    readonly property real maximumScreenFraction: 0.80

    property real scale: 1.0
    property real targetScale: 1.0
    property bool stateValid: false

    onScaleChanged: {
        if (zoomAnimation.running) {
            nativeOps.traceZoom("animation", scale)
            applyState()
        }
    }

    property bool geometryAnchorActive: false
    property real anchorGlobalX: 0.0
    property real anchorGlobalY: 0.0
    property real anchorImageX: 0.0
    property real anchorImageY: 0.0

    property rect restoreGeometry: Qt.rect(0, 0, 1, 1)
    property bool restoreGeometryValid: false
    property bool suppressGeometryCapture: false

    function screenGeometry() {
        if (gestureActive)
            return gestureScreenGeometry
        // Physical monitor geometry preserves the real origin of vertically
        // staggered displays; a shared desktop work area does not.
        const geometry = nativeOps.currentScreenGeometry(hostWindow)
        if (geometry && geometry.width > 0 && geometry.height > 0)
            return geometry
        return {
            "x": hostWindow.x,
            "y": hostWindow.y,
            "width": Math.max(1.0, hostWindow.width),
            "height": Math.max(1.0, hostWindow.height)
        }
    }

    function screenWidth() {
        return Math.max(1.0, screenGeometry().width)
    }

    function screenHeight() {
        return Math.max(1.0, screenGeometry().height)
    }

    function screenCenterX() {
        const screen = screenGeometry()
        return screen.x + screen.width * 0.5
    }

    function screenCenterY() {
        const screen = screenGeometry()
        return screen.y + screen.height * 0.5
    }

    function defaultGeometry() {
        const targetWidth = Math.min(Math.round(screenWidth()), 460)
        const targetHeight = Math.min(Math.round(screenHeight()), 220)
        return Qt.rect(
            Math.round(screenCenterX() - targetWidth * 0.5),
            Math.round(screenCenterY() - targetHeight * 0.5),
            targetWidth,
            targetHeight
        )
    }

    function initialize() {
        restoreGeometry = defaultGeometry()
        restoreGeometryValid = true
    }

    function beginProgrammaticGeometryChange() {
        suppressGeometryCapture = true
        geometryCaptureReleaseTimer.restart()
    }

    function setGeometrySilently(targetX, targetY, targetWidth, targetHeight) {
        beginProgrammaticGeometryChange()
        if (!nativeOps.setWindowGeometry(
                hostWindow,
                targetX,
                targetY,
                targetWidth,
                targetHeight)) {
            hostWindow.x = targetX
            hostWindow.y = targetY
            hostWindow.width = targetWidth
            hostWindow.height = targetHeight
        }
    }

    function currentNativeGeometry() {
        const geometry = nativeOps.currentWindowGeometry(hostWindow)
        if (geometry.width > 0 && geometry.height > 0)
            return geometry
        return Qt.rect(
            hostWindow.x,
            hostWindow.y,
            hostWindow.width,
            hostWindow.height
        )
    }

    function geometriesMatch(left, right) {
        return left.x === right.x
            && left.y === right.y
            && left.width === right.width
            && left.height === right.height
    }

    function captureGeometry() {
        if (hostWindow.fullScreenMode || gestureActive || suppressGeometryCapture)
            return

        const geometry = currentNativeGeometry()
        if (geometry.width <= 0 || geometry.height <= 0)
            return

        restoreGeometry = Qt.rect(
            geometry.x,
            geometry.y,
            geometry.width,
            geometry.height
        )
        restoreGeometryValid = true
    }

    function setTopLeftAnchor(globalX, globalY) {
        anchorGlobalX = globalX
        anchorGlobalY = globalY
        anchorImageX = 0.0
        anchorImageY = 0.0
        geometryAnchorActive = true
    }

    function setCenterAnchor(globalX, globalY) {
        anchorGlobalX = globalX
        anchorGlobalY = globalY
        anchorImageX = hostWindow.imageWidth() * 0.5
        anchorImageY = hostWindow.imageHeight() * 0.5
        geometryAnchorActive = true
    }

    function anchorAtCurrentCenter() {
        const geometry = currentNativeGeometry()
        setCenterAnchor(
            geometry.x + geometry.width * 0.5,
            geometry.y + geometry.height * 0.5
        )
    }

    function anchorAtScreenCenter() {
        setCenterAnchor(screenCenterX(), screenCenterY())
    }

    function anchorAtPointer(pointerX, pointerY) {
        if (gestureActive) {
            const globalX = gestureScreenGeometry.x + pointerX
            const globalY = gestureScreenGeometry.y + pointerY
            const imageRect = gestureImageGeometry
            const scaleValue = Math.max(hostWindow.minScale, scale)
            anchorImageX = hostWindow.clamp(
                (globalX - imageRect.x) / scaleValue, 0, hostWindow.imageWidth())
            anchorImageY = hostWindow.clamp(
                (globalY - imageRect.y) / scaleValue, 0, hostWindow.imageHeight())
            anchorGlobalX = globalX
            anchorGlobalY = globalY
            geometryAnchorActive = true
            return
        }
        const geometry = currentNativeGeometry()
        const scaleValue = Math.max(hostWindow.minScale, scale)
        anchorImageX = hostWindow.clamp(
            pointerX / scaleValue,
            0,
            hostWindow.imageWidth()
        )
        anchorImageY = hostWindow.clamp(
            pointerY / scaleValue,
            0,
            hostWindow.imageHeight()
        )
        anchorGlobalX = geometry.x + pointerX
        anchorGlobalY = geometry.y + pointerY
        geometryAnchorActive = true
    }

    function imageX() {
        return 0.0
    }

    function imageY() {
        return 0.0
    }

    function scaleForScreenFraction(screenFraction) {
        if (!hostWindow.hasImage())
            return 1.0

        const boundedFraction = hostWindow.clamp(screenFraction, 0.05, 1.0)
        return hostWindow.scaleForBoundingBox(
            Math.max(1.0, screenWidth() * boundedFraction),
            Math.max(1.0, screenHeight() * boundedFraction)
        )
    }

    function minimumScale() {
        return Math.min(1.0, scaleForScreenFraction(minimumScreenFraction))
    }

    function openScale() {
        return clampScale(Math.min(1.0, scaleForScreenFraction(openScreenFraction)))
    }

    function maximumScale() {
        return scaleForScreenFraction(maximumScreenFraction)
    }

    function clampScale(scaleValue) {
        return hostWindow.clamp(scaleValue, minimumScale(), maximumScale())
    }

    function exactGeometryForScale(scaleValue) {
        const boundedScale = clampScale(scaleValue)
        const desiredX = anchorGlobalX - anchorImageX * boundedScale
        const desiredY = anchorGlobalY - anchorImageY * boundedScale
        const targetWidth = ViewerMath.scaledExtent(
            hostWindow.imageWidth(),
            boundedScale
        )
        const targetHeight = ViewerMath.scaledExtent(
            hostWindow.imageHeight(),
            boundedScale
        )
        const screen = screenGeometry()
        const constrained = ViewerMath.constrainRectToBounds(
            desiredX,
            desiredY,
            targetWidth,
            targetHeight,
            screen.x,
            screen.y,
            screen.width,
            screen.height
        )

        return Qt.rect(
            Math.round(constrained.x),
            Math.round(constrained.y),
            Math.max(1, Math.round(constrained.width)),
            Math.max(1, Math.round(constrained.height))
        )
    }

    function applyExactGeometry(targetGeometry) {
        const currentGeometry = currentNativeGeometry()
        restoreGeometry = targetGeometry
        restoreGeometryValid = true

        if (!geometriesMatch(currentGeometry, targetGeometry)) {
            setGeometrySilently(
                targetGeometry.x,
                targetGeometry.y,
                targetGeometry.width,
                targetGeometry.height
            )
        }

    }

    function applyGeometryForScale(scaleValue) {
        if (hostWindow.fullScreenMode || !hostWindow.hasImage() || gestureActive)
            return
        if (!geometryAnchorActive)
            anchorAtCurrentCenter()

        applyExactGeometry(exactGeometryForScale(scaleValue))
    }

    function ensureState() {
        if (stateValid)
            return

        const initialScale = hostWindow.hasImage() ? openScale() : 1.0
        scale = initialScale
        targetScale = initialScale
        stateValid = hostWindow.hasImage()
    }

    function applyState() {
        if (hostWindow.fullScreenMode || !hostWindow.hasImage())
            return

        ensureState()
        hostWindow.currentScale = scale
        hostWindow.fitMode = false
        hostWindow.panX = 0
        hostWindow.panY = 0
        applyGeometryForScale(scale)
    }

    function applyOpenScale(centerOnScreen) {
        if (hostWindow.fullScreenMode || !hostWindow.hasImage())
            return

        hostWindow.cancelZoomAnimations()
        const initialScale = openScale()
        const currentGeometry = currentNativeGeometry()
        const centerX = centerOnScreen
            ? screenCenterX()
            : currentGeometry.x + currentGeometry.width * 0.5
        const centerY = centerOnScreen
            ? screenCenterY()
            : currentGeometry.y + currentGeometry.height * 0.5

        scale = initialScale
        targetScale = initialScale
        stateValid = true
        setCenterAnchor(centerX, centerY)
        applyState()
        geometryAnchorActive = false
    }

    function zoomToScale(requestedScale, pointerX, pointerY) {
        nativeOps.traceZoom("wheel", requestedScale)
        ensureState()
        if (!stateValid || hostWindow.fullScreenMode)
            return

        const newScale = clampScale(requestedScale)
        if (Math.abs(newScale - targetScale) < 0.0001)
            return

        zoomAnimation.stop()
        gestureIdleTimer.stop()
        // A fresh wheel input supersedes an exact-window prepare.
        // Keep the already visible fixed surface and prepare the new target
        // only after the scroll burst becomes idle again.
        gesturePreparing = false
        gesturePrepareFrameTimer.stop()
        // A stationary pointer should keep its original image coordinate
        // across a wheel burst. Re-anchoring from each rounded display rect
        // can accumulate one-pixel drift after alternating directions.
        const pointerGlobalX = gestureActive
            ? gestureScreenGeometry.x + pointerX
            : currentNativeGeometry().x + pointerX
        const pointerGlobalY = gestureActive
            ? gestureScreenGeometry.y + pointerY
            : currentNativeGeometry().y + pointerY
        if (!gestureActive || Math.abs(pointerGlobalX - anchorGlobalX) > 0.5
                || Math.abs(pointerGlobalY - anchorGlobalY) > 0.5)
            anchorAtPointer(pointerX, pointerY)
        if (!gestureActive && hostWindow.zoomAnimationDurationMs > 0)
            beginGesture()
        targetScale = newScale
        if (hostWindow.zoomAnimationDurationMs <= 0) {
            finishGestureImmediately()
            scale = newScale
            applyState()
            geometryAnchorActive = false
            return
        }

        zoomAnimation.from = scale
        zoomAnimation.to = newScale
        zoomAnimation.start()
    }

    function setScaleImmediately(scaleValue) {
        if (!hostWindow.hasImage())
            return
        zoomAnimation.stop()
        finishGestureImmediately()
        if (!geometryAnchorActive)
            anchorAtCurrentCenter()

        const boundedScale = clampScale(scaleValue)
        targetScale = boundedScale
        scale = boundedScale
        if (!hostWindow.fullScreenMode)
            applyState()
        geometryAnchorActive = false
    }

    function cancelAnimation() {
        zoomAnimation.stop()
        finishGestureImmediately()
        geometryAnchorActive = false
        targetScale = scale
    }

    function prepareForMove() {
        cancelAnimation()
        geometryCaptureReleaseTimer.stop()
        suppressGeometryCapture = false
    }

    function invalidateState() {
        zoomAnimation.stop()
        finishGestureImmediately()
        stateValid = false
        targetScale = 1.0
        geometryAnchorActive = false
    }

    function reset() {
        zoomAnimation.stop()
        finishGestureImmediately()
        geometryCaptureReleaseTimer.stop()
        applyTimer.stop()
        stateValid = false
        scale = 1.0
        targetScale = 1.0
        geometryAnchorActive = false
        suppressGeometryCapture = false
    }

    function restore() {
        if (!restoreGeometryValid) {
            restoreGeometry = defaultGeometry()
            restoreGeometryValid = true
        }

        if (hostWindow.hasImage()) {
            const hadWindowedState = stateValid
            ensureState()

            if (hadWindowedState) {
                setTopLeftAnchor(restoreGeometry.x, restoreGeometry.y)
            } else {
                anchorAtScreenCenter()
            }
            applyState()
            geometryAnchorActive = false
        }

        // Some window managers apply normal-state geometry only after leaving
        // fullscreen, so repeat the exact restore once on the next frame.
        applyTimer.restart()
    }

    property Timer geometryCaptureReleaseTimer: Timer {
        interval: 250
        repeat: false
        onTriggered: controller.suppressGeometryCapture = false
    }

    property NumberAnimation zoomAnimation: NumberAnimation {
        target: controller
        property: "scale"
        duration: controller.hostWindow.zoomAnimationDurationMs
        easing.type: Easing.OutCubic

        onFinished: {
            controller.scale = controller.targetScale
            controller.applyState()
            if (controller.gestureActive)
                controller.gestureIdleTimer.restart()
            else
                controller.geometryAnchorActive = false
        }
    }

    function beginGesture() {
        if (!gestureSurface || !gestureSurface.readyForHandoff
                || !nativeOps.gestureWindowSupported
                || !nativeOps.gestureWindowSupported())
            return false

        const screen = nativeOps.currentScreenGeometry(hostWindow)
        gestureScreenGeometry = Qt.rect(screen.x, screen.y, screen.width, screen.height)
        if (gestureScreenGeometry.width <= 0 || gestureScreenGeometry.height <= 0)
            return false

        const imageRect = exactGeometryForScale(scale)
        beginProgrammaticGeometryChange()
        if (!nativeOps.setWindowInputRectangle(
                gestureSurface,
                imageRect.x - gestureScreenGeometry.x,
                imageRect.y - gestureScreenGeometry.y,
                imageRect.width,
                imageRect.height))
            return false

        gestureActive = true
        nativeOps.traceZoom("gesture-start", scale)
        if (!nativeOps.beginGestureWindows(hostWindow, gestureSurface)) {
            gestureActive = false
            nativeOps.setWindowInputRectangle(gestureSurface, 0, 0, 0, 0)
            return false
        }
        return true
    }

    function prepareGestureSettlement() {
        if (!gestureActive || gesturePreparing)
            return
        gesturePreparing = true
        nativeOps.traceZoom("gesture-prepare", scale)
        gesturePrepareFrames = 0
        gesturePrepareStartedAt = Date.now()
        const imageRect = gestureImageGeometry
        restoreGeometry = imageRect
        restoreGeometryValid = true
        beginProgrammaticGeometryChange()
        setGeometrySilently(imageRect.x, imageRect.y,
                            imageRect.width, imageRect.height)
        gesturePrepareFrameTimer.start()
    }

    function exactFrameSwapped() {
        if (!gestureActive || !gesturePreparing)
            return
        gesturePrepareFrames += 1
        nativeOps.traceZoom("gesture-prepare-frame-" + gesturePrepareFrames, scale)
        if (gesturePrepareFrames < 3 || Date.now() - gesturePrepareStartedAt < 50)
            return
        if (nativeOps.endGestureWindows(gestureSurface, hostWindow)) {
            nativeOps.traceZoom("gesture-settle", scale)
            gesturePrepareFrameTimer.stop()
            gesturePreparing = false
            gestureActive = false
            geometryAnchorActive = false
        }
    }

    function finishGestureImmediately() {
        gestureIdleTimer.stop()
        gesturePrepareFrameTimer.stop()
        if (!gestureActive)
            return
        const imageRect = gestureImageGeometry
        nativeOps.traceZoom("gesture-cancel", scale)
        restoreGeometry = imageRect
        restoreGeometryValid = true
        setGeometrySilently(imageRect.x, imageRect.y,
                            imageRect.width, imageRect.height)
        nativeOps.endGestureWindows(gestureSurface, hostWindow)
        gesturePreparing = false
        gestureActive = false
        geometryAnchorActive = false
    }

    property Timer gestureIdleTimer: Timer {
        interval: 140
        repeat: false
        onTriggered: controller.prepareGestureSettlement()
    }

    property Timer gesturePrepareFrameTimer: Timer {
        interval: 16
        repeat: true
        onTriggered: {
            if (controller.gesturePreparing)
                controller.nativeOps.requestWindowFrame(controller.hostWindow)
            else
                stop()
        }
    }

    property Timer applyTimer: Timer {
        interval: 16
        repeat: false

        onTriggered: {
            if (!controller.hostWindow.visible
                    || controller.hostWindow.fullScreenMode
                    || !controller.restoreGeometryValid) {
                return
            }

            if (controller.hostWindow.hasImage()) {
                controller.ensureState()
                controller.setTopLeftAnchor(
                    controller.restoreGeometry.x,
                    controller.restoreGeometry.y
                )
                controller.applyState()
                controller.geometryAnchorActive = false
            } else {
                controller.setGeometrySilently(
                    Math.round(controller.restoreGeometry.x),
                    Math.round(controller.restoreGeometry.y),
                    Math.max(1, Math.round(controller.restoreGeometry.width)),
                    Math.max(1, Math.round(controller.restoreGeometry.height))
                )
            }
        }
    }
}
