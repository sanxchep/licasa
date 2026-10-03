/*!
    ImageViewport.qml
    -----------------
    Image rendering, backdrop, pointer interaction, wheel zoom,
    and drag-and-drop handling for Licasa.

    Windowed mode behavior is intentionally simple:
    - at rest, the native window is exactly the displayed image size
    - dragging moves the window
    - wheel zoom uses a transparent fixed surface while the exact window
      prepares its final size behind it on supported X11 desktops
    - a temporary surface accepts input only inside the displayed image
*/

import QtQuick
import "ViewerMath.js" as ViewerMath

Item {
    id: root

    required property bool fullScreenMode
    required property real fullscreenBackdropOpacity
    required property bool transparencyCheckerboardEnabled
    required property real transparencyCheckerboardOpacity
    required property bool windowedTransparencyCheckerboardEnabled
    required property real windowedTransparencyOpacity
    required property bool hasImage

    required property real imageX
    required property real imageY
    required property real imageWidth
    required property real imageHeight
    required property real currentScale

    required property string providerSource
    // The gesture surface selects a bounded source for large images and
    // mirrors the exact source for smaller ones.
    property string sourceOverride: ""
    property size sourceOverrideSize: Qt.size(0, 0)
    property bool presentationOnly: false
    required property bool previewPhase
    required property bool rawFastPhase
    required property bool rawPreviewFirst
    property bool parallelFullResolutionEnabled: false
    required property int previewSourceWidth
    required property int previewSourceHeight

    property url animatedSource: ""
    property var animationController: null
    property var motionPhotoSession: null
    property bool animatedPlaybackActive: false
    property bool floatingPlaybackEnabled: false
    property bool playbackInteractionEnabled: true
    readonly property var pointerStyleHints: Qt.styleHints
    property real animationSpeed: 1.0
    property bool smoothScalingEnabled: true
    property bool mipmapScalingEnabled: true
    property bool pixelAlignedRenderingEnabled: true
    property real devicePixelRatio: 1.0
    // The image and its backing must share the same scene coordinates. Rounding
    // only the image left a fractional transparent strip at fit scale.
    // A native resize can complete a frame after its requested zoom scale.
    // In floating mode the window itself is the image boundary, so render from
    // its actual size to avoid exposing an old image at either corner.
    readonly property real renderedImageX: fullScreenMode ? alignedCoordinate(imageX) : 0
    readonly property real renderedImageY: fullScreenMode ? alignedCoordinate(imageY) : 0
    readonly property real renderedImageRight: fullScreenMode
        ? renderedImageX + imageWidth * currentScale : width
    readonly property real renderedImageBottom: fullScreenMode
        ? renderedImageY + imageHeight * currentScale : height
    readonly property real visibleImageLeft: Math.max(0, Math.min(width, renderedImageX))
    readonly property real visibleImageTop: Math.max(0, Math.min(height, renderedImageY))
    readonly property real visibleImageRight: Math.max(0, Math.min(width, renderedImageRight))
    readonly property real visibleImageBottom: Math.max(0, Math.min(height, renderedImageBottom))
    readonly property real visibleImageBandHeight: Math.max(0, visibleImageBottom - visibleImageTop)

    required property real fullScreenWheelSensitivity
    required property real windowedWheelSensitivity
    required property real maxWheelDeltaPerEvent
    required property bool interactionBlocked

    required property var canOpen

    property int activeImageSlot: -1
    property int pendingImageSlot: -1
    property int fadingImageSlot: -1
    property rect fadingViewportRect: Qt.rect(0, 0, 0, 0)
    property string pendingRequestKey: ""
    property string speculativeFullKey: ""
    readonly property string parallelPairOrigin: String(Math.random()).slice(2)
    property int parallelPairGeneration: 0
    readonly property string parallelPairId: parallelPairOrigin + "-" + parallelPairGeneration
    readonly property bool speculativeFullAvailable: speculativeFullKey.length > 0
        && (photoC.status === Image.Loading || photoC.status === Image.Ready)
    property bool imageLoadFailed: false
    readonly property bool decodePending: imageRequestTimer.running || pendingImageSlot >= 0
    readonly property int progressiveFadeDurationMs: 120

    readonly property var activeImage: activeImageSlot === 0
        ? photoA
        : activeImageSlot === 1
            ? photoB
            : activeImageSlot === 2 ? photoC : null
    readonly property string activeRequestKey: activeImage ? String(activeImage.source) : ""
    readonly property size activeRequestSize: activeImage ? activeImage.sourceSize : Qt.size(0, 0)
    readonly property var pendingImage: pendingImageSlot === 0
        ? photoA
        : pendingImageSlot === 1
            ? photoB
            : pendingImageSlot === 2 ? photoC : null
    readonly property bool effectiveTransparencyCheckerboardEnabled:
        fullScreenMode && !presentationOnly
            ? transparencyCheckerboardEnabled
            : windowedTransparencyCheckerboardEnabled
    readonly property real effectiveTransparencyOpacity:
        fullScreenMode && !presentationOnly
            ? transparencyCheckerboardOpacity
            : windowedTransparencyOpacity
    readonly property int staticImageStatus: activeImage && activeImage.status === Image.Ready
        ? Image.Ready
        : pendingImage
            ? pendingImage.status
            : (imageLoadFailed ? Image.Error
                : providerSource.length > 0 ? Image.Loading : Image.Null)
    readonly property bool animatedFrameReady: animatedPlaybackActive
        && animatedPhoto.status === Image.Ready
    readonly property int imageStatus: animatedPlaybackActive
        ? (animatedFrameReady || staticImageStatus === Image.Ready
            ? Image.Ready
            : animatedPhoto.status === Image.Loading
                ? Image.Loading
                : staticImageStatus)
        : staticImageStatus
    readonly property real imageImplicitWidth: animatedFrameReady
        ? animatedPhoto.implicitWidth
        : activeImage ? activeImage.implicitWidth : 0
    readonly property real imageImplicitHeight: animatedFrameReady
        ? animatedPhoto.implicitHeight
        : activeImage ? activeImage.implicitHeight : 0
    readonly property int animationFrameCount: animationController ? animationController.frameCount : 0
    readonly property int animationCurrentFrame: animationController ? animationController.currentFrame : 0
    readonly property var inputRegion: {
        if (root.fullScreenMode)
            return ViewerMath.boundedRect(0, 0, root.width, root.height, root.width, root.height)

        if (root.hasImage) {
            return ViewerMath.boundedRect(
                root.renderedImageX,
                root.renderedImageY,
                root.renderedImageRight - root.renderedImageX,
                root.renderedImageBottom - root.renderedImageY,
                root.width,
                root.height
            )
        }

        return ViewerMath.boundedRect(
            emptyState.x,
            emptyState.y,
            emptyState.width,
            emptyState.height,
            root.width,
            root.height
        )
    }

    signal panRequested(real dx, real dy)
    signal disableFitModeRequested()
    signal cancelZoomAnimationRequested()
    signal windowMoveStarted()
    signal windowMoveUpdated()
    signal windowMoveFinished()
    signal clampPanRequested()
    signal promoteFullResRequested()
    signal zoomRequested(real factor, real px, real py)
    signal exitFullscreenToImageRequested()
    signal enterFullscreenRequested()
    signal openDialogRequested()
    signal openUrlRequested(var url)
    signal imageReady()
    signal imageRequestFailed()
    signal playbackToggleRequested()

    // Wait out the desktop's double-click interval before committing a single
    // click. Entering fullscreen must never briefly start or pause playback.
    Timer {
        id: playbackClickTimer
        interval: root.pointerStyleHints.mouseDoubleClickInterval
        onTriggered: {
            if (!root.fullScreenMode && root.floatingPlaybackEnabled
                    && root.playbackInteractionEnabled && !root.interactionBlocked)
                root.playbackToggleRequested()
        }
    }

    onFullScreenModeChanged: playbackClickTimer.stop()
    onFloatingPlaybackEnabledChanged: playbackClickTimer.stop()
    onPlaybackInteractionEnabledChanged: playbackClickTimer.stop()
    onInteractionBlockedChanged: playbackClickTimer.stop()
    onAnimatedSourceChanged: playbackClickTimer.stop()

    function requestKeyForStage(stage, parallelStage, requestedWidth, requestedHeight) {
        if (root.providerSource.length === 0)
            return ""

        const separator = root.providerSource.indexOf("?") >= 0 ? "&" : "?"
        const boundedStage = stage === "preview" || stage === "raw-fast"
        const previewWidth = requestedWidth > 0 ? requestedWidth : root.previewSourceWidth
        const previewHeight = requestedHeight > 0 ? requestedHeight : root.previewSourceHeight
        return root.providerSource
            + separator
            + "licasa_stage=" + stage
            + "&licasa_width=" + (boundedStage ? previewWidth : 0)
            + "&licasa_height=" + (boundedStage ? previewHeight : 0)
            + (parallelStage.length > 0 ? "&licasa_parallel=" + parallelStage : "")
            + (parallelStage.length > 0
                ? "&licasa_pair=" + root.parallelPairId
                : "")
    }

    function requestKey() {
        if (root.sourceOverride.length > 0)
            return root.sourceOverride
        const stage = root.rawFastPhase ? "raw-fast"
            : (root.previewPhase ? "preview"
                : (root.rawPreviewFirst ? "raw-interactive" : "full"))
        const parallelStage = stage === "preview" && root.parallelFullResolutionEnabled
            ? "preview"
            : ""
        return requestKeyForStage(stage, parallelStage)
    }

    function fullRequestKey() {
        return requestKeyForStage(root.rawPreviewFirst ? "raw-interactive" : "full", "full")
    }

    function requestCurrentImage() {
        if (root.animatedFrameReady) {
            releaseStaticImages()
            return
        }

        if (root.speculativeFullKey.length > 0 &&
                root.speculativeFullKey !== root.fullRequestKey() &&
                root.activeImageSlot !== 2 && root.fadingImageSlot !== 2) {
            root.speculativeFullKey = ""
            photoC.source = ""
        }

        if (root.sourceOverride.length === 0 && root.providerSource.length > 0 && root.previewPhase &&
                root.parallelFullResolutionEnabled &&
                root.activeImageSlot < 0 && root.speculativeFullKey.length === 0) {
            root.parallelPairGeneration += 1
            root.speculativeFullKey = root.fullRequestKey()
            photoC.sourceSize = Qt.size(0, 0)
            photoC.source = root.speculativeFullKey
        }

        const key = requestKey()
        if (key.length === 0) {
            releaseStaticImages()
            return
        }

        if (key === pendingRequestKey)
            return

        if (!root.previewPhase && !root.rawFastPhase &&
                root.speculativeFullKey === root.fullRequestKey()) {
            if (photoC.status === Image.Ready || photoC.status === Image.Loading) {
                pendingImageSlot = 2
                pendingRequestKey = root.speculativeFullKey
                imageLoadFailed = false
                if (photoC.status === Image.Ready)
                    completeImageRequest(2)
                return
            }
            // A speculative decode can fail admission or decoding. Keep the
            // ready preview and retry through the normal serial full path.
            speculativeFullKey = ""
            photoC.source = ""
        }

        const targetSlot = activeImageSlot === 0 ? 1 : 0
        const target = targetSlot === 0 ? photoA : photoB

        // A rapid second edit can reuse the slot that is still fading out.
        // Finish that handoff before replacing its source; the current image
        // remains fully visible underneath throughout the next decode.
        if (targetSlot === fadingImageSlot)
            retireFadingImage()

        pendingImageSlot = targetSlot
        pendingRequestKey = key
        imageLoadFailed = false
        target.sourceSize = root.sourceOverride.length > 0
            ? root.sourceOverrideSize
            : (root.previewPhase || root.rawFastPhase)
                ? Qt.size(root.previewSourceWidth, root.previewSourceHeight)
                : Qt.size(0, 0)
        target.source = key
        // A superseded request can leave this slot already decoded for the
        // same key. Reassigning an unchanged source emits no status change.
        if (target.status === Image.Ready)
            completeImageRequest(targetSlot)
    }

    function releaseStaticImages() {
        imageRequestTimer.stop()
        progressiveFadeCleanup.stop()
        pendingImageSlot = -1
        activeImageSlot = -1
        fadingImageSlot = -1
        pendingRequestKey = ""
        speculativeFullKey = ""
        imageLoadFailed = false
        photoA.source = ""
        photoB.source = ""
        photoC.source = ""
        photoA.sourceSize = Qt.size(0, 0)
        photoB.sourceSize = Qt.size(0, 0)
        photoC.sourceSize = Qt.size(0, 0)
    }

    function retireFadingImage() {
        const slot = fadingImageSlot
        if (slot < 0)
            return
        progressiveFadeCleanup.stop()
        fadingImageSlot = -1
        if (slot === activeImageSlot || slot === pendingImageSlot)
            return
        const image = slot === 0 ? photoA : slot === 1 ? photoB : photoC
        image.source = ""
        image.sourceSize = Qt.size(0, 0)
    }

    function pendingRequestIsCurrent(slot) {
        if (pendingRequestKey.length === 0)
            return true
        if (pendingRequestKey === requestKey())
            return true
        return slot === 2 && !previewPhase && !rawFastPhase
            && pendingRequestKey === fullRequestKey()
    }

    function completeImageRequest(slot) {
        if (slot !== pendingImageSlot)
            return
        if (!pendingRequestIsCurrent(slot)) {
            pendingImageSlot = -1
            pendingRequestKey = ""
            imageRequestTimer.restart()
            return
        }

        retireFadingImage()
        const previousSlot = activeImageSlot
        if (previousSlot >= 0 && previousSlot !== slot) {
            // The new image can have a different fit or aspect ratio. Keep the
            // outgoing image at its last on-screen bounds during the fade.
            fadingViewportRect = Qt.rect(
                renderedImageX, renderedImageY,
                fullScreenMode ? imageWidth * currentScale : width,
                fullScreenMode ? imageHeight * currentScale : height
            )
            fadingImageSlot = previousSlot
        }
        activeImageSlot = slot
        pendingImageSlot = -1
        pendingRequestKey = ""
        imageLoadFailed = false
        if (slot === 2)
            speculativeFullKey = ""

        if (fadingImageSlot >= 0)
            progressiveFadeCleanup.restart()

        root.imageReady()
    }

    function failImageRequest(slot) {
        if (slot !== pendingImageSlot)
            return
        if (!pendingRequestIsCurrent(slot)) {
            pendingImageSlot = -1
            pendingRequestKey = ""
            imageRequestTimer.restart()
            return
        }
        pendingImageSlot = -1
        pendingRequestKey = ""
        imageLoadFailed = activeImageSlot < 0
        root.imageRequestFailed()
    }

    onProviderSourceChanged: {
        playbackClickTimer.stop()
        imageRequestTimer.restart()
    }
    onSourceOverrideChanged: imageRequestTimer.restart()
    onSourceOverrideSizeChanged: imageRequestTimer.restart()
    onPreviewPhaseChanged: imageRequestTimer.restart()
    onRawFastPhaseChanged: imageRequestTimer.restart()
    onParallelFullResolutionEnabledChanged: {
        if (!parallelFullResolutionEnabled && previewPhase &&
                activeImageSlot !== 2 && fadingImageSlot !== 2) {
            speculativeFullKey = ""
            photoC.source = ""
        }
    }
    onAnimatedPlaybackActiveChanged: {
        if (root.animatedFrameReady)
            root.releaseStaticImages()
        else if (!root.animatedPlaybackActive)
            imageRequestTimer.restart()
    }

    Component.onCompleted: imageRequestTimer.restart()

    Timer {
        id: imageRequestTimer
        interval: 0
        repeat: false
        onTriggered: root.requestCurrentImage()
    }

    Timer {
        id: progressiveFadeCleanup
        interval: root.progressiveFadeDurationMs + 20
        repeat: false
        onTriggered: root.retireFadingImage()
    }

    function imageRight() {
        return root.renderedImageRight
    }

    function imageBottom() {
        return root.renderedImageBottom
    }

    function pointHitsImage(px, py) {
        if (!root.hasImage)
            return false

        return px >= root.renderedImageX
            && px <= imageRight()
            && py >= root.renderedImageY
            && py <= imageBottom()
    }

    function alignedCoordinate(value) {
        if (!pixelAlignedRenderingEnabled)
            return value

        const dpr = Math.max(1.0, devicePixelRatio)
        return Math.round(value * dpr) / dpr
    }

    clip: true

    Rectangle {
        anchors.fill: parent
        visible: root.fullScreenMode && !root.presentationOnly
            && root.fullscreenBackdropOpacity > 0.0 && !root.hasImage
        color: Qt.rgba(20 / 255, 20 / 255, 20 / 255, root.fullscreenBackdropOpacity)
    }

    Rectangle {
        x: 0
        y: 0
        width: root.width
        height: root.visibleImageTop
        visible: root.fullScreenMode && !root.presentationOnly
            && root.fullscreenBackdropOpacity > 0.0 && root.hasImage
        color: Qt.rgba(20 / 255, 20 / 255, 20 / 255, root.fullscreenBackdropOpacity)
    }

    Rectangle {
        x: 0
        y: root.visibleImageBottom
        width: root.width
        height: Math.max(0, root.height - root.visibleImageBottom)
        visible: root.fullScreenMode && !root.presentationOnly
            && root.fullscreenBackdropOpacity > 0.0 && root.hasImage
        color: Qt.rgba(20 / 255, 20 / 255, 20 / 255, root.fullscreenBackdropOpacity)
    }

    Rectangle {
        x: 0
        y: root.visibleImageTop
        width: root.visibleImageLeft
        height: root.visibleImageBandHeight
        visible: root.fullScreenMode && !root.presentationOnly
            && root.fullscreenBackdropOpacity > 0.0 && root.hasImage
        color: Qt.rgba(20 / 255, 20 / 255, 20 / 255, root.fullscreenBackdropOpacity)
    }

    Rectangle {
        x: root.visibleImageRight
        y: root.visibleImageTop
        width: Math.max(0, root.width - root.visibleImageRight)
        height: root.visibleImageBandHeight
        visible: root.fullScreenMode && !root.presentationOnly
            && root.fullscreenBackdropOpacity > 0.0 && root.hasImage
        color: Qt.rgba(20 / 255, 20 / 255, 20 / 255, root.fullscreenBackdropOpacity)
    }

    Item {
        id: transparentPixelBackdrop
        x: root.visibleImageLeft
        y: root.visibleImageTop
        width: Math.max(0, root.visibleImageRight - root.visibleImageLeft)
        height: Math.max(0, root.visibleImageBottom - root.visibleImageTop)
        visible: root.hasImage && width > 0 && height > 0
        opacity: root.effectiveTransparencyCheckerboardEnabled
            ? root.effectiveTransparencyOpacity
            : 1.0
        clip: true

        Rectangle {
            anchors.fill: parent
            visible: !root.effectiveTransparencyCheckerboardEnabled
            color: Qt.rgba(
                20 / 255,
                20 / 255,
                20 / 255,
                root.fullScreenMode && !root.presentationOnly
                    ? root.fullscreenBackdropOpacity
                    : root.windowedTransparencyOpacity
            )
        }

        Canvas {
            id: transparencyGrid
            anchors.fill: parent
            visible: root.effectiveTransparencyCheckerboardEnabled

            onPaint: {
                const ctx = getContext("2d")
                const tile = 18
                ctx.clearRect(0, 0, width, height)
                ctx.fillStyle = "#C8C8C8"
                ctx.fillRect(0, 0, width, height)
                ctx.fillStyle = "#929292"

                for (let row = 0, y = 0; y < height; ++row, y += tile) {
                    const firstColumn = row % 2
                    for (let x = firstColumn * tile; x < width; x += tile * 2) {
                        ctx.fillRect(x, y, tile, tile)
                    }
                }
            }

            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onVisibleChanged: {
                if (visible)
                    requestPaint()
            }
        }
    }

    Item {
        id: imageStage
        objectName: "imageStage"
        visible: root.imageStatus === Image.Ready
        x: root.renderedImageX
        y: root.renderedImageY
        width: root.fullScreenMode ? Math.max(1, root.imageWidth) : root.width
        height: root.fullScreenMode ? Math.max(1, root.imageHeight) : root.height
        scale: root.fullScreenMode ? root.currentScale : 1.0
        transformOrigin: Item.TopLeft

        readonly property real fadingScale: Math.max(0.0001, scale)
        readonly property real fadingX: (root.fadingViewportRect.x - x) / fadingScale
        readonly property real fadingY: (root.fadingViewportRect.y - y) / fadingScale
        readonly property real fadingWidth: root.fadingViewportRect.width / fadingScale
        readonly property real fadingHeight: root.fadingViewportRect.height / fadingScale

        Image {
            id: photoA
            objectName: "staticImageA"
            x: root.fadingImageSlot === 0 && root.fullScreenMode ? imageStage.fadingX : 0
            y: root.fadingImageSlot === 0 && root.fullScreenMode ? imageStage.fadingY : 0
            width: root.fadingImageSlot === 0 && root.fullScreenMode
                ? imageStage.fadingWidth : parent.width
            height: root.fadingImageSlot === 0 && root.fullScreenMode
                ? imageStage.fadingHeight : parent.height
            visible: (root.activeImageSlot === 0 || root.fadingImageSlot === 0)
                && !root.animatedFrameReady
            z: root.fadingImageSlot === 0 ? 1 : 0
            opacity: root.fadingImageSlot === 0 && root.activeImageSlot !== 0 ? 0.0 : 1.0
            asynchronous: true
            cache: false
            smooth: root.smoothScalingEnabled
            mipmap: root.mipmapScalingEnabled
            fillMode: Image.Stretch

            Behavior on opacity {
                enabled: root.fadingImageSlot === 0
                NumberAnimation { duration: root.progressiveFadeDurationMs; easing.type: Easing.OutQuad }
            }

            onStatusChanged: {
                if (status === Image.Ready)
                    root.completeImageRequest(0)
                else if (status === Image.Error)
                    root.failImageRequest(0)
            }
        }

        Image {
            id: photoB
            objectName: "staticImageB"
            x: root.fadingImageSlot === 1 && root.fullScreenMode ? imageStage.fadingX : 0
            y: root.fadingImageSlot === 1 && root.fullScreenMode ? imageStage.fadingY : 0
            width: root.fadingImageSlot === 1 && root.fullScreenMode
                ? imageStage.fadingWidth : parent.width
            height: root.fadingImageSlot === 1 && root.fullScreenMode
                ? imageStage.fadingHeight : parent.height
            visible: (root.activeImageSlot === 1 || root.fadingImageSlot === 1)
                && !root.animatedFrameReady
            z: root.fadingImageSlot === 1 ? 1 : 0
            opacity: root.fadingImageSlot === 1 && root.activeImageSlot !== 1 ? 0.0 : 1.0
            asynchronous: true
            cache: false
            smooth: root.smoothScalingEnabled
            mipmap: root.mipmapScalingEnabled
            fillMode: Image.Stretch

            Behavior on opacity {
                enabled: root.fadingImageSlot === 1
                NumberAnimation { duration: root.progressiveFadeDurationMs; easing.type: Easing.OutQuad }
            }

            onStatusChanged: {
                if (status === Image.Ready)
                    root.completeImageRequest(1)
                else if (status === Image.Error)
                    root.failImageRequest(1)
            }
        }

        Image {
            id: photoC
            objectName: "staticImageC"
            x: root.fadingImageSlot === 2 && root.fullScreenMode ? imageStage.fadingX : 0
            y: root.fadingImageSlot === 2 && root.fullScreenMode ? imageStage.fadingY : 0
            width: root.fadingImageSlot === 2 && root.fullScreenMode
                ? imageStage.fadingWidth : parent.width
            height: root.fadingImageSlot === 2 && root.fullScreenMode
                ? imageStage.fadingHeight : parent.height
            visible: (root.activeImageSlot === 2 || root.fadingImageSlot === 2)
                && !root.animatedFrameReady
            z: root.fadingImageSlot === 2 ? 1 : 0
            opacity: root.fadingImageSlot === 2 && root.activeImageSlot !== 2 ? 0.0 : 1.0
            asynchronous: true
            cache: false
            smooth: root.smoothScalingEnabled
            mipmap: root.mipmapScalingEnabled
            fillMode: Image.Stretch

            Behavior on opacity {
                enabled: root.fadingImageSlot === 2
                NumberAnimation { duration: root.progressiveFadeDurationMs; easing.type: Easing.OutQuad }
            }

            onStatusChanged: {
                if (status === Image.Ready)
                    root.completeImageRequest(2)
                else if (status === Image.Error)
                    root.failImageRequest(2)
            }
        }

        Image {
            id: animatedPhoto

            anchors.fill: parent
            visible: root.animatedFrameReady
            source: root.animatedPlaybackActive && root.animationController
                ? root.animationController.frameSource : ""
            // The frame provider only returns an already decoded, shared image.
            // Synchronous lookup avoids a queue of obsolete frame requests.
            asynchronous: false
            cache: false
            smooth: root.smoothScalingEnabled
            mipmap: root.mipmapScalingEnabled
            fillMode: Image.Stretch

            onStatusChanged: {
                if (status === Image.Ready) {
                    if (root.animatedPlaybackActive)
                        root.releaseStaticImages()
                    root.imageReady()
                }
            }
        }

        // Native presentation only. Keep the surface itself lazy so ordinary
        // JPEG/PNG windows allocate no temporal presentation object. Once a
        // validated Motion Photo exists, C++ pulls an owned bounded RGBA frame
        // from MotionPhotoSession; QML never receives QVideoFrame, QVideoSink,
        // decoder handles or raw pixel buffers. The still image remains resident
        // underneath for instant pause/stop/error fallback.
        Loader {
            id: motionPhotoFrameLoader
            anchors.fill: parent
            z: 3

            // Keep every binding that reaches back into ImageViewport on the
            // Loader itself. The nested Component has its own creation context,
            // so its visual object remains self-contained.
            property var motionSession: root.motionPhotoSession
            property bool frameSmooth: root.smoothScalingEnabled

            active: motionSession && motionSession.available
            // The native surface paints transparently until its first valid
            // frame, so visibility does not need to inspect Loader.item. This
            // also keeps the Loader boundary statically type-safe for QML lint.
            visible: active && !root.animatedFrameReady

            onLoaded: {
                item.session = motionSession
                item.smooth = frameSmooth
            }
            onMotionSessionChanged: {
                if (item)
                    item.session = motionSession
            }
            onFrameSmoothChanged: {
                if (item)
                    item.smooth = frameSmooth
            }

            // Keep the C++ presentation type out of this always-loaded QML
            // compilation unit. The tiny host imports the generated Licasa QML
            // module only when a Motion Photo is actually active, which both
            // preserves ordinary-still startup and activates the C++ type
            // registrar before MotionPhotoFrameSurface is instantiated.
            source: active ? Qt.resolvedUrl("MotionPhotoFrameSurfaceHost.qml") : ""
        }
    }

    Rectangle {
        id: emptyState
        objectName: "emptyImageState"
        anchors.centerIn: parent
        readonly property bool compact: root.width < 320 || root.height < 200
        visible: !root.presentationOnly && root.imageStatus !== Image.Ready
            && root.width >= 180 && root.height >= 110
        z: 20
        width: Math.min(420, Math.max(0, parent.width - 24))
        height: Math.min(parent.height - 24, compact ? 104 : 176)
        radius: 24
        color: Qt.rgba(18 / 255, 20 / 255, 24 / 255, 0.94)
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.16)
        antialiasing: true

        Column {
            anchors.centerIn: parent
            spacing: emptyState.compact ? 8 : 14

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                color: "#f2f2f2"
                font.pixelSize: emptyState.compact ? 16 : 20
                font.weight: Font.DemiBold
                renderType: Text.NativeRendering
                horizontalAlignment: Text.AlignHCenter
                text: root.imageStatus === Image.Loading
                    ? "Opening image…"
                    : root.imageStatus === Image.Error
                        ? emptyState.compact ? "Open failed"
                                             : "That image could not be opened"
                        : emptyState.compact ? "Open an image" : "Drop an image here"
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !emptyState.compact && root.imageStatus !== Image.Loading
                color: Qt.rgba(1, 1, 1, 0.68)
                font.pixelSize: 12
                renderType: Text.NativeRendering
                text: root.imageStatus === Image.Error
                    ? "Choose another local image file"
                    : "or choose one from your computer"
            }

            Rectangle {
                id: openImageButton
                objectName: "emptyStateOpenButton"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: root.imageStatus !== Image.Loading
                width: 122
                height: 38
                radius: height / 2
                color: openButton.pressed ? "#2466D8" : openButton.containsMouse ? "#347CF2" : "#2A72E8"
                border.width: activeFocus ? 2 : 1
                border.color: activeFocus ? "#D8FFFFFF" : "#50FFFFFF"
                activeFocusOnTab: visible

                Accessible.role: Accessible.Button
                Accessible.name: "Open image"
                Accessible.focusable: visible
                Accessible.focused: activeFocus
                Accessible.onPressAction: root.openDialogRequested()

                Keys.onPressed: function(event) {
                    if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                            && event.key !== Qt.Key_Enter)
                        return
                    if (!event.isAutoRepeat)
                        root.openDialogRequested()
                    event.accepted = true
                }

                Text {
                    anchors.centerIn: parent
                    text: "Open image"
                    color: "white"
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    id: openButton
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onPressed: openImageButton.forceActiveFocus()
                    onClicked: root.openDialogRequested()
                }
            }
        }
    }

    function normalizedWheelDelta(wheel) {
        // Precision touchpads commonly provide both delta types. Prefer the
        // pixel distance in that case; the angle delta can be so small that a
        // zoom step is effectively invisible.
        if (wheel.pixelDelta.y !== 0)
            return wheel.pixelDelta.y * 6.0

        if (wheel.angleDelta.y !== 0)
            return wheel.angleDelta.y

        return 0
    }

    function handleWheel(wheel, px, py) {
        if (root.imageStatus !== Image.Ready || root.interactionBlocked)
            return

        playbackClickTimer.stop()

        const rawDelta = root.normalizedWheelDelta(wheel)
        if (rawDelta === 0)
            return

        root.promoteFullResRequested()

        const sensitivity = root.fullScreenMode
            ? root.fullScreenWheelSensitivity
            : root.windowedWheelSensitivity
        const factor = ViewerMath.wheelFactor(
            sensitivity,
            rawDelta,
            root.maxWheelDeltaPerEvent
        )

        root.zoomRequested(factor, px, py)
        wheel.accepted = true
    }

    Item {
        id: wheelInputLayer

        x: root.renderedImageX
        y: root.renderedImageY
        width: Math.max(0, root.renderedImageRight - root.renderedImageX)
        height: Math.max(0, root.renderedImageBottom - root.renderedImageY)
        z: 11
        visible: root.imageStatus === Image.Ready
        enabled: visible && !root.interactionBlocked

        WheelHandler {
            id: imageWheelHandler

            target: null
            orientation: Qt.Vertical
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            blocking: true

            onWheel: function(wheel) {
                const viewportPoint = wheelInputLayer.mapToItem(
                    root,
                    point.position.x,
                    point.position.y
                )
                root.handleWheel(wheel, viewportPoint.x, viewportPoint.y)
            }
        }
    }

    MouseArea {
        id: pointerLayer
        anchors.fill: parent
        z: 10
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        enabled: !root.interactionBlocked

        readonly property bool hoverOnImage:
            containsMouse
            && root.imageStatus === Image.Ready
            && root.pointHitsImage(mouseX, mouseY)

        property real lastX: 0
        property real lastY: 0
        property point pressPosition: Qt.point(0, 0)
        property bool dragged: false
        property bool pressedOnImage: false

        cursorShape: root.fullScreenMode
            ? ((hoverOnImage && pressed)
                ? Qt.ClosedHandCursor
                : ((hoverOnImage && root.imageStatus === Image.Ready) ? Qt.OpenHandCursor : Qt.ArrowCursor))
            : ((containsMouse && root.imageStatus === Image.Ready)
                ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                : Qt.ArrowCursor)

        onPressed: function(mouse) {
            pressPosition = Qt.point(mouse.x, mouse.y)
            dragged = false
            pressedOnImage = root.imageStatus === Image.Ready
                && root.pointHitsImage(mouse.x, mouse.y)
            if (root.imageStatus !== Image.Ready)
                return

            // Playback intent must not enqueue a still-image promotion that
            // closes its own admission gate before the single-click timer fires.
            if (!root.floatingPlaybackEnabled)
                root.promoteFullResRequested()
            root.cancelZoomAnimationRequested()

            if (!root.fullScreenMode) {
                if (root.pointHitsImage(mouse.x, mouse.y) && !root.floatingPlaybackEnabled)
                    root.windowMoveStarted()
                return
            }

            if (!root.pointHitsImage(mouse.x, mouse.y))
                return

            root.disableFitModeRequested()
            lastX = mouse.x
            lastY = mouse.y
        }

        onPositionChanged: function(mouse) {
            if (root.imageStatus !== Image.Ready || !pressed || !pressedOnImage)
                return

            if (Math.abs(mouse.x - pressPosition.x) >= root.pointerStyleHints.startDragDistance
                    || Math.abs(mouse.y - pressPosition.y) >= root.pointerStyleHints.startDragDistance) {
                if (!dragged && !root.fullScreenMode && root.floatingPlaybackEnabled)
                    root.windowMoveStarted()
                dragged = true
                playbackClickTimer.stop()
            }

            if (!root.fullScreenMode) {
                root.windowMoveUpdated()
                return
            }

            root.panRequested(mouse.x - lastX, mouse.y - lastY)
            lastX = mouse.x
            lastY = mouse.y
        }

        onReleased: {
            if (root.fullScreenMode)
                root.clampPanRequested()
            else
                root.windowMoveFinished()
        }

        onCanceled: {
            playbackClickTimer.stop()
            if (root.fullScreenMode)
                root.clampPanRequested()
            else
                root.windowMoveFinished()
        }

        onClicked: function(mouse) {
            if (pressedOnImage && !dragged && !root.fullScreenMode && root.floatingPlaybackEnabled
                    && root.playbackInteractionEnabled && root.imageStatus === Image.Ready
                    && root.pointHitsImage(mouse.x, mouse.y))
                playbackClickTimer.restart()
        }

        onDoubleClicked: function(mouse) {
            playbackClickTimer.stop()
            if (root.imageStatus !== Image.Ready
                    || !root.pointHitsImage(mouse.x, mouse.y))
                return

            if (root.fullScreenMode) {
                root.exitFullscreenToImageRequested()
                return
            }

            root.enterFullscreenRequested()
        }
    }

    DropArea {
        anchors.fill: parent

        onDropped: function(drop) {
            if (drop.hasUrls && drop.urls.length > 0) {
                const u = drop.urls[0]
                if (!root.canOpen || root.canOpen(u)) {
                    root.openUrlRequested(u)
                    drop.acceptProposedAction()
                }
            }
        }
    }
}
