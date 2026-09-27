import QtQuick

QtObject {
    id: controller

    required property var hostWindow
    required property var nativeOps
    required property var zoomController

    property bool active: false
    property bool moved: false
    property bool nativeMoveActive: false
    readonly property real dragThreshold: 3
    property real startCursorX: 0
    property real startCursorY: 0
    property real startWindowX: 0
    property real startWindowY: 0

    function validPoint(point) {
        return point && Number.isFinite(Number(point.x))
            && Number.isFinite(Number(point.y))
    }

    function begin() {
        if (active || hostWindow.fullScreenMode || !hostWindow.hasImage())
            return false

        // Release any programmatic geometry guard before handing movement to
        // the window manager.
        zoomController.prepareForMove()
        const cursor = nativeOps.globalCursorPosition()
        const geometry = nativeOps.currentWindowGeometry(hostWindow)
        if (!validPoint(cursor) || !validPoint(geometry))
            return false

        startCursorX = Number(cursor.x)
        startCursorY = Number(cursor.y)
        startWindowX = Number(geometry.x)
        startWindowY = Number(geometry.y)
        moved = false
        active = true

        // Let the compositor own the interactive move whenever the platform
        // supports it. This keeps pointer sampling and frame pacing out of the
        // QML event loop and is substantially smoother on both X11 and Wayland.
        nativeMoveActive = nativeOps.startSystemMove(hostWindow)
        return true
    }

    function update() {
        if (!active)
            return false

        const cursor = nativeOps.globalCursorPosition()
        if (!validPoint(cursor))
            return false

        const deltaX = Number(cursor.x) - startCursorX
        const deltaY = Number(cursor.y) - startCursorY

        if (nativeMoveActive) {
            const geometry = nativeOps.currentWindowGeometry(hostWindow)
            if (validPoint(geometry)) {
                moved = moved
                    || Math.hypot(deltaX, deltaY) >= dragThreshold
                    || Math.abs(Number(geometry.x) - startWindowX) > 0.5
                    || Math.abs(Number(geometry.y) - startWindowY) > 0.5
            }
            return true
        }

        if (!moved && Math.hypot(deltaX, deltaY) < dragThreshold)
            return true

        moved = true
        const targetX = Math.round(startWindowX + deltaX)
        const targetY = Math.round(startWindowY + deltaY)
        if (!nativeOps.setWindowPosition(hostWindow, targetX, targetY)) {
            hostWindow.x = targetX
            hostWindow.y = targetY
        }
        return true
    }

    function finish() {
        if (!active)
            return

        // Native window managers may cancel the QML pointer grab while their
        // own move grab is still active. Keep tracking until the physical
        // button is released so geometry capture cannot stop mid-drag.
        if (nativeMoveActive && nativeOps.leftMouseButtonPressed())
            return

        finishNow()
    }

    function finishNow() {
        if (!active)
            return

        update()
        const geometryChanged = moved
        active = false
        moved = false
        nativeMoveActive = false
        if (geometryChanged) {
            zoomController.captureGeometry()
        }
    }

    function cancel() {
        active = false
        moved = false
        nativeMoveActive = false
    }

    property Timer nativeMoveTracker: Timer {
        interval: 8
        repeat: true
        running: controller.active && controller.nativeMoveActive

        onTriggered: {
            controller.update()
            if (!controller.nativeOps.leftMouseButtonPressed())
                controller.finishNow()
        }
    }
}
