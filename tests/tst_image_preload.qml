import QtQuick
import QtTest
import "../qml"
import "../qml/FileUrls.js" as FileUrls

Item {
    id: visualRoot
    width: 640
    height: 480

    Component {
        id: viewportComponent
        ImageViewport {
            property url sourceFile: "file:///photo.png"
            width: 320
            height: 240
            fullScreenMode: false
            fullscreenBackdropOpacity: 0.7
            transparencyCheckerboardEnabled: false
            transparencyCheckerboardOpacity: 0.6
            windowedTransparencyCheckerboardEnabled: false
            windowedTransparencyOpacity: 0.6
            hasImage: true
            imageX: 0
            imageY: 0
            imageWidth: 640
            imageHeight: 480
            currentScale: 0.5
            providerSource: "image://previewtest/photo.png"
            previewPhase: true
            rawFastPhase: false
            rawPreviewFirst: false
            previewFirstPreloadEnabled: FileUrls.previewFirstFullResolution(sourceFile)
            parallelFullResolutionEnabled: !previewFirstPreloadEnabled
            previewSourceWidth: 160
            previewSourceHeight: 120
            fullScreenWheelSensitivity: 1.0015
            windowedWheelSensitivity: 1.0009
            maxWheelDeltaPerEvent: 120
            interactionBlocked: false
            canOpen: function() { return true }
        }
    }

    Component {
        id: mainComponent
        Main {
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
                    return { size: Qt.size(640, 480), withinBudget: true }
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
                property var saveNameFilters: ["PNG image (*.png)", "JPEG image (*.jpg *.jpeg)",
                                               "All writable images (*.png *.jpg *.jpeg *.tiff)"]
                signal directoryIndexReady()
                function canOpen(source) { return true }
                function prepareDirectoryIndex(source) {}
                function hasDirectoryIndex(source) { return true }
                function nearbyImages(source, radius, direction) {
                    return ["file:///prepared-neighbor.png"]
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
    }

    TestCase {
        name: "ImagePreload"
        when: windowShown
        property var viewport: null
        property var mainWindow: null
        property var otherMainWindow: null

        function init() { testPreviewRequests.reset() }
        function cleanup() {
            if (otherMainWindow) {
                otherMainWindow.clearImageState()
                otherMainWindow.destroy()
                otherMainWindow = null
            }
            if (mainWindow) {
                mainWindow.clearImageState()
                mainWindow.destroy()
                mainWindow = null
            }
            if (viewport)
                viewport.releaseStaticImages()
            tryVerify(function() {
                return testPreviewRequests.requests().every(function(request) {
                    return request.finished
                })
            }, 5000)
            viewport = null
        }
        function makeMain() {
            mainWindow = mainComponent.createObject(null)
            verify(mainWindow)
            const image = findChild(mainWindow, "imageViewport")
            verify(image)
            // Keep the real Main hidden while the controlled provider and
            // explicit frameSwapped signal establish the presentation order.
            image.sourceOverride = "image://previewtest/main?licasa_stage=full"
            image.sourceOverrideSize = Qt.size(640, 480)
            image.animatedSource = ""
            for (let childIndex = 0; childIndex < mainWindow.data.length; ++childIndex) {
                const child = mainWindow.data[childIndex]
                if (!child.contentItem)
                    continue
                for (let itemIndex = 0; itemIndex < child.contentItem.children.length; ++itemIndex) {
                    const item = child.contentItem.children[itemIndex]
                    if (item.animatedSource !== undefined)
                        item.animatedSource = ""
                }
            }
            return mainWindow
        }

        function test_sourceResetDiscardsPendingNumericEntry_data() {
            return [{ tag: "neutral", exposure: 0 }, { tag: "edited", exposure: .4 }]
        }

        function test_sourceResetDiscardsPendingNumericEntry(data) {
            const main = makeMain()
            main.fullScreenMode = true
            main.show()
            main.requestActivate()
            tryCompare(main, "active", true)
            main.editPanelOpen = true
            main.editorExposure = data.exposure
            const row = findChild(main, "editorExposureControl")
            const input = findChild(row, "adjustmentValueInput")
            verify(input)
            input.forceActiveFocus()
            compare(main.activeFocusItem, input)
            input.text = "25"
            input.dirty = true
            main.clearEditorSessionState(true)
            compare(main.editorExposure, 0)
            compare(input.text, "0")
            compare(input.dirty, false)
            input.finishEditing(true)
            compare(main.editorExposure, 0)
            main.undoEditorChange()
            compare(main.editorExposure, 0)
        }

        // Exercise the production Main and shared-service signals without writing photos.
        function makeSaveWindows() {
            const first = makeMain()
            const second = makeMain()
            mainWindow = first
            otherMainWindow = second
            second.imageSaveService = first.imageSaveService
            const windows = [first, second]
            for (let index = 0; index < windows.length; ++index) {
                const main = windows[index]
                main.viewerPreferences.fullResolutionRenderingEnabled = false
                const image = findChild(main, "imageViewport")
                image.sourceOverride = "image://previewtest/save-window-" + index + "?licasa_stage=full"
                main.loadImageUrl("file:///shared.png", true)
            }
            tryVerify(function() { return testPreviewRequests.requests().length === 2 }, 5000)
            for (const request of testPreviewRequests.requests())
                verify(testPreviewRequests.release(request.id, false))
            for (const main of windows)
                tryCompare(findChild(main, "imageViewport"), "imageStatus", Image.Ready, 5000)
            first.beginEditorInteraction()
            first.editorExposure = 0.1
            first.finishEditorInteraction()
            second.beginEditorInteraction()
            second.editorExposure = 0.7
            second.finishEditorInteraction()
            second.beginEditorInteraction()
            second.editorContrast = 0.3
            second.finishEditorInteraction()
            second.undoEditorChange()
            return windows
        }

        function verifyPeerEdits(peer, previewKey, revision) {
            compare(String(peer.residentImageUrl), "file:///shared.png")
            compare(String(peer.currentImageUrl), "file:///shared.png")
            compare(peer.editorExposure, 0.7)
            compare(peer.editorContrast, 0)
            compare(peer.editorHasAdjustments, true)
            compare(peer.imageRevision, revision)
            compare(findChild(peer, "imageViewport").activeRequestKey, previewKey)
            compare(peer.editorSaveStatus, "")
            compare(peer.pendingSaveKind, "")
            const notices = findChild(peer, "notificationHistoryButton").parent
            compare(notices.notifications.filter(function(entry) {
                return entry.operationKey === "save"
            }).length, 0)
            peer.redoEditorChange()
            compare(peer.editorContrast, 0.3)
            peer.undoEditorChange()
            peer.undoEditorChange()
            compare(peer.editorExposure, 0)
            peer.redoEditorChange()
            compare(peer.editorExposure, 0.7)
        }

        function test_multiwindowSavePreservesPeerEdits_data() {
            return [ { tag: "overwrite", destination: "file:///shared.png" },
                     { tag: "save-as", destination: "file:///saved-copy.png" } ]
        }

        function test_overwriteShortcutWaitsForConsent() {
            const windows = makeSaveWindows()
            const main = windows[0]
            const service = main.imageSaveService
            const editor = findChild(main, "editorMenuButton").parent
            const shortcut = findChild(main, "overwriteSaveShortcut")
            shortcut.activated()
            compare(main.editPanelOpen, true)
            compare(editor.overwriteConfirmationOpen, true)
            compare(service.pending, null)
            findChild(main, "editorOverwriteNoButton").clicked()
            compare(service.pending, null)
            shortcut.activated()
            findChild(main, "editorOverwriteYesButton").clicked()
            verify(service.pending)
            compare(String(service.pending.destination), String(main.residentImageUrl))
            compare(service.pending.owner, main)
            service.complete("")
        }

        function test_exportAndLateSaveResultsPreserveNewerEdits() {
            const windows = makeSaveWindows()
            const main = windows[0]
            const peer = windows[1]
            const peerExposure = peer.editorExposure
            main.editorExposure = .2
            main.saveEditedImage("file:///copy.png")
            main.editorExposure = .8
            main.imageSaveService.complete("")
            compare(main.editorExposure, .8)
            compare(String(main.residentImageUrl), "file:///shared.png")
            main.pendingCopyIntent = { source: String(main.residentImageUrl), revision: main.imageRevision,
                edits: main.editorEditValues(), options: { format: "webp", scale: .5, copyOnly: true }, kind: "export" }
            main.saveEditedImage("file:///export.webp")
            compare(main.imageSaveService.pending.options.format, "webp")
            main.imageSaveService.complete("")
            compare(String(main.residentImageUrl), "file:///shared.png")
            compare(main.editorExposure, .8)
            compare(peer.editorExposure, peerExposure)
        }

        function test_copyIgnoresCustomExportOptions() {
            const windows = makeSaveWindows()
            const main = windows[0]
            main.editorExportFormat = "webp"
            main.editorExportScale = .5
            main.saveEditedImage("file:///copy.png")
            compare(main.imageSaveService.pending.options.format, "original")
            compare(main.imageSaveService.pending.options.scale, 1)
            main.imageSaveService.complete("")
        }

        function test_saveCopyDefaultsKeepFormatAndFullResolution() {
            const windows = makeSaveWindows()
            const main = windows[0]
            compare(main.editorExportFormat, "original")
            compare(main.editorExportScale, 1)
            compare(String(main.suggestedSaveAsUrl()), "file:///shared-edited.png")
            main.saveEditedImage("file:///shared-edited.png")
            const request = main.imageSaveService.pending
            compare(String(request.source), "file:///shared.png")
            compare(String(request.destination), "file:///shared-edited.png")
            compare(request.options.format, "original")
            compare(request.options.scale, 1)
            main.imageSaveService.complete("")
        }

        function test_multiwindowSavePreservesPeerEdits(data) {
            const windows = makeSaveWindows()
            const owner = windows[0]
            const peer = windows[1]
            const key = findChild(peer, "imageViewport").activeRequestKey
            const revision = peer.imageRevision
            if (data.tag === "overwrite")
                owner.saveCurrentImage()
            else
                owner.saveEditedImage(data.destination)
            compare(owner.pendingSaveKind, "image")
            compare(owner.imageSaveService.pending.edits.exposure, 0.1)
            owner.imageSaveService.complete("")
            compare(owner.editorHasAdjustments, false)
            compare(String(owner.residentImageUrl), data.destination)
            compare(owner.pendingSaveKind, "")
            verify(owner.editorSaveStatus.indexOf("Saved ") === 0)
            verifyPeerEdits(peer, key, revision)
        }

        function test_multiwindowSaveFailurePreservesBothEditors_data() {
            return [ { tag: "synchronous", synchronous: true },
                     { tag: "asynchronous", synchronous: false } ]
        }

        function test_multiwindowSaveFailurePreservesBothEditors(data) {
            const windows = makeSaveWindows()
            const owner = windows[0]
            const peer = windows[1]
            const key = findChild(peer, "imageViewport").activeRequestKey
            const revision = peer.imageRevision
            owner.imageSaveService.rejectImmediately = data.synchronous
            owner.saveCurrentImage()
            if (!data.synchronous)
                owner.imageSaveService.complete("Save refused")
            compare(owner.editorSaveStatus, "Save refused")
            compare(owner.pendingSaveKind, "")
            compare(owner.editorExposure, 0.1)
            compare(owner.editorHasAdjustments, true)
            owner.undoEditorChange()
            compare(owner.editorExposure, 0)
            verifyPeerEdits(peer, key, revision)
        }

        function test_multiwindowBusyRejectionDoesNotStealSave() {
            const windows = makeSaveWindows()
            const owner = windows[0]
            const peer = windows[1]
            owner.saveCurrentImage()
            peer.saveCurrentImage()
            compare(peer.pendingSaveKind, "")
            // A direct competing caller is rejected synchronously by the shared service.
            compare(owner.imageSaveService.save("file:///shared.png", "file:///other.png",
                                                {}, {}, peer), false)
            compare(owner.pendingSaveKind, "image")
            compare(owner.editorSaveStatus, "")
            compare(owner.imageSaveService.pending.owner, owner)
            owner.imageSaveService.complete("")
            compare(owner.editorHasAdjustments, false)
            compare(peer.editorExposure, 0.7)
            compare(peer.editorHasAdjustments, true)
        }

        function test_saveCompletionAfterNavigationDoesNotResetNewSession() {
            const windows = makeSaveWindows()
            const owner = windows[0]
            owner.saveCurrentImage()
            owner.loadImageUrl("file:///another.png", true)
            owner.loadImageUrl("file:///shared.png", true)
            owner.editorExposure = 0.9
            owner.imageSaveService.complete("")
            compare(owner.editorExposure, 0.9)
            compare(owner.editorHasAdjustments, true)
            compare(owner.editorSaveStatus, "")
            compare(String(owner.residentImageUrl), "file:///shared.png")
            compare(owner.pendingSaveKind, "")
        }

        function test_unownedOrStaleResultCannotResetEditor() {
            const windows = makeSaveWindows()
            const owner = windows[0]
            const peer = windows[1]
            const service = owner.imageSaveService
            // A matching window alone is insufficient without a pending image save.
            service.saveCompleted("file:///shared.png", "file:///stale.png", peer)
            compare(peer.editorExposure, 0.7)
            compare(String(peer.residentImageUrl), "file:///shared.png")
            owner.saveCurrentImage()
            service.saveCompleted("file:///shared.png", "file:///foreign.png", null)
            service.frameSaveCompleted("file:///shared.png", "file:///frame.png", owner)
            service.frameSaveFailed("file:///shared.png", "Foreign frame failure", peer)
            compare(owner.pendingSaveKind, "image")
            compare(owner.editorHasAdjustments, true)
            service.complete("")
            compare(owner.editorHasAdjustments, false)
            compare(peer.editorHasAdjustments, true)
        }

        function test_frameSaveNotificationsBelongToInitiator_data() {
            return [ { tag: "success", error: "" }, { tag: "failure", error: "Frame refused" } ]
        }

        function test_frameSaveNotificationsBelongToInitiator(data) {
            const windows = makeSaveWindows()
            const owner = windows[0]
            const peer = windows[1]
            owner.pendingSaveKind = "frame"
            const service = owner.imageSaveService
            if (data.error)
                service.frameSaveFailed("file:///shared.png", data.error, owner)
            else
                service.frameSaveCompleted("file:///shared.png", "file:///frame.png", owner)
            compare(owner.pendingSaveKind, "")
            compare(owner.editorExposure, 0.1)
            compare(peer.editorExposure, 0.7)
            const notices = findChild(peer, "notificationHistoryButton").parent
            compare(notices.notifications.filter(function(entry) {
                return entry.operationKey === "save"
            }).length, 0)
            const ownNotices = findChild(owner, "notificationHistoryButton").parent
            const results = ownNotices.notifications.filter(function(entry) {
                return entry.operationKey === "save"
            })
            compare(results.length, 1)
            compare(results[0].title, data.error ? "Frame could not be saved" : "Frame saved")
        }

        function makePanelWindow() {
            const main = makeMain()
            main.smoothMenuMotionEnabled = false
            main.currentImageUrl = "file:///panel.png"
            main.residentImageUrl = "file:///panel.png"
            return main
        }

        function test_settingsRejectEditorOpen() {
            const main = makePanelWindow()
            const settings = findChild(main, "settingsMenuButton").parent
            settings.panelOpen = true
            main.setEditPanelOpen(true)
            compare(main.editPanelOpen, false)
            compare(settings.panelOpen, true)
        }

        function test_settingsLayeringThroughDismissal_data() {
            return [{ tag: "animated", animated: true, reopen: false },
                    { tag: "interrupted-close", animated: true, reopen: true },
                    { tag: "reduced-motion", animated: false, reopen: false }]
        }

        function test_settingsLayeringThroughDismissal(data) {
            const main = makePanelWindow()
            const settings = findChild(main, "settingsMenuButton").parent
            const notices = findChild(main, "notificationCenter")
            verify(notices.z > settings.z)
            settings.panelOpen = true
            verify(settings.panelVisible)
            verify(notices.z < settings.z)
            main.smoothMenuMotionEnabled = data.animated
            settings.panelOpen = false
            if (data.animated) {
                verify(settings.panelVisible)
                verify(notices.z < settings.z,
                       "Notifications must stay behind the closing Settings panel")
                if (data.reopen) {
                    settings.panelOpen = true
                    verify(notices.z < settings.z)
                    settings.panelOpen = false
                }
            }
            tryCompare(settings, "panelVisible", false, 1000)
            verify(notices.z > settings.z,
                   "Notifications return to the foreground only after Settings is hidden")
        }

        function test_editorRejectsSettingsOpen() {
            const main = makePanelWindow()
            const settingsButton = findChild(main, "settingsMenuButton")
            main.setEditPanelOpen(true)
            settingsButton.clicked()
            compare(main.editPanelOpen, true)
            compare(settingsButton.parent.panelOpen, false)
        }

        function test_editorSuppressesInfoWithoutChangingPreference_data() {
            return [{ tag: "enabled", enabled: true }, { tag: "disabled", enabled: false }]
        }

        function test_editorSuppressesInfoWithoutChangingPreference(data) {
            const main = makePanelWindow()
            main.infoOverlayEnabled = data.enabled
            const info = findChild(main, "infoOverlays")
            const details = findChild(info, "imageInfoDetails")
            main.setEditPanelOpen(true)
            compare(info.editorPanelVisible, true)
            compare(details.hideProgress, 1)
            compare(details.visible, false)
            compare(main.infoOverlayEnabled, data.enabled)
            main.smoothMenuMotionEnabled = true
            main.setEditPanelOpen(false)
            // Remain tucked away until the closing editor has left the screen.
            compare(info.editorPanelVisible, true)
            compare(details.hideProgress, 1)
            tryCompare(info, "editorPanelVisible", false, 1000)
            tryCompare(details, "hideProgress", 0, 1000)
            compare(main.infoOverlayEnabled, data.enabled)
            compare(info.infoOverlayEnabled, data.enabled)
        }

        function test_oppositePanelShortcutsAreBlocked_data() {
            return [{ tag: "edit", shortcut: "editorShortcut", settings: true },
                    { tag: "settings", shortcut: "settingsShortcut", settings: false },
                    { tag: "preferences", shortcut: "settingsPreferencesShortcut", settings: false }]
        }

        function test_oppositePanelShortcutsAreBlocked(data) {
            const main = makePanelWindow()
            const settings = findChild(main, "settingsMenuButton").parent
            if (data.settings)
                settings.panelOpen = true
            else
                main.setEditPanelOpen(true)
            const shortcut = findChild(main, data.shortcut)
            verify(shortcut)
            compare(shortcut.enabled, false)
            // Even a stale/queued activation cannot replace the active drawer.
            shortcut.activated()
            compare(settings.panelOpen, data.settings)
            compare(main.editPanelOpen, !data.settings)
        }

        function test_closingPanelStillBlocksOppositeOpen_data() {
            return [{ tag: "settings-closing", settings: true },
                    { tag: "editor-closing", settings: false }]
        }

        function test_closingPanelStillBlocksOppositeOpen(data) {
            const main = makePanelWindow()
            const settings = findChild(main, "settingsMenuButton").parent
            const editor = findChild(main, "editorMenuButton").parent
            if (data.settings)
                settings.panelOpen = true
            else
                main.setEditPanelOpen(true)
            main.smoothMenuMotionEnabled = true
            if (data.settings)
                settings.panelOpen = false
            else
                main.setEditPanelOpen(false)
            compare(data.settings ? settings.panelVisible : editor.panelVisible, true)
            if (data.settings)
                main.setEditPanelOpen(true)
            else
                settings.panelOpen = true
            compare(settings.panelOpen, false)
            compare(main.editPanelOpen, false)
            const shortcut = findChild(main, data.settings ? "editorShortcut" : "settingsShortcut")
            compare(shortcut.enabled, false)
        }

        function test_mainDefersNeighborsUntilSelectedFrame() {
            const main = makeMain()
            const manager = main.windowManager
            main.loadImageUrl("file:///selected.png", true)
            main.formatSupport.directoryIndexReady()
            wait(0)
            compare(manager.nearbyRequests.length, 0)
            compare(manager.prepared.length, 1)
            main.frameSwapped()
            wait(0)
            compare(manager.nearbyRequests.length, 0)

            release(requests(1)[0])
            tryCompare(main, "nearbyPreviewsReadyForFrame", true, 5000)
            wait(0)
            compare(main.visible, false)
            compare(manager.nearbyRequests.length, 0)
            compare(manager.prepared[0], "file:///previous-neighbor.png")
            main.displayedFileName = "stale.png"
            main.frameSwapped()
            wait(0)
            compare(manager.nearbyRequests.length, 0)
            main.displayedFileName = main.currentFileName
            main.frameSwapped()
            tryCompare(main, "nearbyPreviewsAwaitingFrame", false)
            tryVerify(function() { return manager.nearbyRequests.length === 1 })
            compare(manager.prepared[0], "file:///prepared-neighbor.png")
        }

        function test_mainLoadAndClearRetirePendingNeighborRefresh() {
            const main = makeMain()
            const manager = main.windowManager
            main.loadImageUrl("file:///selected.png", true)
            release(requests(1)[0])
            tryCompare(main, "nearbyPreviewsReadyForFrame", true, 5000)
            main.formatSupport.directoryIndexReady()
            wait(0)
            compare(manager.nearbyRequests.length, 0)
            main.loadImageUrl("file:///replacement.png", true)
            compare(main.nearbyPreviewsReadyForFrame, false)
            main.frameSwapped()
            wait(0)
            compare(manager.nearbyRequests.length, 0)
            findChild(main, "imageViewport").sourceOverride =
                "image://previewtest/replacement?licasa_stage=full"
            tryVerify(function() {
                return testPreviewRequests.requests().some(function(request) {
                    return request.source.indexOf("replacement") >= 0
                        && !request.cancelled && !request.finished
                })
            }, 5000)
            release(testPreviewRequests.requests().filter(function(request) {
                return request.source.indexOf("replacement") >= 0
                    && !request.cancelled && !request.finished
            })[0])
            tryCompare(main, "nearbyPreviewsReadyForFrame", true, 5000)
            main.formatSupport.directoryIndexReady()
            wait(0)
            compare(main.nearbyPreviewsRefreshPending, true)
            compare(manager.nearbyRequests.length, 0)
            main.clearImageState()
            compare(main.nearbyPreviewsAwaitingFrame, false)
            compare(main.nearbyPreviewsRefreshPending, false)
            main.frameSwapped()
            wait(0)
            compare(manager.nearbyRequests.length, 0)
        }
        function make(sourceFile) {
            const source = sourceFile || "file:///photo.png"
            viewport = createTemporaryObject(viewportComponent, visualRoot, {
                sourceFile: source,
                providerSource: "image://previewtest/" + FileUrls.fileName(source)
            })
            verify(viewport)
            return viewport
        }
        function requests(count) {
            tryVerify(function() { return testPreviewRequests.requests().length >= count }, 5000)
            return testPreviewRequests.requests()
        }
        function stage(request) { return FileUrls.queryValue(request.source, "licasa_stage") }
        function release(request, fail) {
            verify(testPreviewRequests.release(request.id, fail === true))
        }
        function readyPreview() {
            const preview = requests(1)[0]
            compare(stage(preview), "preview")
            release(preview)
            tryCompare(viewport, "imageStatus", Image.Ready, 5000)
            return requests(2)[1]
        }

        function test_formatPolicy_data() {
            return [
                { tag: "png", url: "file:///UPPER.PNG", previewFirst: true },
                { tag: "ppm", url: "file:///photo.ppm", previewFirst: true },
                { tag: "pnm", url: "file:///photo.pnm", previewFirst: true },
                { tag: "pgm", url: "file:///photo.pgm", previewFirst: true },
                { tag: "pbm", url: "file:///photo.pbm", previewFirst: true },
                { tag: "raw", url: "file:///photo.dng", previewFirst: false },
                { tag: "jpeg", url: "file:///photo.jpg", previewFirst: false }
            ]
        }
        function test_formatPolicy(data) {
            make(data.url)
            compare(viewport.previewFirstPreloadEnabled, data.previewFirst)
            const first = requests(data.previewFirst ? 1 : 2)
            if (data.previewFirst) {
                compare(first.length, 1)
                compare(stage(first[0]), "preview")
                compare(FileUrls.queryValue(first[0].source, "licasa_parallel"), "")
                compare(viewport.speculativeFullKey, "")
                compare(String(findChild(viewport, "staticImageC").source), "")
            } else {
                compare(first.length, 2)
                verify(first.some(function(request) {
                    return FileUrls.queryValue(request.source, "licasa_parallel") === "preview"
                }))
                verify(first.some(function(request) {
                    return FileUrls.queryValue(request.source, "licasa_parallel") === "full"
                }))
            }
        }

        function test_previewStartsFullBeforeReadySignalAndReusesC() {
            make()
            const c = findChild(viewport, "staticImageC")
            const readyStates = []
            viewport.imageReady.connect(function() {
                readyStates.push({ slot: viewport.activeImageSlot, full: String(c.source) })
            })
            const first = requests(1)
            compare(first.length, 1)
            compare(String(c.source), "")
            const full = readyPreview()
            compare(readyStates.length, 1)
            verify(readyStates[0].slot !== 2)
            compare(readyStates[0].full, viewport.fullRequestKey())
            compare(stage(full), "full")
            compare(FileUrls.queryValue(full.source, "licasa_parallel"), "")
            const key = viewport.fullRequestKey()
            release(full)
            tryCompare(c, "status", Image.Ready, 5000)
            verify(viewport.activeImageSlot !== 2)
            viewport.previewPhase = false
            compare(viewport.fullRequestKey(), key)
            tryCompare(viewport, "activeImageSlot", 2, 5000)
            compare(String(c.source), key)
            compare(testPreviewRequests.requests().length, 2)
            compare(viewport.activeParallelPairId, "")
        }

        function test_navigationCancelsOldPreloadAndIgnoresItsCompletion() {
            make()
            const oldFull = readyPreview()
            viewport.providerSource = "image://previewtest/next.png"
            const next = requests(3)[2]
            tryVerify(function() { return testPreviewRequests.requests()[1].cancelled }, 5000)
            verify(viewport.activeRequestKey.indexOf("photo.png") >= 0)
            release(next)
            tryVerify(function() { return viewport.activeRequestKey.indexOf("next.png") >= 0 }, 5000)
            const newFull = requests(4)[3]
            verify(newFull.source.indexOf("next.png") >= 0)
            verify(viewport.activeImageSlot !== 2)
            compare(FileUrls.queryValue(newFull.source, "licasa_parallel"), "")
            // Releasing an already cancelled response cannot activate its old C raster.
            testPreviewRequests.release(oldFull.id, false)
            verify(viewport.activeRequestKey.indexOf("next.png") >= 0)
            release(newFull)
            viewport.previewPhase = false
            tryCompare(viewport, "activeImageSlot", 2, 5000)
            verify(viewport.activeRequestKey.indexOf("next.png") >= 0)
        }

        function test_clearCancelsFullWithoutReopening() {
            make()
            readyPreview()
            viewport.releaseStaticImages()
            tryVerify(function() { return testPreviewRequests.requests()[1].cancelled }, 5000)
            compare(viewport.activeImageSlot, -1)
            compare(viewport.pendingImageSlot, -1)
            compare(viewport.speculativeFullKey, "")
            compare(String(findChild(viewport, "staticImageC").source), "")
            compare(viewport.imageStatus, Image.Loading)
            compare(testPreviewRequests.requests().length, 2)
        }

        function test_enablingAfterBrowsePreloadsWithoutRepeatingPreview() {
            make()
            viewport.previewFirstPreloadEnabled = false
            viewport.parallelFullResolutionEnabled = false
            const preview = requests(1)[0]
            release(preview)
            tryCompare(viewport, "imageStatus", Image.Ready, 5000)
            compare(testPreviewRequests.requests().length, 1)
            const key = viewport.activeRequestKey
            viewport.previewFirstPreloadEnabled = true
            const full = requests(2)[1]
            compare(stage(full), "full")
            compare(viewport.activeRequestKey, key)
            viewport.previewFirstPreloadEnabled = false
            tryVerify(function() { return testPreviewRequests.requests()[1].cancelled }, 5000)
            compare(testPreviewRequests.requests().length, 2)
            viewport.previewFirstPreloadEnabled = true
            const resumed = requests(3)[2]
            compare(stage(resumed), "full")
            compare(viewport.activeRequestKey, key)
            release(resumed)
            viewport.previewPhase = false
            tryCompare(viewport, "activeImageSlot", 2, 5000)
            compare(testPreviewRequests.requests().length, 3)
        }

        function test_modeChangeWaitsForSameTurnSourcePublication() {
            make("file:///photo.jpg")
            viewport.parallelFullResolutionEnabled = false
            release(requests(1)[0])
            tryCompare(viewport, "imageStatus", Image.Ready, 5000)
            compare(testPreviewRequests.requests().length, 1)
            viewport.sourceFile = "file:///next.png"
            // Main publishes selected-file metadata before its new provider key.
            // That change must not synchronously preload the previous picture.
            compare(String(findChild(viewport, "staticImageC").source), "")
            viewport.providerSource = "image://previewtest/next.png"
            const next = requests(2)[1]
            verify(next.source.indexOf("next.png") >= 0)
            compare(stage(next), "preview")
            release(next)
            const full = requests(3)[2]
            verify(full.source.indexOf("next.png") >= 0)
            compare(stage(full), "full")
            release(full)
            viewport.previewPhase = false
            tryCompare(viewport, "activeImageSlot", 2, 5000)
        }

        function test_navigationPreviewIsUnpairedWithoutCurrentEagerFull() {
            make("file:///photo.dng")
            const pair = requests(2)
            const preview = pair.filter(function(request) { return stage(request) === "preview" })[0]
            release(preview)
            tryCompare(viewport, "imageStatus", Image.Ready, 5000)
            viewport.providerSource = "image://previewtest/next.dng"
            const next = requests(3)[2]
            compare(stage(next), "preview")
            compare(FileUrls.queryValue(next.source, "licasa_parallel"), "")
            compare(FileUrls.queryValue(next.source, "licasa_pair"), "")
            release(next)
            tryVerify(function() { return viewport.activeRequestKey.indexOf("next.dng") >= 0 }, 5000)
            compare(testPreviewRequests.requests().length, 3)
        }

        function test_sourceOverrideSizeChangeReloadsSameUrl() {
            make()
            viewport.providerSource = ""
            viewport.sourceOverride = "image://previewtest/override?licasa_stage=preview"
            viewport.sourceOverrideSize = Qt.size(160, 120)
            const initial = requests(1)[0]
            compare(initial.width, 160)
            compare(initial.height, 120)
            release(initial)
            tryCompare(viewport, "imageStatus", Image.Ready, 5000)
            const key = viewport.activeRequestKey
            viewport.sourceOverrideSize = Qt.size(320, 240)
            const replacement = requests(2)[1]
            compare(replacement.source, initial.source)
            compare(replacement.width, 320)
            compare(replacement.height, 240)
            release(replacement)
            tryVerify(function() { return viewport.pendingImageSlot === -1 }, 5000)
            compare(viewport.activeRequestKey, key)
            compare(testPreviewRequests.requests().length, 2)
        }

        function test_fullErrorFallsBackWhileKeepingReadyPreview() {
            make()
            const full = readyPreview()
            const previewSlot = viewport.activeImageSlot
            release(full, true)
            tryCompare(findChild(viewport, "staticImageC"), "status", Image.Error, 5000)
            viewport.previewPhase = false
            const fallback = requests(3)[2]
            compare(stage(fallback), "full")
            compare(viewport.activeImageSlot, previewSlot)
            compare(viewport.imageStatus, Image.Ready)
            release(fallback)
            tryVerify(function() { return viewport.pendingImageSlot === -1 }, 5000)
            verify(viewport.activeImageSlot !== previewSlot)
            compare(viewport.imageStatus, Image.Ready)
        }

        function test_promotedFullFailureKeepsPresentedPreview() {
            make()
            const full = readyPreview()
            const previewKey = viewport.activeRequestKey
            const previewSlot = viewport.activeImageSlot
            let failures = 0
            viewport.imageRequestFailed.connect(function() { failures += 1 })

            viewport.previewPhase = false
            tryCompare(viewport, "pendingImageSlot", 2, 5000)
            release(full, true)
            tryCompare(viewport, "pendingImageSlot", -1, 5000)
            compare(failures, 1)
            compare(viewport.imageStatus, Image.Ready)
            compare(viewport.activeRequestKey, previewKey)
            compare(viewport.activeImageSlot, previewSlot)
            compare(testPreviewRequests.requests().length, 2)
        }

        function test_failedReplacementKeepsPresentedPreviewAndAllowsNextOpen() {
            make()
            viewport.previewFirstPreloadEnabled = false
            viewport.parallelFullResolutionEnabled = false
            release(requests(1)[0])
            tryCompare(viewport, "imageStatus", Image.Ready, 5000)
            const previewKey = viewport.activeRequestKey
            let failures = 0
            viewport.imageRequestFailed.connect(function() { failures += 1 })

            viewport.providerSource = "image://previewtest/failed.png"
            const replacement = requests(2)[1]
            release(replacement, true)
            tryCompare(viewport, "pendingImageSlot", -1, 5000)
            compare(failures, 1)
            compare(viewport.imageStatus, Image.Ready)
            compare(viewport.activeRequestKey, previewKey)

            viewport.providerSource = "image://previewtest/retry.png"
            release(requests(3)[2])
            tryVerify(function() {
                return viewport.activeRequestKey.indexOf("retry.png") >= 0
            }, 5000)
            compare(viewport.imageStatus, Image.Ready)
            compare(failures, 1)
        }

        function test_newPreviewCanReuseFadingFullSlot() {
            make()
            release(readyPreview())
            viewport.previewPhase = false
            tryCompare(viewport, "activeImageSlot", 2, 5000)
            viewport.previewPhase = true
            viewport.providerSource = "image://previewtest/another.png"
            const preview = requests(3)[2]
            release(preview)
            const full = requests(4)[3]
            verify(viewport.activeImageSlot !== 2)
            verify(viewport.fadingImageSlot !== 2)
            verify(String(findChild(viewport, "staticImageC").source).indexOf("another.png") >= 0)
            release(full)
            viewport.previewPhase = false
            tryCompare(viewport, "activeImageSlot", 2, 5000)
            verify(viewport.activeRequestKey.indexOf("another.png") >= 0)
        }

        function test_presentedPairComesFromActiveRasterDespiteNewGeneration() {
            make("file:///photo.dng")
            const pair = requests(2)
            const preview = pair.filter(function(request) { return stage(request) === "preview" })[0]
            const presented = FileUrls.queryValue(preview.source, "licasa_pair")
            release(preview)
            tryCompare(viewport, "imageStatus", Image.Ready, 5000)
            compare(viewport.activeParallelPairId, presented)
            viewport.parallelPairGeneration += 7
            verify(viewport.parallelPairId !== presented)
            compare(viewport.activeParallelPairId, presented)
            viewport.releaseStaticImages()
            compare(viewport.activeParallelPairId, "")
        }
    }
}
