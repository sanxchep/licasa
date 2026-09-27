import QtQuick

// Adapt viewer controllers to the presentation-only transport tray.
TemporalControls {
    id: temporalControls

    required property var motionSession
    required property var frameSaveCoordinator
    required property var motionExportService
    required property var animationExportService
    required property var animationController
    required property bool imageAnimated
    property var windowManager: null
    property var viewerRoot: null
    property var preferredCoverFrameCoordinator: null
    objectName: "temporalControls"
    anchors.fill: parent

    readonly property bool motionMode: temporalControls.motionSession
        && temporalControls.motionSession.available
        && !(temporalControls.imageAnimated && temporalControls.animationController !== null)

    signal saveFrameAsRequested()
    signal exportMotionAsRequested()
    signal exportAnimationAsRequested()

    function refreshPreferredCoverFrameCoordinator() {
        if (!temporalControls.windowManager || !temporalControls.viewerRoot)
            return
        temporalControls.preferredCoverFrameCoordinator =
            temporalControls.windowManager.ensurePreferredCoverFrameCoordinator(
                temporalControls.viewerRoot)
    }

    function schedulePreferredCoverFrameCoordinatorRefresh() {
        preferredCoverFrameRefreshTimer.restart()
    }

    Timer {
        id: preferredCoverFrameRefreshTimer
        interval: 0
        repeat: false
        onTriggered: temporalControls.refreshPreferredCoverFrameCoordinator()
    }

    Component.onCompleted: refreshPreferredCoverFrameCoordinator()
    onAnimationControllerChanged: schedulePreferredCoverFrameCoordinatorRefresh()
    onImageAnimatedChanged: schedulePreferredCoverFrameCoordinatorRefresh()

    Connections {
        target: temporalControls.motionSession
        enabled: temporalControls.motionSession !== null
        function onAvailableChanged() {
            temporalControls.schedulePreferredCoverFrameCoordinatorRefresh()
        }
    }

    hasTimeline: true
    playing: motionMode
        ? temporalControls.motionSession.playing
        : (temporalControls.animationController ? temporalControls.animationController.playing : false)
    canScrub: motionMode
        ? temporalControls.motionSession.seekable
            && temporalControls.motionSession.durationMs > 0
        : temporalControls.animationController
            && temporalControls.animationController.frameCount > 1
    canStep: !motionMode
        && temporalControls.animationController
        && temporalControls.animationController.frameCount > 1
    saveFrameVisible: motionMode
        && temporalControls.motionSession.active
    canSaveFrame: saveFrameVisible
        && temporalControls.frameSaveCoordinator
        && temporalControls.frameSaveCoordinator.canSaveFrame
    // TemporalControls owns one presentation-level Export slot. Motion Photo
    // and animated-image modes route that intent to separate native services.
    exportMotionVisible: motionMode
        || (temporalControls.imageAnimated
            && temporalControls.animationController)
    canExportMotion: exportMotionVisible
        && (motionMode
            ? (!temporalControls.motionExportService
                || !temporalControls.motionExportService.busy)
            : (!temporalControls.animationExportService
                || !temporalControls.animationExportService.busy))
    coverFrameVisible: temporalControls.preferredCoverFrameCoordinator
        && (temporalControls.preferredCoverFrameCoordinator.canSetCurrent
            || temporalControls.preferredCoverFrameCoordinator.hasPreferred)
    canSetCoverFrame: coverFrameVisible
    coverFramePreferred: temporalControls.preferredCoverFrameCoordinator
        ? temporalControls.preferredCoverFrameCoordinator.hasPreferred : false
    currentIsPreferredCover: temporalControls.preferredCoverFrameCoordinator
        ? temporalControls.preferredCoverFrameCoordinator.currentIsPreferred : false
    position: motionMode
        ? (temporalControls.motionSession.durationMs > 0
            ? Math.max(0.0, Math.min(1.0,
                temporalControls.motionSession.positionMs
                    / temporalControls.motionSession.durationMs))
            : 0.0)
        : (temporalControls.animationController
                && temporalControls.animationController.frameCount > 1
            ? Math.max(0, temporalControls.animationController.currentFrame)
                / (temporalControls.animationController.frameCount - 1)
            : 0.0)
    currentFrame: motionMode ? -1
        : (temporalControls.animationController
            ? temporalControls.animationController.currentFrame : -1)
    frameCount: motionMode ? 0
        : (temporalControls.animationController
            ? temporalControls.animationController.frameCount : 0)
    timeBased: motionMode
    positionMs: motionMode ? temporalControls.motionSession.positionMs : 0
    durationMs: motionMode ? temporalControls.motionSession.durationMs : 0

    onTogglePlaybackRequested: {
        if (motionMode) {
            if (!interactionEnabled)
                return
            temporalControls.motionSession.togglePlayback()
            return
        }
        if (temporalControls.animationController)
            temporalControls.animationController.playing = !temporalControls.animationController.playing
    }

    onSeekRequested: function(normalizedPosition) {
        if (motionMode) {
            if (!interactionEnabled
                    || !temporalControls.motionSession.seekable
                    || temporalControls.motionSession.durationMs <= 0)
                return
            const targetMs = Math.round(Math.max(0.0, Math.min(1.0,
                normalizedPosition)) * temporalControls.motionSession.durationMs)
            temporalControls.motionSession.seek(targetMs)
            return
        }

        if (!temporalControls.animationController || temporalControls.animationController.frameCount <= 1)
            return
        const lastFrame = temporalControls.animationController.frameCount - 1
        const frame = Math.max(0, Math.min(
            lastFrame, Math.round(normalizedPosition * lastFrame)))
        temporalControls.animationController.seekFrame(frame)
    }

    onStepRequested: function(delta) {
        if (motionMode)
            return
        if (!temporalControls.animationController || temporalControls.animationController.frameCount <= 1)
            return
        const lastFrame = temporalControls.animationController.frameCount - 1
        const frame = Math.max(0, Math.min(
            lastFrame, temporalControls.animationController.currentFrame + delta))
        temporalControls.animationController.seekFrame(frame)
    }

    onSaveFrameRequested: {
        if (interactionEnabled && canSaveFrame)
            temporalControls.saveFrameAsRequested()
    }

    onExportMotionRequested: {
        if (!interactionEnabled || !canExportMotion)
            return
        if (motionMode) {
            temporalControls.exportMotionAsRequested()
            return
        }
        temporalControls.exportAnimationAsRequested()
    }

    onCoverFrameRequested: {
        if (!temporalControls.interactionEnabled
                || !temporalControls.preferredCoverFrameCoordinator)
            return
        const coordinator = temporalControls.preferredCoverFrameCoordinator
        if (coordinator.currentIsPreferred
                || (!coordinator.canSetCurrent && coordinator.hasPreferred)) {
            coordinator.clearPreferred()
            return
        }
        if (coordinator.canSetCurrent)
            coordinator.makeCurrentPreferred()
    }
}
