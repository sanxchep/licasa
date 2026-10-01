import QtQuick
import QtTest
import "../qml/components"
import "../qml"

Item {
    id: visualRoot
    width: 640
    height: 480

    TestCase {
        id: testCase
        name: "Controls"
        when: windowShown

    Component {
        id: signalSpyComponent
        SignalSpy {}
    }

    Component {
        id: toggleComponent
        ToggleRow {
            width: 320
            title: "Toggle"
            subtitle: "The complete row is interactive"
            checked: false
        }
    }

    Component {
        id: valueRowComponent
        ValueRow {
            width: 320
            title: "Opacity"
            value: 0.5
            step: 0.1
        }
    }

    Component {
        id: settingsMenuComponent
        FullscreenSideMenu {
            width: 640
            height: 480
            fullscreen: true
            maximumImageMemoryMiB: 8192
            minimumImageMemoryMiB: 384
            maximumImageMemoryLimitMiB: 32768
        }
    }

    Component {
        id: adjustRowComponent
        AdjustRow {
            width: 300
            label: "Exposure"
            value: 0.0
        }
    }

    Component {
        id: actionChipComponent
        ActionChip { text: "Apply" }
    }

    Component {
        id: headerButtonComponent
        HeaderButton {
            width: 90
            height: 34
            text: "Reset"
        }
    }

    Component {
        id: temporalControlsComponent

        TemporalControls {
            width: 500
            height: 58
            hasTimeline: true
            playing: true
            canScrub: true
            currentFrame: 2
            frameCount: 5
            position: 0.5
        }
    }

    Component {
        id: overlayComponent
        OverlayControls {
            width: 500
            height: 360
            appVisible: true
            fullScreenMode: true
            hasImage: true
            windowedCloseButtonX: 440
            windowedCloseButtonY: 12
        }
    }

    Component {
        id: notificationCenterComponent

        NotificationCenter {
            width: 500
            height: 360
            appVisible: true
            fullScreenMode: true
        }
    }

    Component {
        id: imageViewportComponent
        ImageViewport {
            width: 320
            height: 240
            fullScreenMode: false
            fullscreenBackdropOpacity: 0.7
            transparencyCheckerboardEnabled: true
            transparencyCheckerboardOpacity: 0.6
            windowedTransparencyCheckerboardEnabled: true
            windowedTransparencyOpacity: 0.55
            hasImage: true
            imageX: 0
            imageY: 0
            imageWidth: 1240
            imageHeight: 1248
            currentScale: 0.2
            providerSource: Qt.resolvedUrl("../assets/licasa.png").toString()
            previewPhase: true
            rawFastPhase: false
            rawPreviewFirst: false
            previewSourceWidth: 320
            previewSourceHeight: 240
            fullScreenWheelSensitivity: 1.0015
            windowedWheelSensitivity: 1.0009
            maxWheelDeltaPerEvent: 120
            interactionBlocked: false
            canOpen: function() { return true }
        }
    }

    Component {
        id: animatedViewportComponent

        ImageViewport {
            width: 640
            height: 480
            fullScreenMode: true
            fullscreenBackdropOpacity: 0.7
            transparencyCheckerboardEnabled: false
            transparencyCheckerboardOpacity: 0.7
            windowedTransparencyCheckerboardEnabled: false
            windowedTransparencyOpacity: 0.7
            hasImage: imageStatus === Image.Ready
            imageX: 160
            imageY: 120
            imageWidth: 320
            imageHeight: 240
            currentScale: 1.0
            providerSource: Qt.resolvedUrl(
                "test-assets/animated/gif/standard-transparent-320x240.gif"
            ).toString()
            previewPhase: false
            rawFastPhase: false
            rawPreviewFirst: false
            previewSourceWidth: 640
            previewSourceHeight: 480
            animatedSource: Qt.resolvedUrl(
                "test-assets/animated/gif/standard-transparent-320x240.gif"
            )
            animatedPlaybackActive: true
            Component.onCompleted: {
                animationController = testAnimationService.createController(this)
                animationController.source = animatedSource
                animationController.active = true
            }
            animationSpeed: 1.0
            smoothScalingEnabled: true
            mipmapScalingEnabled: true
            pixelAlignedRenderingEnabled: true
            devicePixelRatio: 1.0
            fullScreenWheelSensitivity: 1.0015
            windowedWheelSensitivity: 1.0009
            maxWheelDeltaPerEvent: 120
            interactionBlocked: false
            canOpen: function() { return true }
        }
    }

    Component {
        id: scrollBarCompositionComponent

        Item {
            width: 180
            height: 220

            property alias flickable: testFlickable
            property alias scrollBar: testScrollBar

            Flickable {
                id: testFlickable
                width: 150
                height: parent.height
                contentWidth: width
                contentHeight: 1000
                boundsBehavior: Flickable.StopAtBounds

                Rectangle {
                    width: testFlickable.width
                    height: testFlickable.contentHeight
                    color: "#202020"
                }
            }

            InteractiveScrollBar {
                id: testScrollBar
                anchors.right: parent.right
                height: parent.height
                flickable: testFlickable
            }
        }
    }

    Component {
        id: cropOverlayComponent
        CropOverlay {
            width: 640
            height: 480
            active: true
            imageX: 100
            imageY: 100
            imageWidth: 400
            imageHeight: 200
            cropX: 0.25
            cropY: 0.25
            cropWidth: 0.5
            cropHeight: 0.5
        }
    }

    Component {
        id: editorCropCompositionComponent

        Item {
            width: 1000
            height: 700

            property alias cropOverlay: cropOverlay
            property alias editorPanel: editorPanel

            CropOverlay {
                id: cropOverlay
                anchors.fill: parent
                z: 1050
                active: true
                imageX: 480
                imageY: 120
                imageWidth: 400
                imageHeight: 300
                cropX: 0.1
                cropY: 0.1
                cropWidth: 0.8
                cropHeight: 0.8
            }

            EditorSidePanel {
                id: editorPanel
                anchors.fill: parent
                z: 1100
                panelOpen: true
                hasImage: true
                cropMode: true
                cropOutputWidth: 320
                cropOutputHeight: 240
            }
        }
    }

    Component {
        id: editorSessionComponent
        EditorSession {}
    }

    function createSpy(target, signalName) {
        const spy = createTemporaryObject(signalSpyComponent, testCase, {
            "target": target,
            "signalName": signalName
        })
        verify(spy)
        verify(spy.valid)
        return spy
    }

    function createControl(component) {
        const control = createTemporaryObject(component, visualRoot, { "x": 12, "y": 12 })
        verify(control)
        waitForRendering(control)
        return control
    }

    function test_toggleRespondsAcrossTheWholeRow() {
        const control = createControl(toggleComponent)
        const spy = createSpy(control, "toggled")
        verify(spy)

        mouseClick(control, 24, control.height / 2, Qt.LeftButton)
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], true)

        mouseClick(control, control.width - 20, control.height / 2, Qt.LeftButton)
        compare(spy.count, 2)
    }

    function test_settingsRowsRespondToKeyboard() {
        const toggle = createControl(toggleComponent)
        const toggleSpy = createSpy(toggle, "toggled")
        toggle.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(toggleSpy.count, 1)
        compare(toggleSpy.signalArguments[0][0], true)

        const value = createControl(valueRowComponent)
        const valueSpy = createSpy(value, "valueChangedByUser")
        value.forceActiveFocus()
        keyClick(Qt.Key_Right)
        compare(valueSpy.count, 1)
        fuzzyCompare(valueSpy.signalArguments[0][0], 0.6, 0.000001)
        keyClick(Qt.Key_Home)
        compare(valueSpy.signalArguments[1][0], 0)
    }

    function test_timelineKeyboardUsesMediaControls() {
        const timeline = createControl(temporalControlsComponent)
        const stepSpy = createSpy(timeline, "stepRequested")
        const seekSpy = createSpy(timeline, "seekRequested")
        const previous = findChild(timeline, "temporalPreviousButton")
        const scrubber = findChild(timeline, "temporalScrubberMouse").parent
        verify(previous)
        verify(scrubber)

        previous.forceActiveFocus()
        verify(timeline.keyboardFocusWithin)
        keyClick(Qt.Key_Return)
        compare(stepSpy.count, 1)
        compare(stepSpy.signalArguments[0][0], -1)

        scrubber.forceActiveFocus()
        keyClick(Qt.Key_Right)
        compare(seekSpy.count, 1)
        fuzzyCompare(seekSpy.signalArguments[0][0], 0.52, 0.000001)
    }

    function test_cropKeyboardNudgesAndResizesSelection() {
        const crop = createControl(cropOverlayComponent)
        const spy = createSpy(crop, "cropRequested")
        crop.keyboardAdjust(1, 0, false)
        compare(spy.count, 1)
        fuzzyCompare(spy.signalArguments[0][0], 0.28, 0.000001)
        fuzzyCompare(spy.signalArguments[0][2], 0.5, 0.000001)

        crop.keyboardAdjust(1, 0, true)
        compare(spy.count, 2)
        verify(spy.signalArguments[1][2] > 0.5)
    }

    function test_valueRowButtonsAndTrackEmitValues() {
        const control = createControl(valueRowComponent)
        const spy = createSpy(control, "valueChangedByUser")
        verify(spy)

        mouseClick(control, 21, control.height - 17, Qt.LeftButton)
        fuzzyCompare(spy.signalArguments[0][0], 0.4, 0.000001)

        mouseClick(control, control.width - 21, control.height - 17, Qt.LeftButton)
        fuzzyCompare(spy.signalArguments[1][0], 0.6, 0.000001)

        mouseClick(control, control.width / 2, control.height - 17, Qt.LeftButton)
        fuzzyCompare(spy.signalArguments[2][0], 0.5, 0.02)
    }

    function test_valueRowSupportsAdvancedNonUnitRanges() {
        const control = createControl(valueRowComponent)
        control.from = 0.25
        control.to = 2.0
        control.value = 1.0
        control.step = 0.25
        const spy = createSpy(control, "valueChangedByUser")

        mouseClick(control, control.width - 21, control.height - 17, Qt.LeftButton)
        fuzzyCompare(spy.signalArguments[0][0], 1.25, 0.000001)

        mouseClick(control, control.width / 2, control.height - 17, Qt.LeftButton)
        fuzzyCompare(spy.signalArguments[1][0], 1.125, 0.03)
    }

    function test_largeImageSettingUsesMemoryStepsAndGuidance() {
        const menu = createControl(settingsMenuComponent)
        menu.panelOpen = true
        menu.advancedOpen = true
        wait(0)

        const control = findChild(menu, "maximumImageMemoryControl")
        verify(control)
        const spy = createSpy(menu, "maximumImageMemoryMiBRequested")
        verify(spy)

        control.valueChangedByUser(2075)
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], 2048)
        verify(menu.estimatedLargeImageMemoryText().indexOf("GiB") >= 0)

        control.valueChangedByUser(384)
        compare(spy.signalArguments[1][0], 384)
        control.valueChangedByUser(32768)
        compare(spy.signalArguments[2][0], 32768)

        menu.currentImageMegapixels = 240
        menu.currentImageLimitedToPreview = true
        compare(menu.currentImageMegapixelsText(), "240 MP")
    }

    function test_scrollBarHandleDragsTheFlickable() {
        const composition = createControl(scrollBarCompositionComponent)
        compare(composition.flickable.contentY, 0)

        mouseDrag(composition.scrollBar, 8, 20, 0, 120, Qt.LeftButton)

        verify(composition.flickable.contentY > 300)
        verify(composition.flickable.contentY
            <= composition.flickable.contentHeight - composition.flickable.height)
    }

    function test_adjustmentSliderHasAUsableHitTarget() {
        const control = createControl(adjustRowComponent)
        const spy = createSpy(control, "valueRequested")
        const startedSpy = createSpy(control, "interactionStarted")
        const finishedSpy = createSpy(control, "interactionFinished")
        verify(spy)

        mouseClick(control, control.width * 0.75, control.height - 8, Qt.LeftButton)
        compare(spy.count, 1)
        compare(startedSpy.count, 1)
        compare(finishedSpy.count, 1)
        fuzzyCompare(spy.signalArguments[0][0], 0.5, 0.02)
    }

    function test_cropShieldPreservesCropInteraction() {
        const control = createControl(cropOverlayComponent)
        const spy = createSpy(control, "cropRequested")

        compare(control.shieldOpacity, 0.7)
        const topShield = findChild(control, "cropShieldTop")
        verify(topShield)
        fuzzyCompare(topShield.height, 50, 0.01)

        mouseDrag(control, 300, 200, 40, 20, Qt.LeftButton)
        verify(spy.count > 0)
        const values = spy.signalArguments[spy.count - 1]
        fuzzyCompare(values[0], 0.35, 0.02)
        fuzzyCompare(values[1], 0.35, 0.02)
        fuzzyCompare(values[2], 0.5, 0.001)
        fuzzyCompare(values[3], 0.5, 0.001)
    }

    function test_longDescriptionsWrapAndGrowTheirRows() {
        const toggle = createControl(toggleComponent)
        toggle.width = 190
        toggle.subtitle = "This longer explanation must continue on a second line instead of being clipped."
        tryVerify(function() { return toggle.height > 58 })

        const valueRow = createControl(valueRowComponent)
        valueRow.width = 190
        valueRow.subtitle = "This longer slider explanation must wrap cleanly onto another line."
        tryVerify(function() { return valueRow.height > 72 })
    }

    function test_cropCornerHasALargeResizeTarget() {
        const control = createControl(cropOverlayComponent)
        const spy = createSpy(control, "cropRequested")

        mouseDrag(control, 200, 150, -40, -20, Qt.LeftButton)
        verify(spy.count > 0)
        const values = spy.signalArguments[spy.count - 1]
        fuzzyCompare(values[0], 0.15, 0.02)
        fuzzyCompare(values[1], 0.15, 0.02)
        fuzzyCompare(values[2], 0.60, 0.02)
        fuzzyCompare(values[3], 0.60, 0.02)
    }

    function test_cropCompleteEdgeIsDraggable() {
        const control = createControl(cropOverlayComponent)
        const spy = createSpy(control, "cropRequested")

        // Drag the right border away from its small midpoint grip.
        mouseDrag(control, 400, 170, 40, 0, Qt.LeftButton)
        verify(spy.count > 0)
        const values = spy.signalArguments[spy.count - 1]
        fuzzyCompare(values[0], 0.25, 0.001)
        fuzzyCompare(values[1], 0.25, 0.001)
        fuzzyCompare(values[2], 0.60, 0.02)
        fuzzyCompare(values[3], 0.50, 0.001)
    }

    function test_cropCanBeDrawnDirectlyOnThePhoto() {
        const control = createControl(cropOverlayComponent)
        const spy = createSpy(control, "cropRequested")

        mouseDrag(control, 110, 110, 70, 30, Qt.LeftButton)
        verify(spy.count > 0)
        const values = spy.signalArguments[spy.count - 1]
        fuzzyCompare(values[0], 0.025, 0.02)
        fuzzyCompare(values[1], 0.05, 0.02)
        fuzzyCompare(values[2], 0.175, 0.02)
        fuzzyCompare(values[3], 0.18, 0.02)
    }

    function test_cropAspectLockSurvivesCornerDragging() {
        const control = createControl(cropOverlayComponent)
        control.aspectRatio = 1.0
        const spy = createSpy(control, "cropRequested")

        mouseDrag(control, 200, 150, -40, -20, Qt.LeftButton)
        verify(spy.count > 0)
        const values = spy.signalArguments[spy.count - 1]
        const pixelRatio = values[2] * control.imageWidth
            / (values[3] * control.imageHeight)
        fuzzyCompare(pixelRatio, 1.0, 0.02)
    }

    function test_cropCanvasDoesNotCloseTheEditorPanel() {
        const composition = createControl(editorCropCompositionComponent)
        const cropSpy = createSpy(composition.cropOverlay, "cropRequested")
        const closeSpy = createSpy(composition.editorPanel, "panelOpenChangedByUser")

        mouseDrag(composition, 520, 150, 35, 25, Qt.LeftButton)
        verify(cropSpy.count > 0)
        compare(closeSpy.count, 0)
        compare(composition.editorPanel.panelOpen, true)
    }

    function test_customCropAspectRatioEmitsOnlyValidValues() {
        const composition = createControl(editorCropCompositionComponent)
        const ratioSpy = createSpy(
            composition.editorPanel,
            "customCropAspectRatioRequested"
        )

        const customChip = findChild(composition.editorPanel, "editorCustomAspectChip")
        verify(customChip)
        customChip.clicked()
        compare(composition.editorPanel.customAspectEditorOpen, true)
        const widthInput = findChild(composition.editorPanel, "customAspectWidthInput")
        verify(widthInput)
        tryVerify(function() { return widthInput.activeFocus })
        const applyChip = findChild(composition.editorPanel, "editorApplyCustomAspectChip")
        verify(applyChip)

        composition.editorPanel.customAspectWidthText = "7"
        composition.editorPanel.customAspectHeightText = "5"
        applyChip.clicked()
        compare(ratioSpy.count, 1)
        fuzzyCompare(ratioSpy.signalArguments[0][0], 1.4, 0.000001)
        compare(composition.editorPanel.cropAspectPreset, "free")

        composition.editorPanel.customAspectHeightText = "0"
        compare(applyChip.enabled, false)
        applyChip.clicked()
        compare(ratioSpy.count, 1)
    }

    function test_editorModeContentKeepsLiveValuesAndRequests() {
        const composition = createControl(editorCropCompositionComponent)
        const editor = composition.editorPanel
        editor.cropMode = false
        editor.activeTool = "adjust"
        editor.exposure = 0.3
        wait(0)

        const exposure = findChild(editor, "editorExposureControl")
        verify(exposure)
        fuzzyCompare(exposure.value, 0.3, 0.000001)
        const exposureSpy = createSpy(editor, "exposureRequested")
        exposure.commitValue(0.6)
        compare(exposureSpy.count, 1)
        fuzzyCompare(exposureSpy.signalArguments[0][0], 0.6, 0.000001)

        editor.activeTool = "export"
        editor.exportScale = 1.5
        wait(0)
        const scale = findChild(editor, "editorExportScaleControl")
        verify(scale)
        fuzzyCompare(scale.value, 1.5, 0.000001)
        const scaleSpy = createSpy(editor, "exportScaleRequested")
        scale.commitValue(1.8)
        compare(scaleSpy.count, 1)
        fuzzyCompare(scaleSpy.signalArguments[0][0], 1.8, 0.000001)

        const jpeg = findChild(editor, "editorExportFormat_jpeg")
        verify(jpeg)
        const formatSpy = createSpy(editor, "exportFormatRequested")
        jpeg.clicked()
        compare(formatSpy.count, 1)
        compare(formatSpy.signalArguments[0][0], "jpeg")
    }

    function test_buttonsEmitClicks() {
        const chip = createControl(actionChipComponent)
        const chipSpy = createSpy(chip, "clicked")
        mouseClick(chip, chip.width / 2, chip.height / 2, Qt.LeftButton)
        compare(chipSpy.count, 1)

        chip.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(chipSpy.count, 2)

        const header = createControl(headerButtonComponent)
        const headerSpy = createSpy(header, "clicked")
        mouseClick(header, header.width / 2, header.height / 2, Qt.LeftButton)
        compare(headerSpy.count, 1)

        header.forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(headerSpy.count, 2)
    }

    function test_temporalControlsHideForStillAndExposeFrameState() {
        const control = createControl(temporalControlsComponent)
        compare(control.visible, true)
        compare(control.timeLabel, "3 / 5")
        fuzzyCompare(control.clampedPosition, 0.5, 0.000001)

        control.hasTimeline = false
        compare(control.visible, false)
    }

    function test_temporalControlsEmitTransportAndBoundedSeekIntent() {
        const control = createControl(temporalControlsComponent)
        const toggleSpy = createSpy(control, "togglePlaybackRequested")
        const seekSpy = createSpy(control, "seekRequested")
        const stepSpy = createSpy(control, "stepRequested")

        const play = findChild(control, "temporalPlayButton")
        const scrubber = findChild(control, "temporalScrubberMouse")
        const previous = findChild(control, "temporalPreviousButton")
        const next = findChild(control, "temporalNextButton")
        verify(play)
        verify(scrubber)
        verify(previous)
        verify(next)

        mouseClick(play, play.width / 2, play.height / 2, Qt.LeftButton)
        compare(toggleSpy.count, 1)

        mouseClick(previous, previous.width / 2, previous.height / 2, Qt.LeftButton)
        mouseClick(next, next.width / 2, next.height / 2, Qt.LeftButton)
        compare(stepSpy.count, 2)
        compare(stepSpy.signalArguments[0][0], -1)
        compare(stepSpy.signalArguments[1][0], 1)

        mouseClick(scrubber, scrubber.width * 0.75, scrubber.height / 2, Qt.LeftButton)
        verify(seekSpy.count >= 1)
        const value = seekSpy.signalArguments[seekSpy.count - 1][0]
        fuzzyCompare(value, 0.75, 0.03)

        control.canScrub = false
        const before = seekSpy.count
        mouseClick(scrubber, scrubber.width * 0.25, scrubber.height / 2, Qt.LeftButton)
        compare(seekSpy.count, before)
    }

    function test_overlayButtonsAreConnected() {
        const overlay = createControl(overlayComponent)
        const fullscreenSpy = createSpy(overlay, "toggleFullScreenRequested")
        const closeSpy = createSpy(overlay, "closeRequested")

        mouseClick(overlay, 39, 39, Qt.LeftButton)
        compare(fullscreenSpy.count, 1)

        mouseClick(overlay, overlay.width - 31, 31, Qt.LeftButton)
        compare(closeSpy.count, 1)

        overlay.fullScreenMode = false
        mouseClick(overlay, 30, 30, Qt.LeftButton)
        compare(fullscreenSpy.count, 1)
    }

    function test_notificationCategoriesHistoryAndAutoHide() {
        const center = createControl(notificationCenterComponent)
        center.begin("image", "Opening image", "large-photo.dng")
        compare(center.toastCategory, "warning")
        compare(center.anyLoading, true)
        compare(center.toastVisible, true)
        compare(center.notificationCount, 1)
        wait(1000)
        compare(center.toastVisible, true)
        tryCompare(center, "toastVisible", false, 2500)

        center.finish("image", "success", "Image ready", "large-photo.dng")
        compare(center.toastCategory, "success")
        compare(center.anyLoading, false)
        compare(center.notificationCount, 1)
        compare(center.notifications[0].category, "success")
        compare(center.notifications[0].title, "Image ready")
        compare(center.toastVisible, false)

        const button = findChild(center, "notificationHistoryButton")
        verify(button)
        compare(button.accessibleName, "Notifications")
        const indicator = findChild(center, "notificationIndicator")
        verify(indicator)
        compare(indicator.visible, true)
        compare(indicator.width, 11)
        mouseClick(button, button.width / 2, button.height / 2, Qt.LeftButton)
        compare(center.historyOpen, true)
        compare(center.notificationCount, 1)

        center.finish("save", "error", "Image could not be saved", "Disk is full")
        compare(center.toastCategory, "error")
        compare(center.toastVisible, true)
        compare(center.notificationCount, 2)
        compare(center.historyOpen, true)
        center.historyOpen = false
        tryCompare(center, "toastVisible", false, 2500)

        center.endSession()
        compare(center.sessionActive, false)
        compare(center.notificationCount, 0)
        compare(center.anyLoading, false)
        compare(center.historyOpen, false)
        compare(center.toastVisible, false)
        compare(indicator.visible, false)
        center.finish("save", "success", "Late save", "")
        compare(center.notificationCount, 0)

        center.startSession()
        center.begin("image", "Opening another image", "next.jpg")
        compare(center.notificationCount, 1)
        compare(center.anyLoading, true)
        center.clear()
        compare(center.notificationCount, 0)
        compare(center.toastVisible, false)
    }

    function test_notificationsReplaceToastsAndDelayImageOpening() {
        const center = createControl(notificationCenterComponent)

        center.begin("image", "Opening image", "small.jpg", 300)
        compare(center.toastVisible, false)
        center.finish("image", "success", "Image ready", "small.jpg · 40 ms")
        wait(350)
        compare(center.toastVisible, false)
        compare(center.notificationCount, 1)
        compare(center.notifications[0].category, "success")
        compare(center.notifications[0].detail, "small.jpg · 40 ms")

        center.begin("image", "Opening image", "large.dng", 100)
        compare(center.toastVisible, false)
        tryCompare(center, "toastVisible", true, 500)
        compare(center.toastCategory, "warning")
        center.update("image", "Opening image", "large.dng · Loading full resolution…")
        compare(center.toastDetail, "large.dng · Loading full resolution…")
        compare(center.notificationCount, 2)

        center.finish("image", "success", "Image ready", "large.dng · 980 ms")
        compare(center.toastVisible, false)
        compare(center.notificationCount, 2)
        compare(center.notifications[0].category, "success")
        compare(center.notifications[0].detail, "large.dng · 980 ms")

        center.begin("save", "Saving image", "large.dng")
        compare(center.toastVisible, true)
        center.finish("background", "error", "Background process error", "Failed")
        compare(center.toastCategory, "error")
        compare(center.toastTitle, "Background process error")
        compare(center.toastVisible, true)
        center.stop("save")
        compare(center.toastVisible, true)
        center.begin("image", "Opening image", "next.jpg", 300)
        compare(center.toastVisible, false)
        center.finish("settings", "success", "Settings saved", "")
        compare(center.toastVisible, false)
        wait(350)
        compare(center.toastVisible, false)
    }

    function test_notificationHistoryIsBounded() {
        const center = createControl(notificationCenterComponent)
        center.begin("save", "Saving image", "large.dng")
        for (let index = 0; index < 120; ++index)
            center.finish("event" + index, "success", "Event " + index, "")

        compare(center.notificationCount, 101)
        compare(center.notifications[0].title, "Event 119")
        compare(center.notifications[100].operationKey, "save")
        center.stop("save")
        compare(center.notificationCount, 100)
        compare(center.notifications[99].title, "Event 20")
    }

    function test_failedFullResolutionRequestKeepsReadyPreview() {
        const viewport = createControl(imageViewportComponent)
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        const failureSpy = createSpy(viewport, "imageRequestFailed")

        viewport.providerSource = "file:///no-such-licasa-image.png"
        tryCompare(failureSpy, "count", 1, 5000)
        compare(viewport.imageStatus, Image.Ready)
        compare(viewport.decodePending, false)
    }

    function test_floatingImageFillsNativeBoundsDuringResize() {
        const viewport = createControl(imageViewportComponent)
        const imageStage = findChild(viewport, "imageStage")
        verify(imageStage)

        viewport.width = 401
        viewport.height = 301
        viewport.currentScale = 0.5
        compare(imageStage.x, 0)
        compare(imageStage.y, 0)
        compare(imageStage.width, 401)
        compare(imageStage.height, 301)
        compare(imageStage.scale, 1)
        compare(viewport.inputRegion.width, 401)
        compare(viewport.inputRegion.height, 301)
        compare(viewport.pointHitsImage(400, 300), true)
        compare(viewport.pointHitsImage(402, 300), false)

        viewport.fullScreenMode = true
        compare(imageStage.width, 1240)
        compare(imageStage.height, 1248)
        compare(imageStage.scale, 0.5)
    }

    function test_imageLoaderRetainsReadyFrameDuringFullResolutionSwap() {
        const viewport = createControl(imageViewportComponent)
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        const previewSlot = viewport.activeImageSlot
        verify(previewSlot >= 0)

        viewport.previewPhase = false

        // The ready preview remains active while the other slot decodes.
        compare(viewport.imageStatus, Image.Ready)
        compare(viewport.activeImageSlot, previewSlot)
        tryVerify(function() {
            return viewport.activeImageSlot !== previewSlot
        }, 5000)
        compare(viewport.imageStatus, Image.Ready)
    }

    function test_parallelFullImageWaitsForPreviewThenPromotes() {
        const viewport = createTemporaryObject(imageViewportComponent, visualRoot, {
            "parallelFullResolutionEnabled": true
        })
        verify(viewport)
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        verify(viewport.speculativeFullKey.indexOf("licasa_parallel=full") >= 0)
        verify(viewport.activeImageSlot !== 2)

        viewport.previewPhase = false
        tryCompare(viewport, "activeImageSlot", 2, 5000)
        compare(viewport.imageStatus, Image.Ready)
        verify(viewport.activeRequestKey.indexOf("licasa_parallel=full") >= 0)
        viewport.releaseStaticImages()
        compare(viewport.speculativeFullKey, "")
    }

    function test_floatingPlaybackSingleAndDoubleClicks() {
        const viewport = createControl(imageViewportComponent)
        viewport.floatingPlaybackEnabled = true
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        const toggle = createSpy(viewport, "playbackToggleRequested")
        const fullscreen = createSpy(viewport, "enterFullscreenRequested")
        mouseClick(viewport, 100, 100, Qt.LeftButton)
        compare(toggle.count, 0)
        tryCompare(toggle, "count", 1, Qt.styleHints.mouseDoubleClickInterval + 500)
        mouseClick(viewport, 100, 100, Qt.LeftButton)
        tryCompare(toggle, "count", 2, Qt.styleHints.mouseDoubleClickInterval + 500)
        mouseDoubleClickSequence(viewport, 100, 100, Qt.LeftButton)
        compare(fullscreen.count, 1)
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        compare(toggle.count, 2)
    }

    function test_floatingPlaybackIgnoresDragAndCancelledIntent() {
        const viewport = createControl(imageViewportComponent)
        viewport.floatingPlaybackEnabled = true
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        const toggle = createSpy(viewport, "playbackToggleRequested")
        const move = createSpy(viewport, "windowMoveStarted")
        mouseDrag(viewport, 80, 80, 60, 20, Qt.LeftButton)
        compare(move.count, 1)
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        compare(toggle.count, 0)
        mouseClick(viewport, 100, 100, Qt.LeftButton)
        viewport.playbackInteractionEnabled = false
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        compare(toggle.count, 0)
        viewport.playbackInteractionEnabled = true
        mouseClick(viewport, 100, 100, Qt.LeftButton)
        viewport.fullScreenMode = true
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        compare(toggle.count, 0)
        viewport.fullScreenMode = false
        viewport.floatingPlaybackEnabled = false
        mouseClick(viewport, 100, 100, Qt.LeftButton)
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        compare(toggle.count, 0)
    }

    function test_rawFastStageUsesBoundedProgressiveRequest() {
        const viewport = createControl(imageViewportComponent)
        viewport.previewPhase = false
        viewport.rawFastPhase = true
        const key = viewport.requestKey()
        verify(key.indexOf("licasa_stage=raw-fast") >= 0)
        verify(key.indexOf("licasa_width=320") >= 0)
        verify(key.indexOf("licasa_height=240") >= 0)
    }

    function test_rawFullDetailUsesInteractiveDevelopmentStage() {
        const viewport = createControl(imageViewportComponent)
        viewport.previewPhase = false
        viewport.rawFastPhase = false
        viewport.rawPreviewFirst = true
        const key = viewport.requestKey()
        verify(key.indexOf("licasa_stage=raw-interactive") >= 0)
        verify(key.indexOf("licasa_width=0") >= 0)
        verify(key.indexOf("licasa_height=0") >= 0)
    }

    function test_imageBackdropTracksPixelAlignedImageEdge() {
        const viewport = createControl(imageViewportComponent)
        viewport.fullScreenMode = true
        // DSC00326.dng is upright 4024 x 6048. At the default 70% fit in a
        // 1920 x 1080 view, the original backing began at 708.5 while the
        // photo was rounded to 709, exposing a thin transparent left strip.
        viewport.width = 1920
        viewport.height = 1080
        viewport.imageX = 708.5
        viewport.imageY = 162
        viewport.imageWidth = 4024
        viewport.imageHeight = 6048
        viewport.currentScale = 0.125
        compare(viewport.renderedImageX, 709)
        compare(viewport.visibleImageLeft, 709)
        compare(viewport.visibleImageRight, 1212)

        viewport.width = 320
        viewport.height = 240
        viewport.devicePixelRatio = 2.0
        viewport.imageX = 40.3
        viewport.imageY = 30.3
        viewport.imageWidth = 100
        viewport.imageHeight = 80
        viewport.currentScale = 0.8

        compare(viewport.renderedImageX, 40.5)
        compare(viewport.renderedImageY, 30.5)
        compare(viewport.visibleImageLeft, viewport.renderedImageX)
        compare(viewport.visibleImageTop, viewport.renderedImageY)
        compare(viewport.visibleImageRight, viewport.renderedImageRight)
        compare(viewport.visibleImageBottom, viewport.renderedImageBottom)
        verify(!viewport.pointHitsImage(40.4, 50))
        verify(viewport.pointHitsImage(40.5, 50))

        // A zoom may move the fit position; the backing still follows it.
        viewport.currentScale = 0.9
        viewport.imageX = 35.2
        compare(viewport.visibleImageLeft, viewport.renderedImageX)
        compare(viewport.visibleImageRight, viewport.renderedImageRight)
    }

    function test_rawFullDetailHasNoManualAction() {
        const viewport = createControl(imageViewportComponent)
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        verify(!findChild(viewport, "loadRawFullDetail"))
    }

    function test_imageWheelZoomWorksInWindowedAndFullscreenModes() {
        const viewport = createControl(imageViewportComponent)
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        const zoomSpy = createSpy(viewport, "zoomRequested")

        mouseMove(viewport, 100, 100)
        mouseWheel(viewport, 100, 100, 0, 120, Qt.NoButton)
        tryCompare(zoomSpy, "count", 1)
        verify(zoomSpy.signalArguments[0][0] > 1.0)
        fuzzyCompare(zoomSpy.signalArguments[0][1], 100, 0.01)
        fuzzyCompare(zoomSpy.signalArguments[0][2], 100, 0.01)

        viewport.fullScreenMode = true
        mouseWheel(viewport, 100, 100, 0, -120, Qt.NoButton)
        tryCompare(zoomSpy, "count", 2)
        verify(zoomSpy.signalArguments[1][0] < 1.0)

        // The wheel handler follows the displayed photo rather than claiming
        // the empty fullscreen backdrop.
        mouseWheel(viewport, 300, 100, 0, 120, Qt.NoButton)
        wait(20)
        compare(zoomSpy.count, 2)
    }

    function test_imageWheelUsesPrecisionPixelDeltaWhenAvailable() {
        const viewport = createControl(imageViewportComponent)
        compare(viewport.normalizedWheelDelta({
            "angleDelta": { "y": 1 },
            "pixelDelta": { "y": 2 }
        }), 12)
    }

    function test_imageWheelHonorsInteractionBlocking() {
        const viewport = createControl(imageViewportComponent)
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        const zoomSpy = createSpy(viewport, "zoomRequested")
        viewport.interactionBlocked = true

        mouseMove(viewport, 100, 100)
        mouseWheel(viewport, 100, 100, 0, 120, Qt.NoButton)
        wait(20)
        compare(zoomSpy.count, 0)
    }

    function test_animatedImageAdvancesBeyondTheFirstFrame() {
        const viewport = createControl(animatedViewportComponent)
        tryCompare(viewport, "imageStatus", Image.Ready, 5000)
        tryVerify(function() { return viewport.animationFrameCount > 1 }, 5000)
        tryCompare(viewport, "activeImageSlot", -1, 5000)
        compare(viewport.pendingImageSlot, -1)

        const firstFrame = viewport.animationCurrentFrame
        tryVerify(function() {
            return viewport.animationCurrentFrame !== firstFrame
        }, 3000)
    }

    function test_editorSessionGroupsSliderChangesIntoOneUndoStep() {
        const session = createTemporaryObject(editorSessionComponent, testCase)
        verify(session)

        session.beginInteraction()
        session.setAdjustment("exposure", 0.2)
        session.setAdjustment("exposure", 0.6)
        session.finishInteraction()

        fuzzyCompare(session.exposure, 0.6, 0.000001)
        compare(session.undoStack.length, 1)
        session.undo()
        fuzzyCompare(session.exposure, 0.0, 0.000001)
        verify(session.canRedo)
        session.redo()
        fuzzyCompare(session.exposure, 0.6, 0.000001)
    }

    function test_editorSessionRejectsUnknownAndNonFiniteAdjustments() {
        const session = createTemporaryObject(editorSessionComponent, testCase)
        verify(session)
        const editSpy = createSpy(session, "editsChanged")

        session.setAdjustment("exposure", 0.4)
        fuzzyCompare(session.exposure, 0.4, 0.000001)
        session.setAdjustment("exposure", NaN)
        compare(session.exposure, 0.0)
        verify(Number.isFinite(session.exposure))

        const signalCount = editSpy.count
        session.setAdjustment("unexpectedProperty", 1.0)
        compare(editSpy.count, signalCount)
        compare(session.unexpectedProperty, undefined)

        session.updateDraftCrop(NaN, Infinity, -Infinity, NaN)
        verify(Number.isFinite(session.draftCropX))
        verify(Number.isFinite(session.draftCropY))
        verify(Number.isFinite(session.draftCropWidth))
        verify(Number.isFinite(session.draftCropHeight))
    }

    function test_editorSessionCropPresetIsAppliedAndUndoable() {
        const session = createTemporaryObject(editorSessionComponent, testCase)
        verify(session)

        session.beginCrop()
        session.applyCropPreset("16:9", 1200, 800)
        fuzzyCompare(session.draftCropX, 0.0, 0.000001)
        fuzzyCompare(session.draftCropY, 0.078125, 0.000001)
        fuzzyCompare(session.draftCropWidth, 1.0, 0.000001)
        fuzzyCompare(session.draftCropHeight, 0.84375, 0.000001)

        session.applyDraftCrop()
        compare(session.cropMode, false)
        fuzzyCompare(session.cropHeight, 0.84375, 0.000001)
        session.undo()
        fuzzyCompare(session.cropY, 0.0, 0.000001)
        fuzzyCompare(session.cropHeight, 1.0, 0.000001)
    }

    function test_editorSessionSupportsPortraitCropPreset() {
        const session = createTemporaryObject(editorSessionComponent, testCase)
        verify(session)

        session.beginCrop()
        session.applyCropPreset("4:5", 1200, 800)
        const pixelRatio = session.draftCropWidth * 1200
            / (session.draftCropHeight * 800)
        fuzzyCompare(pixelRatio, 4.0 / 5.0, 0.000001)
    }

    function test_editorSessionRotationPreservesCropCoordinates() {
        const session = createTemporaryObject(editorSessionComponent, testCase)
        verify(session)
        session.cropX = 0.1
        session.cropY = 0.2
        session.cropWidth = 0.5
        session.cropHeight = 0.4

        session.rotateRight()
        compare(session.quarterTurns, 1)
        fuzzyCompare(session.cropX, 0.4, 0.000001)
        fuzzyCompare(session.cropY, 0.1, 0.000001)
        fuzzyCompare(session.cropWidth, 0.4, 0.000001)
        fuzzyCompare(session.cropHeight, 0.5, 0.000001)

        session.undo()
        compare(session.quarterTurns, 0)
        fuzzyCompare(session.cropX, 0.1, 0.000001)
        fuzzyCompare(session.cropY, 0.2, 0.000001)
        fuzzyCompare(session.cropWidth, 0.5, 0.000001)
        fuzzyCompare(session.cropHeight, 0.4, 0.000001)
    }
    }
}
