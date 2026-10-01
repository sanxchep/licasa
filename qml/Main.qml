/*!
    Main.qml
    --------
    Main application window for Licasa.

    Author attribution: Sanxchep

    Notes:
    - The top-level Window itself is never faded.
    - Fullscreen and windowed transforms are stored independently.
    - The floating window physically follows the displayed image bounds.
    - Fullscreen image placement and windowed transforms are independent.
*/

import QtQuick
import QtQuick.Window
import "ViewerMath.js" as ViewerMath
import "FileUrls.js" as FileUrls

Window {
    id: root

    required property var windowSession
    required property var imageProbe
    required property var photoAssetProbe
    required property var motionPhotoSession
    property var motionPhotoExportService: null
    property var animationExportService: null
    required property var motionPhotoFrameSaveCoordinator
    required property var formatSupport
    required property var imageSaveService
    property var imageAnimationService: null
    property var animationController: null
    required property var nativeWindowOps
    required property var backgroundModeManager
    required property var viewerPreferences
    required property var windowManager

    width: 1360
    height: 860
    visible: false
    title: "Licasa"
    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"
    onFrameSwapped: {
        windowedZoom.exactFrameSwapped()
        if (imageViewport.activeRequestKey.indexOf("licasa_parallel=preview") >= 0
                && imageViewport.parallelPairId !== lastPresentedParallelPairId) {
            lastPresentedParallelPairId = imageViewport.parallelPairId
            windowManager.markParallelPreviewPresented(lastPresentedParallelPairId)
        }
    }
    onActiveFocusItemChanged: {
        if (fullscreenSideMenu.panelOpen)
            fullscreenSideMenu.revealFocusedItem(activeFocusItem)
        else if (root.editPanelOpen)
            editorSidePanel.revealFocusedItem(activeFocusItem)
    }

    readonly property bool persistentShell: windowSession.persistentShell

    property bool pendingImageReveal: false
    property bool fullScreenMode: true

    property var currentImageUrl: null
    property var residentImageUrl: null
    property size naturalImageSize: Qt.size(0, 0)
    property int imageRevision: 0
    property string currentFileName: ""
    property bool currentImageAnimated: false
    property bool currentPhotoHasMotion: false
    property string motionProbeRequestedUrl: ""
    property string renderedProviderSource: ""
    property string lastPresentedParallelPairId: ""
    property double imageOpenStartedMs: -1

    readonly property real fullscreenBackdropOpacity: viewerPreferences.fullscreenBackgroundOpacity
    readonly property bool transparencyCheckerboardEnabled: viewerPreferences.transparencyCheckerboardEnabled
    readonly property real transparencyCheckerboardOpacity: viewerPreferences.transparencyCheckerboardOpacity
    readonly property bool windowedTransparencyCheckerboardEnabled:
        viewerPreferences.windowedTransparencyCheckerboardEnabled
    readonly property real windowedTransparencyOpacity:
        viewerPreferences.windowedTransparencyOpacity
    readonly property bool animatedPlaybackActive: currentImageAnimated
        && (visible || pendingImageReveal)
        && viewerPreferences.animatedImagePlaybackEnabled
        && !editPanelOpen
        && !editorHasAdjustments
        && !imageSaveService.busy
    readonly property bool smoothImageScalingEnabled:
        viewerPreferences.smoothImageScalingEnabled
    readonly property bool mipmapImageScalingEnabled:
        viewerPreferences.mipmapImageScalingEnabled
    readonly property bool pixelAlignedRenderingEnabled:
        viewerPreferences.pixelAlignedRenderingEnabled
    readonly property bool fullResolutionRenderingEnabled:
        viewerPreferences.fullResolutionRenderingEnabled
    readonly property bool colorManagedRenderingEnabled:
        viewerPreferences.colorManagedRenderingEnabled

    onCurrentImageAnimatedChanged: {
        if (currentImageAnimated && !animationController && imageAnimationService)
            animationController = imageAnimationService.createController(root)
    }

    Binding {
        target: root.animationController
        property: "source"
        value: root.residentImageUrl || ""
        when: root.animationController !== null
    }
    Binding {
        target: root.animationController
        property: "active"
        value: root.animatedPlaybackActive
        when: root.animationController !== null
    }
    Binding {
        target: root.animationController
        property: "speed"
        value: root.viewerPreferences.animationSpeed
        when: root.animationController !== null
    }
    readonly property real cropShieldOpacity: viewerPreferences.cropShieldOpacity

    readonly property real minScale: 0.02
    readonly property real maxScale: 16.0
    readonly property real fullScreenWheelSensitivity: 1.0015
    readonly property real windowedWheelSensitivity: 1.0009
    readonly property real openFitPadding: 0.70
    readonly property real editorFitPadding: 0.96
    readonly property real editorViewportMargin: 24
    readonly property real fullscreenRecoveryGrip: 48
    readonly property int zoomAnimationDurationMs: 160
    readonly property real cornerControlSize: 48
    readonly property real cornerControlMargin: 24

    property real currentScale: 1.0
    property real panX: 0
    property real panY: 0
    property bool fitMode: true
    property real fitPadding: openFitPadding

    property bool previewPhase: false
    property bool imageLimitedToPreview: false
    property bool rawPreviewFirst: false
    property size rawEmbeddedPreviewSize: Qt.size(0, 0)
    property bool rawFastPhase: false
    property bool rawFullDetailRequested: false
    property bool rawMetadataProbePending: false
    property url rawMetadataProbeUrl: ""
    property real rawMetadataProbeDurationMs: 0.0
    readonly property real previewOverscanFactor: 1.20
    readonly property real previewTriggerRatio: 1.15
    readonly property int maxPreviewDecodeExtent: 8192
    readonly property int fullResUpgradeDelayMs: 80

    readonly property real maxWheelDeltaPerEvent: 120

    property real fullscreenSavedScale: 1.0
    property real fullscreenSavedPanX: 0.0
    property real fullscreenSavedPanY: 0.0
    property bool fullscreenSavedFitMode: true
    property real fullscreenSavedFitPadding: openFitPadding
    property bool fullscreenStateValid: false

    property real fullscreenTargetScale: 1.0
    property real zoomAnchorViewportX: 0.0
    property real zoomAnchorViewportY: 0.0
    property real zoomAnchorImageX: 0.0
    property real zoomAnchorImageY: 0.0
    property bool fullscreenZoomAnchorActive: false
    property int screenRevision: 0

    property bool infoOverlayEnabled: true
    property bool smoothMenuMotionEnabled: true

    property bool editPanelOpen: false
    property int editorComputeWarmupRevision: 0

    onEditPanelOpenChanged: {
        if (editPanelOpen)
            stopMotionPhotoPlaybackForLifecycle()
    }
    property real editorWorkspaceInset: 0.0
    property string cropAspectPreset: "free"
    property real cropAspectRatio: 0.0

    property alias editorExposure: editorSession.exposure
    property alias editorContrast: editorSession.contrast
    property alias editorHighlights: editorSession.highlights
    property alias editorShadows: editorSession.shadows
    property alias editorSaturation: editorSession.saturation
    property alias editorVibrance: editorSession.vibrance
    property alias editorWarmth: editorSession.warmth
    property alias editorTint: editorSession.tint
    property alias editorBlur: editorSession.blur
    property alias editorSharpen: editorSession.sharpen
    property alias editorVignette: editorSession.vignette

    property alias editorFlipHorizontal: editorSession.flipHorizontal
    property alias editorFlipVertical: editorSession.flipVertical
    property alias editorQuarterTurns: editorSession.quarterTurns
    property int displayedQuarterTurns: 0

    property alias editorCropX: editorSession.cropX
    property alias editorCropY: editorSession.cropY
    property alias editorCropWidth: editorSession.cropWidth
    property alias editorCropHeight: editorSession.cropHeight
    property real displayedCropX: 0.0
    property real displayedCropY: 0.0
    property real displayedCropWidth: 1.0
    property real displayedCropHeight: 1.0
    property alias editorCropMode: editorSession.cropMode
    property alias draftCropX: editorSession.draftCropX
    property alias draftCropY: editorSession.draftCropY
    property alias draftCropWidth: editorSession.draftCropWidth
    property alias draftCropHeight: editorSession.draftCropHeight

    property alias editorExportFormat: editorSession.exportFormat
    property alias editorExportScale: editorSession.exportScale
    property alias editorExportQuality: editorSession.exportQuality
    property alias compareOriginal: editorSession.compareOriginal
    property string editorSaveStatus: ""
    property string pendingSaveKind: ""
    property int cropEstimateRevision: 0
    property string cropEstimateText: ""

    onResidentImageUrlChanged: invalidateCropEstimate()
    onEditorCropModeChanged: invalidateCropEstimate()
    onDraftCropXChanged: invalidateCropEstimate()
    onDraftCropYChanged: invalidateCropEstimate()
    onDraftCropWidthChanged: invalidateCropEstimate()
    onDraftCropHeightChanged: invalidateCropEstimate()
    onEditorExportFormatChanged: invalidateCropEstimate()
    onEditorExportScaleChanged: invalidateCropEstimate()
    onEditorExportQualityChanged: invalidateCropEstimate()

    readonly property bool editorHasAdjustments: editorSession.hasAdjustments

    function clamp(v, lo, hi) {
        return ViewerMath.clamp(v, lo, hi)
    }

    function boolFlag(v) {
        return v ? "1" : "0"
    }

    function normalizedQuarterTurns(value) {
        const remainder = value % 4
        return remainder < 0 ? remainder + 4 : remainder
    }

    function effectiveEditorQuarterTurns() {
        return compareOriginal ? 0 : normalizedQuarterTurns(editorQuarterTurns)
    }

    function effectiveEditorCropX() {
        return compareOriginal || editorCropMode ? 0.0 : editorCropX
    }

    function effectiveEditorCropY() {
        return compareOriginal || editorCropMode ? 0.0 : editorCropY
    }

    function effectiveEditorCropWidth() {
        return compareOriginal || editorCropMode ? 1.0 : editorCropWidth
    }

    function effectiveEditorCropHeight() {
        return compareOriginal || editorCropMode ? 1.0 : editorCropHeight
    }

    function effectiveDpr() {
        // screenRevision makes the binding reevaluate after a cross-monitor
        // move, even on platforms that keep the same Window object alive.
        return nativeWindowOps.windowDevicePixelRatio(root)
            + screenRevision * 0
    }

    function transformedNaturalWidth(quarterTurns) {
        return normalizedQuarterTurns(quarterTurns) % 2 === 0
            ? naturalImageSize.width
            : naturalImageSize.height
    }

    function transformedNaturalHeight(quarterTurns) {
        return normalizedQuarterTurns(quarterTurns) % 2 === 0
            ? naturalImageSize.height
            : naturalImageSize.width
    }

    function croppedPixelExtent(fullExtent, cropStart, cropExtent) {
        const left = Math.floor(clamp(cropStart, 0.0, 1.0) * fullExtent)
        const right = Math.ceil(clamp(cropStart + cropExtent, 0.0, 1.0) * fullExtent)
        return Math.max(1, right - left)
    }

    function imageWidth() {
        if (naturalImageSize.width > 0 && naturalImageSize.height > 0) {
            return croppedPixelExtent(
                transformedNaturalWidth(displayedQuarterTurns),
                displayedCropX,
                displayedCropWidth
            )
        }
        return Math.max(1, imageViewport.imageImplicitWidth)
    }

    function imageHeight() {
        if (naturalImageSize.width > 0 && naturalImageSize.height > 0) {
            return croppedPixelExtent(
                transformedNaturalHeight(displayedQuarterTurns),
                displayedCropY,
                displayedCropHeight
            )
        }
        return Math.max(1, imageViewport.imageImplicitHeight)
    }

    function imageRight() {
        return imageX() + imageWidth() * currentScale
    }

    function imageBottom() {
        return imageY() + imageHeight() * currentScale
    }

    function windowedCloseButtonX(buttonWidth) {
        const margin = 8
        if (!hasImage())
            return imageViewport.inputRegion.x + imageViewport.inputRegion.width - buttonWidth - margin

        const desired = imageRight() - buttonWidth - margin
        return clamp(desired, margin, imageViewport.width - buttonWidth - margin)
    }

    function windowedCloseButtonY(buttonHeight) {
        const margin = 8
        if (!hasImage())
            return imageViewport.inputRegion.y + margin

        const desired = imageY() + margin
        return clamp(desired, margin, imageViewport.height - buttonHeight - margin)
    }

    function hasImage() {
        return imageViewport.imageStatus === Image.Ready
            && imageWidth() > 0
            && imageHeight() > 0
    }

    function hasAssignedImage() {
        return FileUrls.isLocal(residentImageUrl) || FileUrls.isLocal(currentImageUrl) || hasImage()
    }

    function editorWorkspaceProgress() {
        const targetInset = Math.max(1.0, targetEditorWorkspaceInset(true))
        return root.fullScreenMode
            ? clamp(editorWorkspaceInset / targetInset, 0.0, 1.0)
            : 0.0
    }

    function contentAreaRightMargin() {
        return editorViewportMargin * editorWorkspaceProgress()
    }

    function contentAreaBottomMargin() {
        return editorViewportMargin * editorWorkspaceProgress()
    }

    function contentAreaWidth() {
        return Math.max(
            1.0,
            imageViewport.width - contentAreaLeft() - contentAreaRightMargin()
        )
    }

    function contentAreaHeight() {
        return Math.max(
            1.0,
            imageViewport.height - contentAreaTop() - contentAreaBottomMargin()
        )
    }

    function contentAreaLeft() {
        return root.fullScreenMode ? Math.max(0.0, editorWorkspaceInset) : 0.0
    }

    function contentAreaTop() {
        return editorViewportMargin * editorWorkspaceProgress()
    }

    function scaleForBoundingBox(maxWidth, maxHeight) {
        if (!hasImage())
            return 1.0

        return ViewerMath.fitScale(
            imageWidth(),
            imageHeight(),
            maxWidth,
            maxHeight,
            minScale,
            maxScale
        )
    }

    function fitScaleFor(padding) {
        return scaleForBoundingBox(
            Math.max(1.0, contentAreaWidth() * padding),
            Math.max(1.0, contentAreaHeight() * padding)
        )
    }

    function centeredX(scaleValue) {
        const s = scaleValue === undefined ? currentScale : scaleValue

        return contentAreaLeft() + (contentAreaWidth() - imageWidth() * s) / 2
    }

    function centeredY(scaleValue) {
        const s = scaleValue === undefined ? currentScale : scaleValue

        return contentAreaTop() + (contentAreaHeight() - imageHeight() * s) / 2
    }

    function imageX() {
        if (root.fullScreenMode)
            return centeredX() + panX

        return windowedZoom.imageX()
    }

    function imageY() {
        if (root.fullScreenMode)
            return centeredY() + panY

        return windowedZoom.imageY()
    }

    function buildProviderSource() {
        if (!FileUrls.isLocal(currentImageUrl))
            return ""

        const activeExposure = compareOriginal ? 0.0 : editorExposure
        const activeContrast = compareOriginal ? 0.0 : editorContrast
        const activeHighlights = compareOriginal ? 0.0 : editorHighlights
        const activeShadows = compareOriginal ? 0.0 : editorShadows
        const activeSaturation = compareOriginal ? 0.0 : editorSaturation
        const activeVibrance = compareOriginal ? 0.0 : editorVibrance
        const activeWarmth = compareOriginal ? 0.0 : editorWarmth
        const activeTint = compareOriginal ? 0.0 : editorTint
        const activeBlur = compareOriginal ? 0.0 : editorBlur
        const activeSharpen = compareOriginal ? 0.0 : editorSharpen
        const activeVignette = compareOriginal ? 0.0 : editorVignette
        const activeQuarterTurns = effectiveEditorQuarterTurns()
        const activeFlipH = compareOriginal ? false : editorFlipHorizontal
        const activeFlipV = compareOriginal ? false : editorFlipVertical

        return "image://licasa/"
            + imageRevision
            + "/"
            + encodeURIComponent(FileUrls.localPath(currentImageUrl))
            + "?b=" + activeExposure.toFixed(4)
            + "&c=" + activeContrast.toFixed(4)
            + "&hi=" + activeHighlights.toFixed(4)
            + "&sh=" + activeShadows.toFixed(4)
            + "&s=" + activeSaturation.toFixed(4)
            + "&vib=" + activeVibrance.toFixed(4)
            + "&w=" + activeWarmth.toFixed(4)
            + "&t=" + activeTint.toFixed(4)
            + "&blur=" + activeBlur.toFixed(4)
            + "&sharp=" + activeSharpen.toFixed(4)
            + "&vig=" + activeVignette.toFixed(4)
            + "&r=" + activeQuarterTurns
            + "&fh=" + boolFlag(activeFlipH)
            + "&fv=" + boolFlag(activeFlipV)
            + "&cx=" + effectiveEditorCropX().toFixed(6)
            + "&cy=" + effectiveEditorCropY().toFixed(6)
            + "&cw=" + effectiveEditorCropWidth().toFixed(6)
            + "&ch=" + effectiveEditorCropHeight().toFixed(6)
            + "&cm=" + boolFlag(colorManagedRenderingEnabled)
            + "&ew=" + editorComputeWarmupRevision
    }

    function previewSourceWidth() {
        const cropBoost = 1.0 / Math.max(
            0.05,
            Math.min(effectiveEditorCropWidth(), effectiveEditorCropHeight())
        )
        return Math.min(maxPreviewDecodeExtent, Math.max(1, Math.ceil(
            imageViewport.width * effectiveDpr() * previewOverscanFactor * cropBoost)))
    }

    function previewSourceHeight() {
        const cropBoost = 1.0 / Math.max(
            0.05,
            Math.min(effectiveEditorCropWidth(), effectiveEditorCropHeight())
        )
        return Math.min(maxPreviewDecodeExtent, Math.max(1, Math.ceil(
            imageViewport.height * effectiveDpr() * previewOverscanFactor * cropBoost)))
    }

    function shouldUsePreview() {
        if (naturalImageSize.width <= 0 || naturalImageSize.height <= 0)
            return true

        return naturalImageSize.width > previewSourceWidth() * previewTriggerRatio
            || naturalImageSize.height > previewSourceHeight() * previewTriggerRatio
    }

    function parallelOpeningWorthwhile() {
        const name = FileUrls.fileName(residentImageUrl).toLowerCase()
        const dot = name.lastIndexOf(".")
        const extension = dot >= 0 ? name.substring(dot + 1) : ""
        // A full-native-raster preview makes two large codec jobs compete for
        // CPU and memory. These codecs overlap only with an embedded preview
        // large enough for the current viewport.
        if (extension === "avif" || extension === "avifs"
                || extension === "heic" || extension === "heif"
                || extension === "hif" || extension === "jxl")
            return rawEmbeddedPreviewSize.width >= previewSourceWidth()
                && rawEmbeddedPreviewSize.height >= previewSourceHeight()
        return true
    }

    function currentImageMegapixels() {
        if (naturalImageSize.width <= 0 || naturalImageSize.height <= 0)
            return 0.0
        return naturalImageSize.width * naturalImageSize.height / 1000000.0
    }

    function syncCurrentImageResourceLimit() {
        if (!FileUrls.isLocal(residentImageUrl))
            return

        const rawInfo = imageProbe.inspect(residentImageUrl)
        rawEmbeddedPreviewSize = rawInfo.previewSize || Qt.size(0, 0)
        const limited = rawInfo.withinBudget !== true
        if (limited === imageLimitedToPreview)
            return

        imageLimitedToPreview = limited
        currentImageAnimated = rawInfo.animated === true && !limited
        fullResTimer.stop()

        if (rawPreviewFirst && !rawFullDetailRequested)
            previewPhase = true
        else if (currentImageAnimated)
            previewPhase = true
        else if (limited)
            previewPhase = true
        else
            previewPhase = fullResolutionRenderingEnabled ? false : shouldUsePreview()

        imageRevision += 1
        refreshEditedPreviewSource()
    }

    function cancelZoomAnimations() {
        windowedZoom.cancelAnimation()

        fullscreenZoomAnimation.stop()
        fullscreenZoomAnchorActive = false
        fullscreenTargetScale = currentScale
    }

    function applyFit(padding) {
        if (!hasImage())
            return

        fullscreenZoomAnimation.stop()
        fullscreenZoomAnchorActive = false
        fitMode = true
        fitPadding = padding
        currentScale = fitScaleFor(fitPadding)
        fullscreenTargetScale = currentScale
        panX = 0
        panY = 0
    }

    function defaultFullscreenFitPadding() {
        return editPanelOpen ? editorFitPadding : openFitPadding
    }

    function resetFullscreenView() {
        if (!hasImage())
            return

        applyFit(defaultFullscreenFitPadding())
    }

    function fullscreenImageNeedsRecovery() {
        if (!hasImage())
            return false

        return ViewerMath.needsViewportRecovery(
            imageX(),
            imageY(),
            imageWidth() * currentScale,
            imageHeight() * currentScale,
            contentAreaLeft(),
            contentAreaTop(),
            contentAreaWidth(),
            contentAreaHeight(),
            fullscreenRecoveryGrip
        )
    }

    function clampPanToViewport() {
        if (!hasImage() || !root.fullScreenMode)
            return

        const scaledW = imageWidth() * currentScale
        const scaledH = imageHeight() * currentScale
        const cx = centeredX()
        const cy = centeredY()
        let ix = cx + panX
        let iy = cy + panY

        // Check the raw dragged position before constraining it. Otherwise a
        // completely lost image is converted into a hard-to-find sliver and
        // can no longer be distinguished from an intentional edge placement.
        if (fullscreenImageNeedsRecovery()) {
            resetFullscreenView()
            return
        }

        // Picasa-style free placement: smaller images may be moved anywhere in
        // fullscreen instead of snapping back to center. Keep only a small grab
        // strip visible so an image cannot be lost completely off-screen.
        ix = contentAreaLeft() + ViewerMath.freeImagePosition(
            ix - contentAreaLeft(), scaledW, contentAreaWidth(), fullscreenRecoveryGrip)
        iy = contentAreaTop() + ViewerMath.freeImagePosition(
            iy - contentAreaTop(), scaledH, contentAreaHeight(), fullscreenRecoveryGrip)

        panX = ix - cx
        panY = iy - cy
    }

    function recoverFullscreenImageIfLost() {
        if (!fullScreenMode || !hasImage())
            return

        if (fullscreenImageNeedsRecovery())
            resetFullscreenView()
    }

    function targetEditorWorkspaceInset(open) {
        return open ? editorSidePanel.workspaceBoundaryX : 0.0
    }

    function setEditPanelOpen(open) {
        const shouldOpen = open && root.fullScreenMode && root.hasAssignedImage()
        if (editPanelOpen === shouldOpen
                && Math.abs(editorWorkspaceInset - targetEditorWorkspaceInset(shouldOpen)) < 0.5) {
            return
        }

        cancelZoomAnimations()
        editorLayoutAnimation.stop()
        if (!shouldOpen) {
            editorSession.cancelCrop()
            cropAspectPreset = "free"
            cropAspectRatio = 0.0
        } else {
            fullscreenSideMenu.panelOpen = false
            notificationCenter.historyOpen = false
        }
        const openingEditor = shouldOpen && !editPanelOpen
        editPanelOpen = shouldOpen

        if (!hasImage()) {
            editorWorkspaceInset = targetEditorWorkspaceInset(shouldOpen)
            return
        }

        if (openingEditor) {
            // The current ready slot remains visible while the provider warms
            // CUDA/OpenCL on its decode worker. The first slider movement then
            // avoids paying driver/context/JIT startup synchronously.
            editorComputeWarmupRevision += 1
            refreshEditedPreviewSource()
        }

        const targetInset = targetEditorWorkspaceInset(shouldOpen)
        const targetPadding = shouldOpen ? editorFitPadding : openFitPadding
        const targetMargin = shouldOpen ? editorViewportMargin : 0.0
        const targetWidth = Math.max(
            1.0,
            imageViewport.width - targetInset - targetMargin
        )
        const targetHeight = Math.max(
            1.0,
            imageViewport.height - targetMargin * 2
        )
        const targetScale = scaleForBoundingBox(
            targetWidth * targetPadding,
            targetHeight * targetPadding
        )

        fitMode = true
        fitPadding = targetPadding
        fullscreenTargetScale = targetScale

        editorInsetAnimation.from = editorWorkspaceInset
        editorInsetAnimation.to = targetInset
        editorScaleAnimation.from = currentScale
        editorScaleAnimation.to = targetScale
        editorPanXAnimation.from = panX
        editorPanXAnimation.to = 0.0
        editorPanYAnimation.from = panY
        editorPanYAnimation.to = 0.0
        editorLayoutAnimation.start()
    }

    function closeEditorImmediately() {
        editorLayoutAnimation.stop()
        editorSession.cancelCrop()
        cropAspectPreset = "free"
        cropAspectRatio = 0.0
        editPanelOpen = false
        editorWorkspaceInset = 0.0

        if (hasImage() && fullScreenMode && fitMode)
            applyFit(openFitPadding)
    }

    function promoteFullRes() {
        // The initial RAW preview is intentionally allowed to start before the
        // synchronous LibRaw metadata probe. Resolve that deferred probe before
        // any full-detail request so the resource-policy limit is known before
        // sensor development can begin. Opening a RAW always schedules this
        // promotion after first paint; zoom is no longer the trigger.
        if (rawMetadataProbePending)
            finishDeferredRawMetadataProbe()

        if ((!previewPhase && !rawFastPhase) || imageLimitedToPreview || animatedPlaybackActive)
            return

        if (rawPreviewFirst && !rawFullDetailRequested)
            rawFullDetailRequested = true

        fullResTimer.stop()
        rawFastPhase = false
        previewPhase = false
    }

    function requestRawFastFallback() {
        if (!rawPreviewFirst || rawFastPhase || rawFullDetailRequested
                || imageLimitedToPreview || animatedPlaybackActive)
            return

        fullResTimer.stop()
        previewPhase = false
        rawFastPhase = true
    }

    function scheduleEditedPreviewRefresh() {
        if (!hasAssignedImage())
            return

        editorSaveStatus = ""
        // Keep an edit refresh on whichever RAW base is already decoded. The
        // open-time promotion will resume after that preview refresh becomes
        // ready; never restart sensor development separately for every slider.
        fullResTimer.stop()
        editorPreviewTimer.restart()
    }

    function refreshEditedPreviewSource() {
        renderedProviderSource = buildProviderSource()
    }

    function rememberCurrentWindowedGeometry() {
        windowedZoom.captureGeometry()
    }

    function captureFullscreenViewState() {
        if (!root.fullScreenMode || !hasImage())
            return

        fullscreenZoomAnimation.stop()
        fullscreenZoomAnchorActive = false
        fullscreenSavedScale = currentScale
        fullscreenSavedPanX = panX
        fullscreenSavedPanY = panY
        fullscreenSavedFitMode = fitMode
        fullscreenSavedFitPadding = fitPadding
        fullscreenStateValid = true
        fullscreenTargetScale = currentScale
    }

    function restoreFullscreenViewState() {
        if (!root.fullScreenMode || !hasImage())
            return

        fullscreenZoomAnimation.stop()
        fullscreenZoomAnchorActive = false

        if (!fullscreenStateValid || fullscreenSavedFitMode) {
            applyFit(fullscreenStateValid ? fullscreenSavedFitPadding : openFitPadding)
            return
        }

        fitMode = false
        fitPadding = fullscreenSavedFitPadding
        currentScale = clamp(fullscreenSavedScale, minScale, maxScale)
        fullscreenTargetScale = currentScale
        panX = fullscreenSavedPanX
        panY = fullscreenSavedPanY
        clampPanToViewport()
    }

    function syncWindowVisibility() {
        if (!root.visible)
            return

        nativeWindowOps.showWindow(root, root.fullScreenMode)
    }

    function setFullScreenState(enabled) {
        if (windowedMove.active)
            windowedMove.finish()

        if (enabled === root.fullScreenMode) {
            syncWindowVisibility()
            root.raise()
            root.requestActivate()
            return
        }

        if (enabled) {
            cancelZoomAnimations()
            rememberCurrentWindowedGeometry()
            editPanelOpen = false
            editorWorkspaceInset = 0.0
            root.fullScreenMode = true
            fullscreenApplyTimer.restart()
        } else {
            closeEditorImmediately()
            captureFullscreenViewState()
            if (root.compareOriginal) {
                root.compareOriginal = false
                root.scheduleEditedPreviewRefresh()
            }
            windowedZoom.beginProgrammaticGeometryChange()
            root.fullScreenMode = false
            windowedZoom.restore()
        }

        syncWindowVisibility()
        root.raise()
        root.requestActivate()
    }

    function beginEditorInteraction() {
        editorSession.beginInteraction()
    }

    function finishEditorInteraction() {
        editorSession.finishInteraction()
    }

    function undoEditorChange() {
        editorSession.undo()
    }

    function redoEditorChange() {
        editorSession.redo()
    }

    function beginCropMode() {
        if (!hasImage() || editorCropMode)
            return

        fitMode = true
        fitPadding = editorFitPadding
        cropAspectPreset = "free"
        cropAspectRatio = 0.0
        editorSession.beginCrop()
    }

    function updateDraftCrop(x, y, width, height) {
        editorSession.updateDraftCrop(x, y, width, height)
    }

    function applyCropPreset(name) {
        if (!editorCropMode)
            beginCropMode()
        if (!editorCropMode)
            return

        const transformedWidth = transformedNaturalWidth(editorQuarterTurns)
        const transformedHeight = transformedNaturalHeight(editorQuarterTurns)

        if (name === "free") {
            cropAspectPreset = "free"
            cropAspectRatio = 0.0
            return
        }

        if (name === "full") {
            cropAspectPreset = "free"
            cropAspectRatio = 0.0
            editorSession.applyCropPreset("original", transformedWidth, transformedHeight)
            return
        }

        cropAspectPreset = name
        cropAspectRatio = name === "original"
            ? transformedWidth / Math.max(1.0, transformedHeight)
            : editorSession.cropPresetRatio(name)
        editorSession.applyCropRatio(
            cropAspectRatio,
            transformedWidth,
            transformedHeight
        )
    }

    function applyCustomCropAspectRatio(requestedRatio) {
        if (!Number.isFinite(requestedRatio) || requestedRatio <= 0)
            return
        if (!editorCropMode)
            beginCropMode()
        if (!editorCropMode)
            return

        const transformedWidth = transformedNaturalWidth(editorQuarterTurns)
        const transformedHeight = transformedNaturalHeight(editorQuarterTurns)
        cropAspectPreset = "custom"
        cropAspectRatio = clamp(requestedRatio, 0.001, 1000.0)
        editorSession.applyCropRatio(
            cropAspectRatio,
            transformedWidth,
            transformedHeight
        )
    }

    function applyDraftCrop() {
        editorSession.applyDraftCrop()
        cropAspectPreset = "free"
        cropAspectRatio = 0.0
    }

    function cancelCropMode() {
        editorSession.cancelCrop()
        cropAspectPreset = "free"
        cropAspectRatio = 0.0
    }

    function draftCropOutputWidth() {
        if (naturalImageSize.width <= 0 || naturalImageSize.height <= 0)
            return 0
        return croppedPixelExtent(
            transformedNaturalWidth(editorQuarterTurns),
            draftCropX,
            draftCropWidth
        )
    }

    function draftCropOutputHeight() {
        if (naturalImageSize.width <= 0 || naturalImageSize.height <= 0)
            return 0
        return croppedPixelExtent(
            transformedNaturalHeight(editorQuarterTurns),
            draftCropY,
            draftCropHeight
        )
    }

    function formatFileSize(byteCount) {
        if (!Number.isFinite(byteCount) || byteCount <= 0)
            return ""

        const units = ["B", "KiB", "MiB", "GiB"]
        let value = byteCount
        let unitIndex = 0
        while (value >= 1024 && unitIndex < units.length - 1) {
            value /= 1024
            unitIndex += 1
        }

        const decimals = unitIndex === 0 || value >= 100 ? 0 : value >= 10 ? 1 : 2
        return value.toFixed(decimals) + " " + units[unitIndex]
    }

    function invalidateCropEstimate() {
        cropEstimateRevision += 1
        cropSizeEstimateTimer.stop()
        if (!editorCropMode || !FileUrls.isLocal(residentImageUrl)) {
            cropEstimateText = ""
            imageSaveService.cancelSizeEstimate()
            return
        }
        cropEstimateText = "calculating…"
        cropSizeEstimateTimer.restart()
    }

    function draftCropEstimatedFileSize() {
        return cropEstimateText
    }

    Timer {
        id: cropSizeEstimateTimer
        interval: 500
        repeat: false
        onTriggered: {
            if (!root.editorCropMode || !FileUrls.isLocal(root.residentImageUrl))
                return
            const edits = root.editorEditValues()
            edits.cropX = root.draftCropX
            edits.cropY = root.draftCropY
            edits.cropWidth = root.draftCropWidth
            edits.cropHeight = root.draftCropHeight
            root.imageSaveService.estimateSize(
                root.residentImageUrl,
                root.cropEstimateRevision,
                edits,
                editorSession.exportValues()
            )
        }
    }

    Connections {
        target: editorSession
        function onEditsChanged() { root.invalidateCropEstimate() }
    }

    function editedOutputWidth(scaleValue) {
        if (naturalImageSize.width <= 0 || naturalImageSize.height <= 0)
            return 0
        return Math.max(1, Math.round(croppedPixelExtent(
            transformedNaturalWidth(editorQuarterTurns), editorCropX, editorCropWidth)
            * scaleValue))
    }

    function editedOutputHeight(scaleValue) {
        if (naturalImageSize.width <= 0 || naturalImageSize.height <= 0)
            return 0
        return Math.max(1, Math.round(croppedPixelExtent(
            transformedNaturalHeight(editorQuarterTurns), editorCropY, editorCropHeight)
            * scaleValue))
    }

    function resetEditorAdjustments() {
        editorSaveStatus = ""
        editorSession.reset()
    }

    function applyEditorPreset(name) {
        editorSession.applyPreset(name)
    }

    function clearEditorSessionState(resetExportSettings) {
        editorSession.clear(resetExportSettings)
        cropAspectPreset = "free"
        cropAspectRatio = 0.0
        displayedQuarterTurns = 0
        displayedCropX = 0.0
        displayedCropY = 0.0
        displayedCropWidth = 1.0
        displayedCropHeight = 1.0
    }

    function stopMotionPhotoPlaybackForLifecycle() {
        // Stop even before the backend reports active: first-play setup is
        // asynchronous and a lifecycle transition must cancel in-flight media.
        if (motionPhotoSession && motionPhotoSession.available)
            motionPhotoSession.stop()
    }

    function clearImageState() {
        imageOpenStartedMs = -1
        pendingSaveKind = ""
        stopMotionPhotoPlaybackForLifecycle()
        fullResTimer.stop()
        editorPreviewTimer.stop()
        cancelZoomAnimations()
        previewPhase = false

        currentImageUrl = null
        residentImageUrl = null
        naturalImageSize = Qt.size(0, 0)
        currentFileName = ""
        currentImageAnimated = false
        currentPhotoHasMotion = false
        motionProbeRequestedUrl = ""
        photoAssetProbe.cancel()
        imageLimitedToPreview = false
        rawPreviewFirst = false
        rawEmbeddedPreviewSize = Qt.size(0, 0)
        rawFastPhase = false
        rawFullDetailRequested = false
        rawMetadataProbePending = false
        rawMetadataProbeUrl = ""
        rawMetadataProbeDurationMs = 0.0
        rawMetadataProbeTimer.stop()
        imageViewport.releaseStaticImages()

        currentScale = 1.0
        panX = 0
        panY = 0
        fitMode = true
        fitPadding = openFitPadding
        windowedMove.cancel()
        windowedZoom.reset()
        fullscreenSavedScale = 1.0
        fullscreenSavedPanX = 0.0
        fullscreenSavedPanY = 0.0
        fullscreenSavedFitMode = true
        fullscreenSavedFitPadding = openFitPadding
        fullscreenStateValid = false
        fullscreenTargetScale = 1.0
        pendingImageReveal = false

        clearEditorSessionState(true)
        editorLayoutAnimation.stop()
        editPanelOpen = false
        editorWorkspaceInset = 0.0
        editorSaveStatus = ""

        imageRevision += 1
        refreshEditedPreviewSource()

        if (openDialogLoader.active) {
            openDialogLoader.source = ""
            openDialogLoader.active = false
        }
        if (saveDialogLoader.active) {
            saveDialogLoader.source = ""
            saveDialogLoader.active = false
        }
        if (frameSaveDialogLoader.active) {
            frameSaveDialogLoader.source = ""
            frameSaveDialogLoader.active = false
        }
        notificationCenter.endSession()
        windowManager.releasePictureResources()
    }

    function enterFullScreen() {
        setFullScreenState(true)
    }

    function exitFullScreenToImage() {
        if (!root.visible)
            return

        if (!hasImage()) {
            setFullScreenState(false)
            return
        }

        promoteFullRes()
        setFullScreenState(false)
    }

    function toggleFullScreen() {
        if (!root.visible)
            return

        setFullScreenState(!root.fullScreenMode)
    }

    function prepareForImageOpen() {
        if (!root.visible) {
            pendingImageReveal = true
            root.fullScreenMode = viewerPreferences.startInFullscreen
            return
        }

        setFullScreenState(viewerPreferences.startInFullscreen)
    }

    function revealLoadedImage() {
        // Keep animation active throughout the hidden-to-visible handoff.
        // Clearing pendingImageReveal first briefly deactivates its controller
        // from inside Image.onStatusChanged, resetting the source binding and
        // starting a second native decode of the same frame.
        root.visible = true
        pendingImageReveal = false
        syncWindowVisibility()
        root.raise()
        root.requestActivate()
    }

    function looksLikeRaw(url) {
        const name = FileUrls.fileName(url).toLowerCase()
        const dot = name.lastIndexOf(".")
        if (dot < 0 || dot === name.length - 1)
            return false

        const extension = name.substring(dot + 1)
        return ["dng", "raw", "cr2", "cr3", "nef", "nrw", "arw",
                "srw", "rw2", "pef", "orf", "raf"].indexOf(extension) >= 0
    }

    function readImageMetadata(url, preserveRawStage) {
        const info = imageProbe.inspect(url)
        naturalImageSize = info.size || Qt.size(0, 0)
        rawPreviewFirst = info.previewFirst === true || (preserveRawStage && rawPreviewFirst)
        rawEmbeddedPreviewSize = info.previewSize || Qt.size(0, 0)
        if (!preserveRawStage) {
            rawFastPhase = false
            rawFullDetailRequested = false
        }
        imageLimitedToPreview = info.withinBudget !== true
        currentImageAnimated = info.animated === true && !imageLimitedToPreview
        // RAW metadata is advisory. Always try the embedded preview first;
        // only a failed decode promotes it to the bounded raw-fast fallback.
    }

    function finishDeferredRawMetadataProbe() {
        if (!rawMetadataProbePending)
            return

        const probeUrl = rawMetadataProbeUrl
        rawMetadataProbePending = false
        rawMetadataProbeUrl = ""
        if (!FileUrls.isLocal(probeUrl)
                || String(probeUrl) !== String(residentImageUrl))
            return

        // First pixels have already reached ImageViewport. Metadata can now do
        // LibRaw camera/thumbnail discovery without holding the initial grey
        // loading surface hostage. Preserve whichever RAW stage is currently
        // visible while applying dimensions and policy information.
        const probeStartedMs = Date.now()
        readImageMetadata(probeUrl, true)
        rawMetadataProbeDurationMs = Math.max(0, Date.now() - probeStartedMs)

        if (imageLimitedToPreview && imageViewport.imageStatus === Image.Ready) {
            const openingDurationMs = imageOpenStartedMs >= 0
                ? Math.max(0, Date.now() - imageOpenStartedMs) : -1
            imageOpenStartedMs = -1
            notificationCenter.finish("image", "success", "Preview ready",
                                      currentFileName + (openingDurationMs >= 0
                                          ? " · " + openingDurationMs + " ms" : "")
                                          + " · Full resolution exceeds the image memory limit")
        }

        if (fitMode) {
            if (fullScreenMode)
                applyFit(fitPadding)
            else
                windowedZoom.applyOpenScale(false)
        }
    }

    function loadImageUrl(u, replaceCurrent) {
        if (!FileUrls.isLocal(u) || !formatSupport.canOpen(u))
            return

        if (!replaceCurrent && hasAssignedImage() && imageViewport.imageStatus !== Image.Error) {
            windowManager.openInNewWindow(u)
            return
        }

        if (hasAssignedImage()) {
            stopMotionPhotoPlaybackForLifecycle()
            windowManager.releasePictureResources()
        }

        notificationCenter.startSession()
        imageOpenStartedMs = Date.now()
        notificationCenter.begin("image", "Opening image", FileUrls.fileName(u), 300)
        if (!replaceCurrent)
            prepareForImageOpen()

        residentImageUrl = u
        if (looksLikeRaw(u)) {
            // Do not synchronously probe LibRaw before the asynchronous preview
            // provider starts. Prime a conservative preview-first state and
            // collect sensor metadata after Image.Ready/first paint.
            naturalImageSize = Qt.size(0, 0)
            rawPreviewFirst = true
            rawEmbeddedPreviewSize = Qt.size(0, 0)
            rawFastPhase = false
            rawFullDetailRequested = false
            imageLimitedToPreview = false
            currentImageAnimated = false
            rawMetadataProbePending = true
            rawMetadataProbeUrl = u
            rawMetadataProbeDurationMs = 0.0
        } else {
            rawMetadataProbePending = false
            rawMetadataProbeUrl = ""
            rawMetadataProbeDurationMs = 0.0
            readImageMetadata(u, false)
        }
        previewPhase = imageLimitedToPreview
            || (rawPreviewFirst && !rawFastPhase)
            || currentImageAnimated
            || shouldUsePreview()
        fullResTimer.stop()

        fitMode = true
        fitPadding = openFitPadding
        panX = 0
        panY = 0

        cancelZoomAnimations()
        windowedZoom.invalidateState()
        fullscreenStateValid = false
        fullscreenSavedFitMode = true
        fullscreenSavedFitPadding = openFitPadding

        clearEditorSessionState(true)
        editorLayoutAnimation.stop()
        editPanelOpen = false
        editorWorkspaceInset = 0.0
        editorSaveStatus = ""

        currentImageUrl = residentImageUrl
        currentFileName = FileUrls.fileName(residentImageUrl)
        currentPhotoHasMotion = false
        motionProbeRequestedUrl = ""
        photoAssetProbe.cancel()
        imageRevision += 1
        refreshEditedPreviewSource()
    }

    function browseImage(direction) {
        if (!root.fullScreenMode || root.editPanelOpen || fullscreenSideMenu.panelOpen
                || !FileUrls.isLocal(root.residentImageUrl) || imageSaveService.busy)
            return
        const adjacent = formatSupport.adjacentImage(root.residentImageUrl, direction)
        if (FileUrls.isLocal(adjacent))
            loadImageUrl(adjacent, true)
    }

    function moveImage(dx, dy) {
        if (!root.fullScreenMode || !hasImage())
            return
        cancelZoomAnimations()
        fitMode = false
        panX += dx
        panY += dy
        panX = contentAreaLeft() + ViewerMath.freeImagePosition(
            imageX() - contentAreaLeft(), imageWidth() * currentScale,
            contentAreaWidth(), fullscreenRecoveryGrip) - centeredX()
        panY = contentAreaTop() + ViewerMath.freeImagePosition(
            imageY() - contentAreaTop(), imageHeight() * currentScale,
            contentAreaHeight(), fullscreenRecoveryGrip) - centeredY()
    }

    function requestPhotoAssetProbeIfNeeded() {
        if (!FileUrls.isLocal(residentImageUrl) || imageViewport.imageStatus !== Image.Ready)
            return

        const key = residentImageUrl.toString()
        if (motionProbeRequestedUrl === key)
            return

        // The first raster is already ready before optional phone-media
        // metadata work starts. This keeps normal JPEG first-preview latency
        // independent of XMP/container probing.
        motionProbeRequestedUrl = key
        photoAssetProbe.request(residentImageUrl)
    }

    function fitToWindow() {
        if (!hasImage())
            return

        promoteFullRes()

        if (fullScreenMode) {
            applyFit(1.0)
            return
        }

        windowedZoom.applyOpenScale(false)
    }

    function actualSize() {
        if (!hasImage())
            return

        promoteFullRes()
        cancelZoomAnimations()
        fitMode = false
        panX = 0
        panY = 0

        if (fullScreenMode) {
            currentScale = 1.0
            fullscreenTargetScale = currentScale
            clampPanToViewport()
            return
        }

        windowedZoom.ensureState()
        windowedZoom.setScaleImmediately(1.0)
    }

    function zoomToScale(requestedScale, px, py) {
        if (!hasImage())
            return

        if (!rawPreviewFirst || requestedScale > currentScale)
            promoteFullRes()
        fitMode = false

        if (!fullScreenMode) {
            windowedZoom.zoomToScale(requestedScale, px, py)
            return
        }

        fullscreenZoomAnimation.stop()
        fullscreenZoomAnchorActive = false

        const oldScale = currentScale
        const newScale = clamp(requestedScale, minScale, maxScale)
        if (Math.abs(newScale - fullscreenTargetScale) < 0.0001)
            return

        const oldImageX = imageX()
        const oldImageY = imageY()

        zoomAnchorViewportX = px
        zoomAnchorViewportY = py
        zoomAnchorImageX = clamp((px - oldImageX) / oldScale, 0, imageWidth())
        zoomAnchorImageY = clamp((py - oldImageY) / oldScale, 0, imageHeight())
        fullscreenTargetScale = newScale
        fullscreenZoomAnchorActive = true

        fullscreenZoomAnimation.from = oldScale
        fullscreenZoomAnimation.to = fullscreenTargetScale
        fullscreenZoomAnimation.start()
    }

    function zoomByFactor(factor, px, py) {
        const baseScale = root.fullScreenMode
            ? fullscreenTargetScale
            : (windowedZoom.stateValid ? windowedZoom.targetScale : currentScale)

        zoomToScale(baseScale * factor, px, py)
    }

    function zoomFromKeyboard(factor) {
        if (!root.fullScreenMode && windowedZoom.gestureActive) {
            const imageRect = windowedZoom.gestureImageGeometry
            zoomByFactor(factor,
                imageRect.x + imageRect.width * 0.5 - gestureWindow.x,
                imageRect.y + imageRect.height * 0.5 - gestureWindow.y)
            return
        }
        zoomByFactor(factor, imageViewport.width * 0.5, imageViewport.height * 0.5)
    }

    function openFileDialog() {
        if (openDialogLoader.dialog) {
            openDialogLoader.dialog.open()
            return
        }

        if (!openDialogLoader.active) {
            openDialogLoader.setSource(
                "qrc:/Licasa/qml/OpenDialog.qml",
                { "formatSupport": root.formatSupport }
            )
            openDialogLoader.active = true
        }
    }

    function suggestedSaveAsUrl() {
        return FileUrls.suggestedEditedCopy(residentImageUrl, editorExportFormat)
    }

    function suggestedFrameSaveAsUrl() {
        return FileUrls.suggestedCopy(residentImageUrl, "frame", "png")
    }

    function saveCurrentMotionFrame(destinationUrl) {
        if (!FileUrls.isLocal(residentImageUrl)
                || !FileUrls.isLocal(destinationUrl)
                || !motionPhotoFrameSaveCoordinator
                || !motionPhotoFrameSaveCoordinator.canSaveFrame) {
            return
        }

        pendingSaveKind = "frame"
        motionPhotoFrameSaveCoordinator.saveCurrentFrame(
            residentImageUrl,
            destinationUrl,
            { "scale": 1.0, "quality": 95 }
        )
    }

    function openFrameSaveAsDialog() {
        if (!hasAssignedImage()
                || imageSaveService.busy
                || !motionPhotoFrameSaveCoordinator
                || !motionPhotoFrameSaveCoordinator.canSaveFrame) {
            return
        }

        const suggestion = suggestedFrameSaveAsUrl()
        if (frameSaveDialogLoader.dialog) {
            frameSaveDialogLoader.dialog.suggestedFile = suggestion
            frameSaveDialogLoader.dialog.open()
            return
        }

        if (!frameSaveDialogLoader.active) {
            frameSaveDialogLoader.setSource(
                "qrc:/Licasa/qml/FrameSaveDialog.qml",
                {
                    "formatSupport": root.formatSupport,
                    "suggestedFile": suggestion
                }
            )
            frameSaveDialogLoader.active = true
        }
    }

    function editorEditValues() {
        return editorSession.editValues()
    }

    function startImageSave(destinationUrl, exportValues) {
        if (!FileUrls.isLocal(residentImageUrl) || !FileUrls.isLocal(destinationUrl) || editorCropMode)
            return

        editorSaveStatus = ""
        stopMotionPhotoPlaybackForLifecycle()
        pendingSaveKind = "image"
        imageSaveService.save(
            residentImageUrl,
            destinationUrl,
            editorEditValues(),
            exportValues
        )
    }

    function saveEditedImage(destinationUrl) {
        startImageSave(destinationUrl, editorSession.exportValues())
    }

    function saveCurrentImage() {
        if (editorHasAdjustments)
            startImageSave(residentImageUrl, {
                "format": "original",
                "scale": 1.0,
                "quality": editorExportQuality
            })
    }

    function openSaveAsDialog() {
        if (!hasAssignedImage() || imageSaveService.busy)
            return

        const suggestion = suggestedSaveAsUrl()
        if (saveDialogLoader.dialog) {
            saveDialogLoader.dialog.suggestedFile = suggestion
            saveDialogLoader.dialog.open()
            return
        }

        if (!saveDialogLoader.active) {
            saveDialogLoader.setSource(
                "qrc:/Licasa/qml/SaveDialog.qml",
                {
                    "formatSupport": root.formatSupport,
                    "suggestedFile": suggestion
                }
            )
            saveDialogLoader.active = true
        }
    }

    function finishSavedImage(sourceUrl, destinationUrl) {
        if (!FileUrls.isLocal(residentImageUrl)
                || sourceUrl.toString() !== residentImageUrl.toString()) {
            return
        }

        stopMotionPhotoPlaybackForLifecycle()
        fullResTimer.stop()
        windowManager.releasePictureResources()
        residentImageUrl = destinationUrl
        currentImageUrl = destinationUrl
        readImageMetadata(destinationUrl, false)
        currentFileName = FileUrls.fileName(destinationUrl)
        currentPhotoHasMotion = false
        motionProbeRequestedUrl = ""
        photoAssetProbe.cancel()

        clearEditorSessionState(false)

        previewPhase = imageLimitedToPreview
            || (rawPreviewFirst && !rawFastPhase)
            || currentImageAnimated
            || shouldUsePreview()
        imageRevision += 1
        refreshEditedPreviewSource()
        editorSaveStatus = "Saved " + currentFileName
    }

    function hideToBackground() {
        if (backgroundModeManager.enabled && persistentShell) {
            clearImageState()
            root.visible = false
            return
        }

        root.close()
    }

    function quitApp() {
        windowManager.quitApplication()
    }

    EditorSession {
        id: editorSession
        onEditsChanged: root.scheduleEditedPreviewRefresh()
    }

    WindowedZoomController {
        id: windowedZoom

        hostWindow: root
        nativeOps: root.nativeWindowOps
        gestureSurface: gestureWindow
    }

    // On X11, this surface paints behind the exact image-size
    // resting window at zero opacity. During a wheel burst it becomes the
    // fixed-size renderer; its visual pixels remain limited to the image.
    Window {
        id: gestureWindow
        flags: Qt.Window | Qt.FramelessWindowHint
            | Qt.X11BypassWindowManagerHint | Qt.WindowDoesNotAcceptFocus
        color: "transparent"
        opacity: 0.0
        visible: root.nativeWindowOps.gestureWindowSupported()
            && root.visible && !root.fullScreenMode && root.hasImage()
        readonly property rect screenBounds: {
            const revision = root.screenRevision
            const bounds = windowedZoom.gestureActive
                ? windowedZoom.gestureScreenGeometry
                : root.nativeWindowOps.currentScreenGeometry(root)
            return Qt.rect(bounds.x, bounds.y, bounds.width, bounds.height)
        }
        x: screenBounds.x
        y: screenBounds.y
        width: screenBounds.width
        height: screenBounds.height
        // A separate full 100 MP texture can take seconds to upload and
        // becomes temporarily stale when the resting window promotes from
        // preview to full detail. This bounded request has enough pixels for
        // every floating size on this screen and stays ready through promotion.
        readonly property int sourceExtent: Math.min(8192, Math.ceil(
            Math.max(width, height) * windowedZoom.maximumScreenFraction
            * root.effectiveDpr()))
        readonly property bool useBoundedSource: !root.currentImageAnimated
            && (root.naturalImageSize.width > sourceExtent
                || root.naturalImageSize.height > sourceExtent)
        readonly property string presentationSource: !visible ? ""
            : useBoundedSource
                ? imageViewport.requestKeyForStage(
                    "preview", "", sourceExtent, sourceExtent)
                : imageViewport.activeRequestKey
        readonly property size presentationSourceSize: useBoundedSource
            ? Qt.size(sourceExtent, sourceExtent)
            : imageViewport.activeRequestSize
        readonly property bool readyForHandoff: visible
            && Math.abs(x - screenBounds.x) < 1
            && Math.abs(y - screenBounds.y) < 1
            && width === screenBounds.width
            && height === screenBounds.height
            && gestureViewport.imageStatus === Image.Ready
            && (root.currentImageAnimated
                ? gestureViewport.animatedFrameReady
                : presentationSource.length > 0
                    && root.nativeWindowOps.sameImageSource(
                        gestureViewport.activeRequestKey, presentationSource))
        onReadyForHandoffChanged: root.nativeWindowOps.traceZoom(
            "gesture-ready-" + readyForHandoff, windowedZoom.scale)

        Component.onCompleted: {
            if (root.nativeWindowOps.gestureWindowSupported())
                root.nativeWindowOps.setWindowInputRectangle(gestureWindow, 0, 0, 0, 0)
        }
        onVisibleChanged: {
            root.nativeWindowOps.traceZoom("gesture-visible-" + visible,
                                           windowedZoom.scale)
            if (visible) {
                root.nativeWindowOps.setWindowInputRectangle(gestureWindow, 0, 0, 0, 0)
                root.nativeWindowOps.setWindowPosition(
                    gestureWindow, screenBounds.x, screenBounds.y)
                root.raise()
            }
        }
        onXChanged: root.nativeWindowOps.traceZoom("gesture-x-" + x,
                                                   windowedZoom.scale)
        onYChanged: root.nativeWindowOps.traceZoom("gesture-y-" + y,
                                                   windowedZoom.scale)
        onOpacityChanged: root.nativeWindowOps.traceZoom("gesture-opacity-" + opacity,
                                                         windowedZoom.scale)
        onFrameSwapped: {
            if (!windowedZoom.gestureActive)
                return
            const imageRect = windowedZoom.gestureImageGeometry
            root.nativeWindowOps.setWindowInputRectangle(
                gestureWindow,
                imageRect.x - gestureWindow.x,
                imageRect.y - gestureWindow.y,
                imageRect.width,
                imageRect.height)
        }

        ImageViewport {
            id: gestureViewport
            anchors.fill: parent
            onImageStatusChanged: root.nativeWindowOps.traceZoom(
                "gesture-image-status-" + imageStatus, windowedZoom.scale)
            presentationOnly: true
            fullScreenMode: true
            fullscreenBackdropOpacity: 0.0
            transparencyCheckerboardEnabled: root.windowedTransparencyCheckerboardEnabled
            transparencyCheckerboardOpacity: root.windowedTransparencyOpacity
            windowedTransparencyCheckerboardEnabled: root.windowedTransparencyCheckerboardEnabled
            windowedTransparencyOpacity: root.windowedTransparencyOpacity
            hasImage: gestureWindow.visible && root.hasImage()
            imageX: windowedZoom.gestureImageGeometry.x - gestureWindow.x
            imageY: windowedZoom.gestureImageGeometry.y - gestureWindow.y
            imageWidth: root.imageWidth()
            imageHeight: root.imageHeight()
            currentScale: windowedZoom.scale
            providerSource: gestureWindow.visible ? root.renderedProviderSource : ""
            sourceOverride: gestureWindow.presentationSource
            sourceOverrideSize: gestureWindow.presentationSourceSize
            previewPhase: false
            rawFastPhase: false
            rawPreviewFirst: false
            parallelFullResolutionEnabled: false
            previewSourceWidth: root.previewSourceWidth()
            previewSourceHeight: root.previewSourceHeight()
            animatedSource: root.residentImageUrl
            animationController: root.animationController
            motionPhotoSession: gestureWindow.visible ? root.motionPhotoSession : null
            animatedPlaybackActive: gestureWindow.visible && root.animatedPlaybackActive
            floatingPlaybackEnabled: false
            playbackInteractionEnabled: false
            animationSpeed: root.viewerPreferences.animationSpeed
            smoothScalingEnabled: root.smoothImageScalingEnabled
            mipmapScalingEnabled: root.mipmapImageScalingEnabled
            pixelAlignedRenderingEnabled: root.pixelAlignedRenderingEnabled
            devicePixelRatio: root.effectiveDpr()
            fullScreenWheelSensitivity: root.windowedWheelSensitivity
            windowedWheelSensitivity: root.windowedWheelSensitivity
            maxWheelDeltaPerEvent: root.maxWheelDeltaPerEvent
            interactionBlocked: false
            canOpen: function(u) { return root.formatSupport.canOpen(u) }
            onZoomRequested: function(factor, px, py) {
                root.zoomByFactor(factor, px, py)
            }
        }

        OverlayControls {
            anchors.fill: parent
            z: 200
            appVisible: gestureWindow.visible
            fullScreenMode: false
            hasImage: root.hasImage()
            windowedCloseButtonX: windowedZoom.gestureImageGeometry.x
                - gestureWindow.x + windowedZoom.gestureImageGeometry.width - 44
            windowedCloseButtonY: windowedZoom.gestureImageGeometry.y
                - gestureWindow.y + 8
            onCloseRequested: root.hideToBackground()
        }
    }

    WindowedMoveController {
        id: windowedMove

        hostWindow: root
        nativeOps: root.nativeWindowOps
        zoomController: windowedZoom
    }

    Component.onCompleted: {
        windowedZoom.initialize()
        root.fullScreenMode = viewerPreferences.startInFullscreen

        if (FileUrls.isLocal(windowSession.initialImageUrl)) {
            root.pendingImageReveal = true
            loadImageUrl(windowSession.initialImageUrl)
            return
        }

        if (windowSession.startHidden) {
            root.visible = false
            return
        }

        root.visible = true
        root.syncWindowVisibility()

        if (!root.fullScreenMode)
            windowedZoom.restore()
    }

    onClosing: function(close) {
        clearImageState()
        if (backgroundModeManager.enabled && persistentShell) {
            close.accepted = false
            root.visible = false
            return
        }

        close.accepted = true

        Qt.callLater(function() {
            if (root)
                windowManager.destroyWindow(root)
        })
    }

    Connections {
        target: root.backgroundModeManager

        function onErrorOccurred(message) {
            console.warn(message)
            notificationCenter.finish("background", "error", "Background process error", message)
        }
    }

    Connections {
        target: root.viewerPreferences

        function onErrorOccurred(message) {
            console.warn(message)
            notificationCenter.finish("settings", "error", "Settings could not be saved", message)
        }

        function onMaximumImageMegapixelsChanged() {
            root.syncCurrentImageResourceLimit()
        }
    }

    Connections {
        target: root.windowSession

        function onOpenRequested(url) {
            root.loadImageUrl(url)
        }
    }

    Connections {
        target: root.photoAssetProbe

        function onInfoReady(url, info) {
            if (!FileUrls.isLocal(root.residentImageUrl)
                    || url.toString() !== root.residentImageUrl.toString()) {
                return
            }
            root.currentPhotoHasMotion = info.motionAvailable === true
        }
    }

    Connections {
        target: root.imageSaveService

        function onBusyChanged() {
            if (!root.imageSaveService.busy || root.pendingSaveKind.length === 0)
                return
            const frame = root.pendingSaveKind === "frame"
            notificationCenter.begin("save", frame ? "Saving frame" : "Saving image",
                                     frame ? "Writing a PNG copy…" : "Writing your changes…")
        }

        function onSizeEstimateReady(sourceUrl, requestId, byteCount, errorMessage) {
            if (!root.editorCropMode
                    || requestId !== root.cropEstimateRevision
                    || sourceUrl.toString() !== root.residentImageUrl.toString())
                return
            root.cropEstimateText = byteCount > 0
                ? root.formatFileSize(byteCount) : "size unavailable"
        }

        function onSaveCompleted(sourceUrl, destinationUrl) {
            if (!FileUrls.isLocal(root.residentImageUrl)
                    || sourceUrl.toString() !== root.residentImageUrl.toString())
                return
            root.finishSavedImage(sourceUrl, destinationUrl)
            notificationCenter.finish("save", "success", "Image saved",
                                      FileUrls.fileName(destinationUrl))
            root.pendingSaveKind = ""
        }

        function onSaveFailed(sourceUrl, message) {
            if (FileUrls.isLocal(root.residentImageUrl)
                    && sourceUrl.toString() === root.residentImageUrl.toString()) {
                root.editorSaveStatus = message
                notificationCenter.finish("save", "error", "Image could not be saved", message)
                root.pendingSaveKind = ""
            }
        }

        function onFrameSaveCompleted(sourceUrl, destinationUrl) {
            if (FileUrls.isLocal(root.residentImageUrl)
                    && sourceUrl.toString() === root.residentImageUrl.toString()) {
                console.info("Saved Motion Photo frame:", destinationUrl.toString())
                notificationCenter.finish("save", "success", "Frame saved",
                                          FileUrls.fileName(destinationUrl))
                root.pendingSaveKind = ""
            }
        }

        function onFrameSaveFailed(sourceUrl, message) {
            if (FileUrls.isLocal(root.residentImageUrl)
                    && sourceUrl.toString() === root.residentImageUrl.toString()) {
                console.warn("Motion Photo frame save failed:", message)
                notificationCenter.finish("save", "error", "Frame could not be saved", message)
                root.pendingSaveKind = ""
            }
        }
    }

    Connections {
        target: root.motionPhotoExportService

        function onBusyChanged() {
            if (root.motionPhotoExportService.busy)
                notificationCenter.begin("motion-export", "Exporting Motion Photo",
                                         "Copying the video…")
        }

        function onExportCompleted(sourceUrl, destinationUrl) {
            if (sourceUrl.toString() === String(root.residentImageUrl))
                notificationCenter.finish("motion-export", "success", "Video exported",
                                          FileUrls.fileName(destinationUrl))
        }

        function onExportFailed(sourceUrl, message) {
            if (sourceUrl.toString() === String(root.residentImageUrl))
                notificationCenter.finish("motion-export", "error", "Video export failed", message)
        }
    }

    Connections {
        target: root.animationExportService

        function onBusyChanged() {
            if (root.animationExportService.busy)
                notificationCenter.begin("animation-export", "Exporting animation",
                                         "Copying the animation…")
        }

        function onExportCompleted(sourceUrl, destinationUrl) {
            if (sourceUrl.toString() === String(root.residentImageUrl))
                notificationCenter.finish("animation-export", "success", "Animation exported",
                                          FileUrls.fileName(destinationUrl))
        }

        function onExportFailed(sourceUrl, message) {
            if (sourceUrl.toString() === String(root.residentImageUrl))
                notificationCenter.finish("animation-export", "error",
                                          "Animation export failed", message)
        }
    }

    onXChanged: {
        screenRevision += 1
        if (!root.fullScreenMode) {
            if (windowedMove.active)
                windowedMove.update()
            else
                rememberCurrentWindowedGeometry()
        }
    }

    onYChanged: {
        screenRevision += 1
        if (!root.fullScreenMode) {
            if (windowedMove.active)
                windowedMove.update()
            else
                rememberCurrentWindowedGeometry()
        }
    }

    onWidthChanged: {
        if (!root.fullScreenMode) {
            rememberCurrentWindowedGeometry()
        }

        if (hasImage() && root.fullScreenMode) {
            if (root.editPanelOpen)
                root.editorWorkspaceInset = root.targetEditorWorkspaceInset(true)

            if (fitMode) {
                currentScale = fitScaleFor(fitPadding)
                fullscreenTargetScale = currentScale
                panX = 0
                panY = 0
            } else {
                fullscreenRecoveryTimer.restart()
            }
        }
    }

    onHeightChanged: {
        if (!root.fullScreenMode) {
            rememberCurrentWindowedGeometry()
        }

        if (hasImage() && root.fullScreenMode) {
            if (fitMode) {
                currentScale = fitScaleFor(fitPadding)
                fullscreenTargetScale = currentScale
                panX = 0
                panY = 0
            } else {
                fullscreenRecoveryTimer.restart()
            }
        }
    }

    onScreenChanged: {
        screenRevision += 1
        if (windowedZoom.gestureActive)
            windowedZoom.cancelAnimation()
        if (root.fullScreenMode)
            fullscreenRecoveryTimer.restart()
    }

    Connections {
        target: root.nativeWindowOps
        ignoreUnknownSignals: true
        function onScreensChanged() {
            root.screenRevision += 1
            if (windowedZoom.gestureActive)
                windowedZoom.cancelAnimation()
            if (root.fullScreenMode)
                fullscreenRecoveryTimer.restart()
        }
    }

    onCurrentScaleChanged: {
        if (!root.fullScreenMode || !root.fullscreenZoomAnchorActive)
            return

        const newTopLeftX = root.zoomAnchorViewportX - root.zoomAnchorImageX * root.currentScale
        const newTopLeftY = root.zoomAnchorViewportY - root.zoomAnchorImageY * root.currentScale
        root.panX = newTopLeftX - root.centeredX(root.currentScale)
        root.panY = newTopLeftY - root.centeredY(root.currentScale)
    }

    onVisibleChanged: {
        if (root.visible) {
            notificationCenter.startSession()
            visibilitySyncTimer.restart()
        } else {
            stopMotionPhotoPlaybackForLifecycle()
            resourceReleaseTimer.restart()
            memoryTrimTimer.restart()
        }
    }

    Shortcut { sequence: "Ctrl+O"; onActivated: root.openFileDialog() }
    Shortcut { sequence: "Ctrl+S"; onActivated: root.saveCurrentImage() }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: root.openSaveAsDialog() }

    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (notificationCenter.historyOpen) {
                notificationCenter.historyOpen = false
                return
            }
            if (root.editorCropMode) {
                root.cancelCropMode()
                return
            }

            if (root.editPanelOpen) {
                root.setEditPanelOpen(false)
                return
            }

            if (fullscreenSideMenu.panelOpen) {
                fullscreenSideMenu.panelOpen = false
                return
            }

            if (root.fullScreenMode) {
                root.exitFullScreenToImage()
                return
            }

            root.hideToBackground()
        }
    }

    Shortcut { sequence: "F"; enabled: !fullscreenSideMenu.panelOpen && !notificationCenter.historyOpen; onActivated: root.fitToWindow() }
    Shortcut { sequence: "1"; enabled: !fullscreenSideMenu.panelOpen && !notificationCenter.historyOpen; onActivated: root.actualSize() }
    Shortcut {
        sequence: "C"
        onActivated: {
            if (root.fullScreenMode && root.editPanelOpen && !root.editorCropMode)
                root.beginCropMode()
        }
    }
    Shortcut {
        sequence: "Return"
        enabled: root.editorCropMode
        onActivated: {
            if (root.editorCropMode)
                root.applyDraftCrop()
        }
    }
    Shortcut { sequence: "Ctrl+Shift+Q"; onActivated: root.quitApp() }
    Shortcut {
        sequence: "Ctrl+Z"
        onActivated: {
            if (root.fullScreenMode && root.editPanelOpen)
                root.undoEditorChange()
        }
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"
        onActivated: {
            if (root.fullScreenMode && root.editPanelOpen)
                root.redoEditorChange()
        }
    }
    Shortcut { sequence: "F11"; onActivated: root.toggleFullScreen() }
    Shortcut { sequence: "+"; enabled: !fullscreenSideMenu.panelOpen && !notificationCenter.historyOpen; onActivated: root.zoomFromKeyboard(1.15) }
    Shortcut { sequence: "="; enabled: !fullscreenSideMenu.panelOpen && !notificationCenter.historyOpen; onActivated: root.zoomFromKeyboard(1.15) }
    Shortcut { sequence: "-"; enabled: !fullscreenSideMenu.panelOpen && !notificationCenter.historyOpen; onActivated: root.zoomFromKeyboard(1.0 / 1.15) }

    Shortcut {
        sequence: "E"
        enabled: !root.editorCropMode
        onActivated: {
            if (root.fullScreenMode && root.hasAssignedImage())
                root.setEditPanelOpen(!root.editPanelOpen)
        }
    }
    Shortcut {
        sequence: "M"
        enabled: !root.editorCropMode
        onActivated: {
            if (root.fullScreenMode) {
                if (root.editPanelOpen)
                    root.setEditPanelOpen(false)
                notificationCenter.historyOpen = false
                fullscreenSideMenu.panelOpen = !fullscreenSideMenu.panelOpen
            }
        }
    }
    Shortcut {
        sequence: "N"
        enabled: root.fullScreenMode && !root.editPanelOpen && !fullscreenSideMenu.panelOpen
        onActivated: notificationCenter.historyOpen = !notificationCenter.historyOpen
    }
    Shortcut {
        sequence: "PageDown"
        enabled: root.fullScreenMode && (root.editPanelOpen || fullscreenSideMenu.panelOpen
            || notificationCenter.historyOpen)
        onActivated: {
            if (notificationCenter.historyOpen)
                notificationCenter.scrollHistoryByPage(1)
            else if (fullscreenSideMenu.panelOpen)
                fullscreenSideMenu.scrollByPage(1)
            else
                editorSidePanel.scrollByPage(1)
        }
    }
    Shortcut {
        sequence: "PageUp"
        enabled: root.fullScreenMode && (root.editPanelOpen || fullscreenSideMenu.panelOpen
            || notificationCenter.historyOpen)
        onActivated: {
            if (notificationCenter.historyOpen)
                notificationCenter.scrollHistoryByPage(-1)
            else if (fullscreenSideMenu.panelOpen)
                fullscreenSideMenu.scrollByPage(-1)
            else
                editorSidePanel.scrollByPage(-1)
        }
    }
    Shortcut {
        sequence: "Ctrl+1"
        enabled: root.fullScreenMode && root.editPanelOpen
        onActivated: editorSidePanel.activateTool("adjust")
    }
    Shortcut {
        sequence: "Ctrl+2"
        enabled: root.fullScreenMode && root.editPanelOpen
        onActivated: editorSidePanel.activateTool("crop")
    }
    Shortcut {
        sequence: "Ctrl+3"
        enabled: root.fullScreenMode && root.editPanelOpen
        onActivated: editorSidePanel.activateTool("looks")
    }
    Shortcut {
        sequence: "Ctrl+4"
        enabled: root.fullScreenMode && root.editPanelOpen
        onActivated: editorSidePanel.activateTool("export")
    }

    Shortcut {
        sequence: "Left"
        enabled: root.fullScreenMode && !root.editPanelOpen && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
            && !(temporalControlsLoader.item && temporalControlsLoader.item.keyboardFocusWithin)
        onActivated: root.browseImage(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.fullScreenMode && !root.editPanelOpen && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
            && !(temporalControlsLoader.item && temporalControlsLoader.item.keyboardFocusWithin)
        onActivated: root.browseImage(1)
    }
    Shortcut {
        sequence: "Up"
        enabled: root.fullScreenMode && !root.editPanelOpen && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
            && !(temporalControlsLoader.item && temporalControlsLoader.item.keyboardFocusWithin)
        onActivated: root.zoomFromKeyboard(1.15)
    }
    Shortcut {
        sequence: "Down"
        enabled: root.fullScreenMode && !root.editPanelOpen && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
            && !(temporalControlsLoader.item && temporalControlsLoader.item.keyboardFocusWithin)
        onActivated: root.zoomFromKeyboard(1.0 / 1.15)
    }
    Shortcut {
        sequence: "Ctrl+Left"
        enabled: root.fullScreenMode && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
        onActivated: root.moveImage(-48, 0)
    }
    Shortcut {
        sequence: "Ctrl+Right"
        enabled: root.fullScreenMode && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
        onActivated: root.moveImage(48, 0)
    }
    Shortcut {
        sequence: "Ctrl+Up"
        enabled: root.fullScreenMode && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
        onActivated: root.moveImage(0, -48)
    }
    Shortcut {
        sequence: "Ctrl+Down"
        enabled: root.fullScreenMode && !fullscreenSideMenu.panelOpen
            && !notificationCenter.historyOpen
        onActivated: root.moveImage(0, 48)
    }
    Shortcut { sequence: "Alt+Left"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(-1, 0, false) }
    Shortcut { sequence: "Alt+Right"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(1, 0, false) }
    Shortcut { sequence: "Alt+Up"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(0, -1, false) }
    Shortcut { sequence: "Alt+Down"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(0, 1, false) }
    Shortcut { sequence: "Alt+Shift+Left"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(-1, 0, true) }
    Shortcut { sequence: "Alt+Shift+Right"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(1, 0, true) }
    Shortcut { sequence: "Alt+Shift+Up"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(0, -1, true) }
    Shortcut { sequence: "Alt+Shift+Down"; enabled: root.editorCropMode; onActivated: cropOverlay.keyboardAdjust(0, 1, true) }

    Shortcut {
        sequence: "Ctrl+R"
        onActivated: {
            if (root.fullScreenMode && root.hasAssignedImage())
                root.resetEditorAdjustments()
        }
    }

    Shortcut {
        sequence: "Y"
        onActivated: {
            if (root.fullScreenMode && root.hasAssignedImage() && !root.editorCropMode) {
                root.compareOriginal = !root.compareOriginal
                root.scheduleEditedPreviewRefresh()
            }
        }
    }

    ImageViewport {
        id: imageViewport
        objectName: "imageViewport"
        anchors.fill: parent
        onImageStatusChanged: root.nativeWindowOps.traceZoom(
            "exact-image-status-" + imageStatus, windowedZoom.scale)

        fullScreenMode: root.fullScreenMode
        fullscreenBackdropOpacity: root.fullscreenBackdropOpacity
        transparencyCheckerboardEnabled: root.transparencyCheckerboardEnabled
        transparencyCheckerboardOpacity: root.transparencyCheckerboardOpacity
        windowedTransparencyCheckerboardEnabled: root.windowedTransparencyCheckerboardEnabled
        windowedTransparencyOpacity: root.windowedTransparencyOpacity
        hasImage: root.hasImage()

        imageX: root.imageX()
        imageY: root.imageY()
        imageWidth: root.imageWidth()
        imageHeight: root.imageHeight()
        currentScale: root.currentScale

        providerSource: root.renderedProviderSource
        previewPhase: root.previewPhase
        rawFastPhase: root.rawFastPhase
        rawPreviewFirst: root.rawPreviewFirst
        parallelFullResolutionEnabled: root.previewPhase
            && !root.imageLimitedToPreview
            && !root.currentImageAnimated
            && root.fullResolutionRenderingEnabled
            && root.parallelOpeningWorthwhile()
        previewSourceWidth: root.previewSourceWidth()
        previewSourceHeight: root.previewSourceHeight()
        animatedSource: root.residentImageUrl
        animationController: root.animationController
        motionPhotoSession: root.motionPhotoSession
        animatedPlaybackActive: root.animatedPlaybackActive
        floatingPlaybackEnabled: !root.currentImageAnimated
            && root.motionPhotoSession !== null && root.motionPhotoSession.available
            && !root.editPanelOpen && !root.editorHasAdjustments
        playbackInteractionEnabled: !root.imageSaveService.busy
            && (!root.motionPhotoExportService || !root.motionPhotoExportService.busy)
            && (!root.animationExportService || !root.animationExportService.busy)
            && (!imageViewport.decodePending
                || (root.motionPhotoSession && root.motionPhotoSession.active))
        onPlaybackToggleRequested: {
            if (root.motionPhotoSession)
                root.motionPhotoSession.togglePlayback()
        }
        animationSpeed: root.viewerPreferences.animationSpeed
        smoothScalingEnabled: root.smoothImageScalingEnabled
        mipmapScalingEnabled: root.mipmapImageScalingEnabled
        pixelAlignedRenderingEnabled: root.pixelAlignedRenderingEnabled
        devicePixelRatio: root.effectiveDpr()

        fullScreenWheelSensitivity: root.fullScreenWheelSensitivity
        windowedWheelSensitivity: root.windowedWheelSensitivity
        maxWheelDeltaPerEvent: root.maxWheelDeltaPerEvent
        interactionBlocked: root.editorCropMode
            || editorSidePanel.pointerInsidePanel
            || fullscreenSideMenu.pointerInsidePanel

        canOpen: function(u) {
            return root.formatSupport.canOpen(u)
        }

        onPanRequested: function(dx, dy) {
            root.panX += dx
            root.panY += dy
        }

        onDisableFitModeRequested: {
            root.cancelZoomAnimations()
            root.fitMode = false
        }

        onCancelZoomAnimationRequested: root.cancelZoomAnimations()

        onWindowMoveStarted: windowedMove.begin()
        onWindowMoveUpdated: windowedMove.update()
        onWindowMoveFinished: windowedMove.finish()

        onClampPanRequested: {
            root.clampPanToViewport()
        }

        onPromoteFullResRequested: {
            root.promoteFullRes()
        }

        onImageRequestFailed: {
            // If the embedded preview cannot be decoded, the already-running
            // full reader is the shortest route to pixels. Avoid queueing a
            // half-size RAW development behind that full reader's memory gate.
            if (!root.hasImage() && imageViewport.speculativeFullAvailable
                    && (root.previewPhase || root.rawFastPhase)
                    && !root.imageLimitedToPreview) {
                root.promoteFullRes()
                return
            }
            // If LibRaw cannot expose a usable embedded thumbnail (common with
            // some CR2/other camera RAWs), do a bounded half-size development
            // first. That replaces the grey loading card with real image pixels
            // before the expensive final development starts.
            if (root.rawPreviewFirst
                    && root.previewPhase
                    && !root.imageLimitedToPreview
                    && !root.rawFullDetailRequested) {
                notificationCenter.update("image", "Loading RAW preview",
                                          "Trying a compatible preview…")
                root.requestRawFastFallback()
                return
            }
            // The fast stage is opportunistic. If a backend cannot produce it,
            // continue to the normal full-detail path rather than getting stuck.
            if (root.rawPreviewFirst
                    && root.rawFastPhase
                    && !root.imageLimitedToPreview
                    && !root.rawFullDetailRequested) {
                notificationCenter.update("image", "Loading RAW image",
                                          "Developing the full image…")
                root.promoteFullRes()
                return
            }
            const previewAvailable = root.hasImage()
            const openingImage = root.imageOpenStartedMs >= 0
                && notificationCenter.isLoading("image")
            root.imageOpenStartedMs = -1
            notificationCenter.finish("image", "error",
                                      openingImage
                                          ? (previewAvailable ? "Full resolution could not load"
                                                              : "Image could not be opened")
                                          : "Image update failed",
                                      previewAvailable
                                          ? (openingImage ? "The preview is still available."
                                                          : "The current image is still available.")
                                          : root.currentFileName)
            if (root.pendingImageReveal)
                root.revealLoadedImage()
        }

        onZoomRequested: function(factor, px, py) {
            root.zoomByFactor(factor, px, py)
        }

        onOpenDialogRequested: root.openFileDialog()

        onExitFullscreenToImageRequested: {
            root.exitFullScreenToImage()
        }

        onEnterFullscreenRequested: {
            root.enterFullScreen()
        }

        onOpenUrlRequested: function(url) {
            root.loadImageUrl(url)
        }

        onImageReady: {
            const shouldReveal = root.pendingImageReveal
            root.displayedQuarterTurns = root.effectiveEditorQuarterTurns()
            root.displayedCropX = root.effectiveEditorCropX()
            root.displayedCropY = root.effectiveEditorCropY()
            root.displayedCropWidth = root.effectiveEditorCropWidth()
            root.displayedCropHeight = root.effectiveEditorCropHeight()

            if (root.fitMode) {
                if (root.fullScreenMode)
                    root.applyFit(root.fitPadding)
                else
                    windowedZoom.applyOpenScale(shouldReveal)
            } else if (!root.fullScreenMode) {
                windowedZoom.applyState()
            }

            // Progressive loading keeps the currently ready raster visible
            // while the next stage decodes in the spare image slot. After the
            // first preview paint, automatically request full detail for every
            // eligible still image, including RAW. Zoom only changes the view;
            // it no longer controls whether the full sensor raster is loaded.
            if ((root.previewPhase || root.rawFastPhase)
                    && !root.imageLimitedToPreview
                    && !root.animatedPlaybackActive
                    && root.fullResolutionRenderingEnabled)
                fullResTimer.restart()

            if (root.imageOpenStartedMs >= 0 && notificationCenter.isLoading("image")) {
                if (root.rawMetadataProbePending
                        || ((root.previewPhase || root.rawFastPhase)
                            && !root.imageLimitedToPreview
                            && !root.animatedPlaybackActive
                            && root.fullResolutionRenderingEnabled)) {
                    notificationCenter.update("image", "Opening image",
                                              root.currentFileName + " · Loading full resolution…")
                } else {
                    const openingDurationMs = Math.max(0, Date.now() - root.imageOpenStartedMs)
                    root.imageOpenStartedMs = -1
                    const detail = root.currentFileName + " · " + openingDurationMs + " ms"
                        + (root.imageLimitedToPreview
                            ? " · Full resolution exceeds the image memory limit" : "")
                    notificationCenter.finish("image", "success",
                                              root.imageLimitedToPreview
                                                  ? "Preview ready" : "Image ready",
                                              detail)
                }
            }

            if (root.fullScreenMode)
                fullscreenRecoveryTimer.restart()

            if (shouldReveal)
                root.revealLoadedImage()

            if (root.rawMetadataProbePending)
                rawMetadataProbeTimer.restart()

            // Defer one event-loop turn after Image.Ready so first-paint and
            // first-preview telemetry are never competing with metadata I/O.
            Qt.callLater(root.requestPhotoAssetProbeIfNeeded)
        }
    }

    CropOverlay {
        id: cropOverlay
        anchors.fill: parent
        z: 1050

        active: root.fullScreenMode && root.editPanelOpen
            && root.editorCropMode && root.hasImage()
        imageX: root.imageX()
        imageY: root.imageY()
        imageWidth: root.imageWidth() * root.currentScale
        imageHeight: root.imageHeight() * root.currentScale
        cropX: root.draftCropX
        cropY: root.draftCropY
        cropWidth: root.draftCropWidth
        cropHeight: root.draftCropHeight
        aspectRatio: root.cropAspectRatio
        outputWidth: root.draftCropOutputWidth()
        outputHeight: root.draftCropOutputHeight()
        estimatedFileSize: root.draftCropEstimatedFileSize()
        shieldOpacity: root.cropShieldOpacity

        onCropRequested: function(x, y, width, height) {
            root.updateDraftCrop(x, y, width, height)
        }
    }

    InfoOverlays {
        anchors.fill: parent
        z: 150

        fullScreenMode: root.fullScreenMode
        infoOverlayEnabled: root.infoOverlayEnabled
        compareOriginal: root.compareOriginal
        imageReady: root.hasImage()
        limitedToPreview: root.imageLimitedToPreview

        currentFileName: root.currentFileName
        imageWidth: root.imageWidth()
        imageHeight: root.imageHeight()
        currentScale: root.currentScale
    }

    Loader {
        id: temporalControlsLoader
        objectName: "temporalControlsLoader"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.fullScreenMode && root.infoOverlayEnabled ? 92 : 18
        width: Math.min(540, Math.max(280, parent.width - 48))
        height: active ? 58 : 0
        z: 210

        // Keep the ordinary still-image path allocation-free for temporal UI.
        // The tray is instantiated only for a live animated-image controller or
        // a validated Motion Photo session. Merely probing motion metadata does
        // not load Qt Multimedia; first playback intent remains the lazy boundary.
        active: root.fullScreenMode && root.hasImage()
            && !root.editPanelOpen
            && !root.editorHasAdjustments
            && ((root.currentImageAnimated && root.animationController !== null)
                || (root.motionPhotoSession && root.motionPhotoSession.available))
        visible: active

        onActiveChanged: {
            if (!active) {
                source = ""
                return
            }
            setSource(Qt.resolvedUrl("ViewerTemporalControls.qml"), {
                motionSession: Qt.binding(function() { return root.motionPhotoSession }),
                frameSaveCoordinator: Qt.binding(function() {
                    return root.motionPhotoFrameSaveCoordinator
                }),
                motionExportService: Qt.binding(function() {
                    return root.motionPhotoExportService
                }),
                animationExportService: Qt.binding(function() {
                    return root.animationExportService
                }),
                animationController: Qt.binding(function() { return root.animationController }),
                imageAnimated: Qt.binding(function() { return root.currentImageAnimated }),
                windowManager: root.windowManager,
                viewerRoot: root,
                interactionEnabled: Qt.binding(function() {
                    // decodePending is an admission gate before Motion Photo
                    // playback starts. Once the session is active it owns the
                    // shared heavy-processing gate, so a queued still-image
                    // promotion may remain pending until playback stops. Do
                    // not let that waiter disable Pause/Seek on the transport
                    // that currently owns the gate.
                    const motionOwnsProcessingGate = root.motionPhotoSession
                        && root.motionPhotoSession.active
                    return !root.imageSaveService.busy
                        && (!root.motionPhotoExportService
                            || !root.motionPhotoExportService.busy)
                        && (!root.animationExportService
                            || !root.animationExportService.busy)
                        && (!imageViewport.decodePending || motionOwnsProcessingGate)
                })
            })
        }
    }

    Connections {
        target: temporalControlsLoader.item
        enabled: temporalControlsLoader.item !== null

        function onSaveFrameAsRequested() {
            root.openFrameSaveAsDialog()
        }

        function onExportMotionAsRequested() {
            const service = root.windowManager.ensureMotionPhotoExportService(root)
            if (!service)
                return
            root.motionPhotoExportService = service
            temporalExportCoordinatorLoader.requestExport("motion")
        }

        function onExportAnimationAsRequested() {
            const service = root.windowManager.ensureAnimationExportService(root)
            if (!service)
                return
            root.animationExportService = service
            temporalExportCoordinatorLoader.requestExport("animation")
        }
    }

    Loader {
        id: temporalExportCoordinatorLoader

        // Keep Item 12 export presentation completely cold until the user
        // explicitly asks to export. Motion Photo capability becoming
        // available must not compete with first-frame playback startup.
        active: false
        asynchronous: true
        property string pendingExportKind: ""

        function dispatchExport(kind) {
            if (!item)
                return false
            if (kind === "motion")
                item.openMotionExportDialog()
            else if (kind === "animation")
                item.openAnimationExportDialog()
            else
                return false
            return true
        }

        function requestExport(kind) {
            if (dispatchExport(kind))
                return

            pendingExportKind = kind
            if (active)
                return

            setSource(Qt.resolvedUrl("TemporalExportCoordinator.qml"), {
                sourceUrl: Qt.binding(function() { return root.residentImageUrl }),
                imageAnimated: Qt.binding(function() { return root.currentImageAnimated }),
                motionExportService: Qt.binding(function() { return root.motionPhotoExportService }),
                animationExportService: Qt.binding(function() { return root.animationExportService })
            })
            active = true
        }

        onLoaded: {
            const kind = pendingExportKind
            pendingExportKind = ""
            dispatchExport(kind)
        }
    }

    OverlayControls {
        anchors.fill: parent
        z: 200

        appVisible: root.visible
        fullScreenMode: root.fullScreenMode
        hasImage: root.hasImage()
        windowedCloseButtonX: root.windowedCloseButtonX(36)
        windowedCloseButtonY: root.windowedCloseButtonY(36)
        cornerControlSize: root.cornerControlSize
        cornerControlMargin: root.cornerControlMargin

        onToggleFullScreenRequested: root.toggleFullScreen()
        onCloseRequested: root.hideToBackground()
    }

    NotificationCenter {
        id: notificationCenter
        objectName: "notificationCenter"
        anchors.fill: parent
        z: fullscreenSideMenu.panelOpen ? 900 : 1600
        appVisible: root.visible
        fullScreenMode: root.fullScreenMode
        cornerControlMargin: root.cornerControlMargin
        closeButtonSize: root.fullScreenMode ? root.cornerControlSize : 36
    }

    FullscreenSideMenu {
        id: fullscreenSideMenu
        anchors.fill: parent
        z: 1000
        fullscreen: root.fullScreenMode
        onPanelOpenChanged: {
            if (panelOpen)
                notificationCenter.historyOpen = false
        }
        cornerControlSize: root.cornerControlSize
        cornerControlMargin: root.cornerControlMargin

        infoOverlayEnabled: root.infoOverlayEnabled
        smoothMotionEnabled: root.smoothMenuMotionEnabled
        startInFullscreenEnabled: root.viewerPreferences.startInFullscreen
        fullscreenBackgroundOpacity: root.viewerPreferences.fullscreenBackgroundOpacity
        transparencyCheckerboardEnabled: root.viewerPreferences.transparencyCheckerboardEnabled
        transparencyCheckerboardOpacity: root.viewerPreferences.transparencyCheckerboardOpacity
        windowedTransparencyCheckerboardEnabled:
            root.viewerPreferences.windowedTransparencyCheckerboardEnabled
        windowedTransparencyOpacity: root.viewerPreferences.windowedTransparencyOpacity
        animatedImagePlaybackEnabled:
            root.viewerPreferences.animatedImagePlaybackEnabled
        animationSpeed: root.viewerPreferences.animationSpeed
        smoothImageScalingEnabled: root.viewerPreferences.smoothImageScalingEnabled
        mipmapImageScalingEnabled: root.viewerPreferences.mipmapImageScalingEnabled
        pixelAlignedRenderingEnabled:
            root.viewerPreferences.pixelAlignedRenderingEnabled
        fullResolutionRenderingEnabled:
            root.viewerPreferences.fullResolutionRenderingEnabled
        colorManagedRenderingEnabled:
            root.viewerPreferences.colorManagedRenderingEnabled
        maximumImageMemoryMiB:
            root.viewerPreferences.maximumImageMemoryMiB
        minimumImageMemoryMiB:
            root.viewerPreferences.minimumImageMemoryMiB
        maximumImageMemoryLimitMiB:
            root.viewerPreferences.maximumImageMemoryLimitMiB
        currentImageMegapixels: root.currentImageMegapixels()
        currentImageLimitedToPreview: root.imageLimitedToPreview
        cropShieldOpacity: root.viewerPreferences.cropShieldOpacity

        onInfoOverlayChanged: function(enabled) {
            root.infoOverlayEnabled = enabled
        }

        onSmoothMotionChanged: function(enabled) {
            root.smoothMenuMotionEnabled = enabled
        }

        onStartInFullscreenChanged: function(enabled) {
            root.viewerPreferences.startInFullscreen = enabled
        }

        onFullscreenBackgroundOpacityRequested: function(value) {
            root.viewerPreferences.fullscreenBackgroundOpacity = value
        }

        onTransparencyCheckerboardChanged: function(enabled) {
            root.viewerPreferences.transparencyCheckerboardEnabled = enabled
        }

        onTransparencyCheckerboardOpacityRequested: function(value) {
            root.viewerPreferences.transparencyCheckerboardOpacity = value
        }

        onWindowedTransparencyCheckerboardChanged: function(enabled) {
            root.viewerPreferences.windowedTransparencyCheckerboardEnabled = enabled
        }

        onWindowedTransparencyOpacityRequested: function(value) {
            root.viewerPreferences.windowedTransparencyOpacity = value
        }

        onAnimatedImagePlaybackChanged: function(enabled) {
            root.viewerPreferences.animatedImagePlaybackEnabled = enabled
        }

        onAnimationSpeedRequested: function(value) {
            root.viewerPreferences.animationSpeed = value
        }

        onSmoothImageScalingChanged: function(enabled) {
            root.viewerPreferences.smoothImageScalingEnabled = enabled
        }

        onMipmapImageScalingChanged: function(enabled) {
            root.viewerPreferences.mipmapImageScalingEnabled = enabled
        }

        onPixelAlignedRenderingChanged: function(enabled) {
            root.viewerPreferences.pixelAlignedRenderingEnabled = enabled
        }

        onFullResolutionRenderingChanged: function(enabled) {
            root.viewerPreferences.fullResolutionRenderingEnabled = enabled
            if (enabled)
                root.promoteFullRes()
        }

        onColorManagedRenderingChanged: function(enabled) {
            root.viewerPreferences.colorManagedRenderingEnabled = enabled
            root.imageRevision += 1
            root.refreshEditedPreviewSource()
        }

        onMaximumImageMemoryMiBRequested: function(value) {
            root.viewerPreferences.maximumImageMemoryMiB = value
        }

        onCropShieldOpacityRequested: function(value) {
            root.viewerPreferences.cropShieldOpacity = value
        }
    }

    EditorSidePanel {
        id: editorSidePanel
        anchors.fill: parent
        z: 1100

        hasImage: root.fullScreenMode && root.hasAssignedImage()
        panelOpen: root.editPanelOpen
        smoothMotionEnabled: root.smoothMenuMotionEnabled
        cornerControlSize: root.cornerControlSize
        cornerControlMargin: root.cornerControlMargin

        exposure: root.editorExposure
        contrast: root.editorContrast
        highlights: root.editorHighlights
        shadows: root.editorShadows
        saturation: root.editorSaturation
        vibrance: root.editorVibrance
        warmth: root.editorWarmth
        tint: root.editorTint
        blur: root.editorBlur
        sharpen: root.editorSharpen
        vignette: root.editorVignette

        flipHorizontal: root.editorFlipHorizontal
        flipVertical: root.editorFlipVertical
        rotationQuarterTurns: root.normalizedQuarterTurns(root.editorQuarterTurns)
        cropMode: root.editorCropMode
        cropAspectPreset: root.cropAspectPreset
        cropOutputWidth: root.draftCropOutputWidth()
        cropOutputHeight: root.draftCropOutputHeight()
        cropEstimatedFileSize: root.draftCropEstimatedFileSize()
        compareOriginal: root.compareOriginal
        hasAdjustments: root.editorHasAdjustments
        saveBusy: root.imageSaveService.busy
        saveStatus: root.editorSaveStatus
        exportFormat: root.editorExportFormat
        exportScale: root.editorExportScale
        exportQuality: root.editorExportQuality
        outputWidth: root.editedOutputWidth(root.editorExportScale)
        outputHeight: root.editedOutputHeight(root.editorExportScale)

        onPanelOpenChangedByUser: function(open) {
            root.setEditPanelOpen(open)
        }

        onExposureRequested: function(value) {
            editorSession.setAdjustment("exposure", value)
        }

        onContrastRequested: function(value) {
            editorSession.setAdjustment("contrast", value)
        }

        onHighlightsRequested: function(value) {
            editorSession.setAdjustment("highlights", value)
        }

        onShadowsRequested: function(value) {
            editorSession.setAdjustment("shadows", value)
        }

        onSaturationRequested: function(value) {
            editorSession.setAdjustment("saturation", value)
        }

        onVibranceRequested: function(value) {
            editorSession.setAdjustment("vibrance", value)
        }

        onWarmthRequested: function(value) {
            editorSession.setAdjustment("warmth", value)
        }

        onTintRequested: function(value) {
            editorSession.setAdjustment("tint", value)
        }

        onBlurRequested: function(value) {
            editorSession.setAdjustment("blur", value)
        }

        onSharpenRequested: function(value) {
            editorSession.setAdjustment("sharpen", value)
        }

        onVignetteRequested: function(value) {
            editorSession.setAdjustment("vignette", value)
        }

        onAdjustmentInteractionStarted: root.beginEditorInteraction()
        onAdjustmentInteractionFinished: root.finishEditorInteraction()

        onFlipHorizontalToggled: editorSession.toggleFlipHorizontal()
        onFlipVerticalToggled: editorSession.toggleFlipVertical()
        onRotateLeftRequested: editorSession.rotateLeft()
        onRotateRightRequested: editorSession.rotateRight()
        onRotationResetRequested: editorSession.resetRotation()

        onCompareOriginalPressed: {
            if (!root.compareOriginal) {
                root.compareOriginal = true
                root.scheduleEditedPreviewRefresh()
            }
        }

        onCompareOriginalReleased: {
            if (root.compareOriginal) {
                root.compareOriginal = false
                root.scheduleEditedPreviewRefresh()
            }
        }

        onResetRequested: {
            root.resetEditorAdjustments()
        }

        onCropStarted: root.beginCropMode()
        onCropCanceled: root.cancelCropMode()
        onCropApplied: root.applyDraftCrop()
        onCropPresetRequested: function(name) { root.applyCropPreset(name) }
        onCustomCropAspectRatioRequested: function(ratio) {
            root.applyCustomCropAspectRatio(ratio)
        }

        onExportFormatRequested: function(format) {
            root.editorExportFormat = format
        }

        onExportScaleRequested: function(value) {
            root.editorExportScale = root.clamp(value, 0.1, 2.0)
        }

        onExportQualityRequested: function(value) {
            root.editorExportQuality = Math.round(root.clamp(value, 10, 100))
        }

        onSaveRequested: root.saveCurrentImage()
        onSaveAsRequested: root.openSaveAsDialog()

        onPresetRequested: function(name) {
            root.applyEditorPreset(name)
        }
    }

    Loader {
        id: openDialogLoader

        readonly property var dialog: item

        active: false
        asynchronous: true

        onLoaded: {
            if (dialog) {
                dialog.fileChosen.connect(root.loadImageUrl)
                dialog.open()
            }
        }
    }

    Loader {
        id: saveDialogLoader

        readonly property var dialog: item

        active: false
        asynchronous: true

        onLoaded: {
            if (dialog) {
                dialog.fileChosen.connect(root.saveEditedImage)
                dialog.open()
            }
        }
    }

    Loader {
        id: frameSaveDialogLoader

        readonly property var dialog: item

        active: false
        asynchronous: true

        onLoaded: {
            if (dialog) {
                dialog.fileChosen.connect(root.saveCurrentMotionFrame)
                dialog.open()
            }
        }
    }

    NumberAnimation {
        id: fullscreenZoomAnimation
        target: root
        property: "currentScale"
        duration: root.zoomAnimationDurationMs
        easing.type: Easing.OutCubic

        onFinished: {
            root.currentScale = root.fullscreenTargetScale
            root.fullscreenZoomAnchorActive = false
            root.clampPanToViewport()
            root.recoverFullscreenImageIfLost()
        }
    }

    ParallelAnimation {
        id: editorLayoutAnimation

        NumberAnimation {
            id: editorInsetAnimation
            target: root
            property: "editorWorkspaceInset"
            duration: root.smoothMenuMotionEnabled ? 220 : 0
            easing.type: Easing.OutCubic
        }

        NumberAnimation {
            id: editorScaleAnimation
            target: root
            property: "currentScale"
            duration: root.smoothMenuMotionEnabled ? 220 : 0
            easing.type: Easing.OutCubic
        }

        NumberAnimation {
            id: editorPanXAnimation
            target: root
            property: "panX"
            duration: root.smoothMenuMotionEnabled ? 220 : 0
            easing.type: Easing.OutCubic
        }

        NumberAnimation {
            id: editorPanYAnimation
            target: root
            property: "panY"
            duration: root.smoothMenuMotionEnabled ? 220 : 0
            easing.type: Easing.OutCubic
        }

        onFinished: {
            root.currentScale = root.fullscreenTargetScale
            root.panX = 0.0
            root.panY = 0.0
            root.recoverFullscreenImageIfLost()
        }
    }

    Timer {
        id: fullscreenApplyTimer
        interval: 16
        repeat: false
        onTriggered: {
            if (!root.visible || !root.fullScreenMode)
                return

            root.restoreFullscreenViewState()
            fullscreenRecoveryTimer.restart()
        }
    }

    Timer {
        id: fullscreenRecoveryTimer
        interval: 0
        repeat: false
        onTriggered: root.recoverFullscreenImageIfLost()
    }

    Timer {
        id: visibilitySyncTimer
        interval: 0
        repeat: false
        onTriggered: root.syncWindowVisibility()
    }

    Timer {
        id: resourceReleaseTimer
        interval: 0
        repeat: false
        onTriggered: {
            if (!root.visible)
                root.nativeWindowOps.releaseWindowResources(root)
        }
    }

    Timer {
        id: memoryTrimTimer
        interval: 1500
        repeat: false
        onTriggered: {
            if (!root.visible && !root.hasAssignedImage())
                root.nativeWindowOps.trimProcessMemory()
        }
    }

    Timer {
        id: rawMetadataProbeTimer
        // Give the scene graph at least one frame to present the embedded RAW
        // preview before synchronous camera metadata probing resumes.
        interval: 34
        repeat: false
        onTriggered: root.finishDeferredRawMetadataProbe()
    }

    Timer {
        id: fullResTimer
        interval: root.fullResUpgradeDelayMs
        repeat: false
        onTriggered: {
            if (!root.imageLimitedToPreview && !root.animatedPlaybackActive)
                root.promoteFullRes()
        }
    }

    Timer {
        id: editorPreviewTimer
        interval: 24
        repeat: false
        onTriggered: {
            const usePreview = root.imageLimitedToPreview || (root.rawPreviewFirst
                ? !root.rawFullDetailRequested : root.shouldUsePreview())
            root.previewPhase = usePreview
            root.refreshEditedPreviewSource()
            // Promote only after ImageViewport reports that the edited preview
            // is ready. Starting this timer here can replace a still-decoding
            // preview with a second full-resolution request, retaining two
            // large image buffers and defeating the preview stage.
            fullResTimer.stop()
        }
    }
}
