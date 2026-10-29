import QtQuick
import "../../qml"

Main {
    property bool realFiles: false
    currentImageUrl: ""
    residentImageUrl: ""
    windowSession: QtObject {
        property url initialImageUrl: ""
        property bool startHidden: true
        property bool persistentShell: true
        signal openRequested(url source)
    }
    imageProbe: QtObject {
        function inspect(source) {
            return realFiles ? testEditor.inspect(source) : { size: Qt.size(640, 480), withinBudget: true }
        }
    }
    photoAssetProbe: QtObject {
        signal infoReady(url source, var info)
        function cancel() {}
        function request(source) {}
    }
    motionPhotoSession: null
    motionPhotoFrameSaveCoordinator: null
    formatSupport: QtObject {
        property var exportFormats: testEditor.formats()
        property var saveNameFilters: exportFormats.map(function(f) { return f.label + " image (*." + f.suffix + ")" })
        signal directoryIndexReady()
        function canOpen(source) { return true }
        function prepareDirectoryIndex(source) {}
        function hasDirectoryIndex(source) { return true }
        function nearbyImages(source, radius, direction) {
            return realFiles ? [] : ["file:///prepared-neighbor.png"]
        }
    }
    imageSaveService: QtObject {
        property bool busy: false
        signal sizeEstimateReady(url sourceUrl, int requestId, int byteCount, string errorMessage, QtObject requestOwner)
        signal saveCompleted(url sourceUrl, url destinationUrl, QtObject requestOwner)
        signal saveFailed(url sourceUrl, string message, QtObject requestOwner)
        signal frameSaveCompleted(url sourceUrl, url destinationUrl, QtObject requestOwner)
        signal frameSaveFailed(url sourceUrl, string message, QtObject requestOwner)
        property var pending: null
        property bool rejectImmediately: false
        function save(source, destination, edits, options, requestOwner) {
            if (busy || rejectImmediately) {
                saveFailed(source, "Save refused", requestOwner || null)
                return false
            }
            pending = { source: source, destination: destination,
                        edits: edits, options: options, owner: requestOwner || null }
            busy = true
            return true
        }
        function complete(error) {
            const request = pending
            pending = null
            busy = false
            if (error)
                saveFailed(request.source, error, request.owner)
            else
                saveCompleted(request.source, request.destination, request.owner)
        }
        property var estimates: []
        function estimateSize(source, revision, edits, options, owner) {
            estimates = estimates.concat([{ source: source, revision: revision, options: options, owner: owner }])
        }
        function cancelSizeEstimate(owner) {}
    }
    nativeWindowOps: QtObject {
        signal screensChanged()
        function gestureWindowSupported() { return false }
        function currentScreenGeometry(window) { return { x: 0, y: 0, width: 1360, height: 860 } }
        function currentWindowGeometry(window) {
            return { x: window.x, y: window.y, width: window.width, height: window.height }
        }
        function windowDevicePixelRatio(window) { return 1 }
        function traceZoom(phase, scale) {}
        function releaseWindowResources(window) {}
        function trimProcessMemory() {}
        function showWindow(window, fullScreen) { return true }
    }
    backgroundModeManager: QtObject {
        property bool enabled: false
        property bool supported: false
        signal errorOccurred(string message)
    }
    viewerPreferences: QtObject {
        property bool startInFullscreen: true
        property real fullscreenBackgroundOpacity: 0.7
        property bool transparencyCheckerboardEnabled: false
        property real transparencyCheckerboardOpacity: 0.6
        property bool windowedTransparencyCheckerboardEnabled: false
        property real windowedTransparencyOpacity: 0.6
        property bool animatedImagePlaybackEnabled: true
        property real animationSpeed: 1
        property bool smoothImageScalingEnabled: true
        property bool mipmapImageScalingEnabled: false
        property bool pixelAlignedRenderingEnabled: false
        property bool fullResolutionRenderingEnabled: true
        property bool colorManagedRenderingEnabled: true
        property real cropShieldOpacity: 0.5
        property int maximumImageMemoryMiB: 8192
        property int minimumImageMemoryMiB: 384
        property int maximumImageMemoryLimitMiB: 32768
        property int maximumImageMegapixels: 512
        signal errorOccurred(string message)
    }
    windowManager: QtObject {
        property url defaultImageUrl: "file:///default.png"
        property var nearbyRequests: []
        function markParallelPreviewPresented(pair) {}
        function prepareFileDialog(window) {}
        property var prepared: ["file:///previous-neighbor.png"]
        signal navigationKeyReleased(var window, int direction)
        signal quitBlocked(string reason)
        function hasDecodedFullBase(source) { return false }
        function releasePictureResources() { prepared = [] }
        function releasePictureResourcesForNavigation(source) {}
        function prepareNearbyPreviews(sources, size, memory) {
            nearbyRequests.push(sources)
            prepared = sources
        }
    }
}
