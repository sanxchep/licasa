import QtQuick
import QtTest
import "../qml"

Item {
    id: visualRoot
    width: 640
    height: 160

    TestCase {
        id: testCase
        name: "TemporalControls"
        when: windowShown

        Component {
            id: temporalComponent
            TemporalControls {
                width: 540
                height: 58
                hasTimeline: true
            }
        }

        Component {
            id: signalSpyComponent
            SignalSpy {}
        }

        function createControl() {
            const control = createTemporaryObject(temporalComponent, visualRoot, {
                "x": 20,
                "y": 20
            })
            verify(control)
            waitForRendering(control)
            return control
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

        function test_frameModeRemainsBackwardCompatible() {
            const control = createControl()
            control.frameCount = 12
            control.currentFrame = 4
            control.canScrub = true

            compare(control.timeBased, false)
            compare(control.timeLabel, "5 / 12")
            compare(control.canStep, true)
        }

        function test_timeModeFormatsMotionPhotoTimeline() {
            const control = createControl()
            control.timeBased = true
            control.positionMs = 61000
            control.durationMs = 125000

            compare(control.timeLabel, "1:01 / 2:05")
            compare(control.formatTimeMs(3661000), "1:01:01")
            compare(control.formatTimeMs(NaN), "0:00")
        }

        function test_saveFrameButtonRemainsIntentOnly() {
            const control = createControl()
            control.saveFrameVisible = true
            control.canSaveFrame = true

            const saveSpy = createSpy(control, "saveFrameRequested")
            const saveButton = findChild(control, "temporalSaveFrameButton")
            verify(saveButton)
            verify(saveButton.visible)

            mouseClick(saveButton, saveButton.width / 2, saveButton.height / 2,
                       Qt.LeftButton)
            compare(saveSpy.count, 1)

            control.interactionEnabled = false
            mouseClick(saveButton, saveButton.width / 2, saveButton.height / 2,
                       Qt.LeftButton)
            compare(saveSpy.count, 1)
        }

        function test_coverFrameButtonRemainsIntentOnly() {
            const control = createControl()
            control.coverFrameVisible = true
            control.canSetCoverFrame = true

            const coverSpy = createSpy(control, "coverFrameRequested")
            const coverButton = findChild(control, "temporalCoverFrameButton")
            verify(coverButton)
            verify(coverButton.visible)

            mouseClick(coverButton, coverButton.width / 2, coverButton.height / 2,
                       Qt.LeftButton)
            compare(coverSpy.count, 1)

            control.canSetCoverFrame = false
            mouseClick(coverButton, coverButton.width / 2, coverButton.height / 2,
                       Qt.LeftButton)
            compare(coverSpy.count, 1)
        }

        function test_exportMotionButtonRemainsIntentOnly() {
            const control = createControl()
            control.exportMotionVisible = true
            control.canExportMotion = true

            const exportSpy = createSpy(control, "exportMotionRequested")
            const exportButton = findChild(control, "temporalExportMotionButton")
            verify(exportButton)
            verify(exportButton.visible)

            mouseClick(exportButton, exportButton.width / 2, exportButton.height / 2,
                       Qt.LeftButton)
            compare(exportSpy.count, 1)

            control.canExportMotion = false
            mouseClick(exportButton, exportButton.width / 2, exportButton.height / 2,
                       Qt.LeftButton)
            compare(exportSpy.count, 1)
        }

        function test_scrubAndTransportRemainIntentOnly() {
            const control = createControl()
            control.timeBased = true
            control.durationMs = 3000
            control.canScrub = true
            control.canStep = false

            const toggleSpy = createSpy(control, "togglePlaybackRequested")
            const seekSpy = createSpy(control, "seekRequested")
            const stepSpy = createSpy(control, "stepRequested")

            const playButton = findChild(control, "temporalPlayButton")
            verify(playButton)
            mouseClick(playButton, playButton.width / 2, playButton.height / 2,
                       Qt.LeftButton)
            compare(toggleSpy.count, 1)

            const scrubber = findChild(control, "temporalScrubberMouse")
            verify(scrubber)
            mouseClick(scrubber, scrubber.width * 0.75, scrubber.height / 2,
                       Qt.LeftButton)
            compare(seekSpy.count, 1)
            fuzzyCompare(seekSpy.signalArguments[0][0], 0.75, 0.03)

            const previousButton = findChild(control, "temporalPreviousButton")
            verify(previousButton)
            mouseClick(previousButton, previousButton.width / 2, previousButton.height / 2,
                       Qt.LeftButton)
            compare(stepSpy.count, 0)
        }
    }
}
