import QtQuick
import QtTest
import "../qml"

Item {
    id: visualRoot
    width: 640
    height: 160

    TestCase {
        id: testCase
        name: "ViewerTemporalControls"
        when: windowShown

        Component {
            id: controlComponent
            ViewerTemporalControls {}
        }
        Component {
            id: motionComponent
            QtObject {
                property bool available: true
                property bool active: true
                property bool playing: false
                property bool seekable: true
                property int durationMs: 2000
                property int positionMs: 500
                property int toggles: 0
                property int lastSeek: -1
                function togglePlayback() { toggles += 1 }
                function seek(position) { lastSeek = position }
            }
        }
        Component {
            id: frameSaveCoordinatorComponent
            QtObject {
                property bool canSaveFrame: true
            }
        }
        Component {
            id: motionExportServiceComponent
            QtObject {
                property bool available: true
                property bool busy: false
                property string suggestedSuffix: "mp4"
            }
        }
        Component {
            id: animationExportServiceComponent
            QtObject {
                property bool busy: false
            }
        }
        Component {
            id: preferredCoverCoordinatorComponent
            QtObject {
                property bool canSetCurrent: true
                property bool hasPreferred: false
                property bool currentIsPreferred: false
                property int setCalls: 0
                property int clearCalls: 0
                function makeCurrentPreferred() { setCalls += 1 }
                function clearPreferred() { clearCalls += 1 }
            }
        }
        Component {
            id: signalSpyComponent
            SignalSpy {}
        }
        Component {
            id: animationComponent
            QtObject {
                property bool playing: false
                property int frameCount: 5
                property int currentFrame: 2
                property int lastSeek: -1
                function seekFrame(frame) { lastSeek = frame }
            }
        }

        function createControl(motion, animation, animated, frameSaveCoordinator,
                               exportService, animationExporter, coverCoordinator) {
            const control = createTemporaryObject(controlComponent, visualRoot, {
                motionSession: motion,
                frameSaveCoordinator: frameSaveCoordinator || null,
                motionExportService: exportService || null,
                animationExportService: animationExporter || null,
                animationController: animation,
                imageAnimated: animated,
                preferredCoverFrameCoordinator: coverCoordinator || null
            })
            verify(control)
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

        function test_motionFrameSaveIntentRequiresProductionCapability() {
            const motion = createTemporaryObject(motionComponent, testCase)
            const coordinator = createTemporaryObject(
                frameSaveCoordinatorComponent, testCase)
            const control = createControl(motion, null, false, coordinator)
            const saveSpy = createSpy(control, "saveFrameAsRequested")

            compare(control.saveFrameVisible, true)
            compare(control.canSaveFrame, true)
            control.saveFrameRequested()
            compare(saveSpy.count, 1)

            control.interactionEnabled = false
            control.saveFrameRequested()
            compare(saveSpy.count, 1)

            control.interactionEnabled = true
            coordinator.canSaveFrame = false
            compare(control.canSaveFrame, false)
            control.saveFrameRequested()
            compare(saveSpy.count, 1)

            coordinator.canSaveFrame = true
            motion.active = false
            compare(control.saveFrameVisible, false)
            compare(control.canSaveFrame, false)
            // Export follows Motion Photo capability, not decoded-frame
            // availability. With lazy services, the service is materialized
            // only after this intent is emitted.
            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, true)
        }

        function test_motionExportIntentRequiresProductionCapability() {
            const motion = createTemporaryObject(motionComponent, testCase)
            const exporter = createTemporaryObject(
                motionExportServiceComponent, testCase)
            const control = createControl(motion, null, false, null, exporter)
            const exportSpy = createSpy(control, "exportMotionAsRequested")

            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, true)
            control.exportMotionRequested()
            compare(exportSpy.count, 1)

            exporter.busy = true
            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, false)
            control.exportMotionRequested()
            compare(exportSpy.count, 1)

            exporter.busy = false
            control.interactionEnabled = false
            control.exportMotionRequested()
            compare(exportSpy.count, 1)

            control.interactionEnabled = true
            motion.active = false
            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, true)

            exporter.available = false
            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, true)

            control.motionExportService = null
            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, true)
            control.exportMotionRequested()
            compare(exportSpy.count, 2)
        }

        function test_motionTransportUsesMillisecondsAndHonorsBusyState() {
            const motion = createTemporaryObject(motionComponent, testCase)
            const control = createControl(motion, null, false)
            compare(control.timeBased, true)
            compare(control.canStep, false)
            compare(control.position, 0.25)
            control.togglePlaybackRequested()
            compare(motion.toggles, 1)
            control.seekRequested(0.75)
            compare(motion.lastSeek, 1500)
            control.seekRequested(5)
            compare(motion.lastSeek, 2000)
            control.interactionEnabled = false
            control.togglePlaybackRequested()
            control.seekRequested(0)
            compare(motion.toggles, 1)
            compare(motion.lastSeek, 2000)
        }

        function test_animationTakesPriorityAndRetainsFrameTransport() {
            const motion = createTemporaryObject(motionComponent, testCase)
            const animation = createTemporaryObject(animationComponent, testCase)
            const animationExporter = createTemporaryObject(
                animationExportServiceComponent, testCase)
            const control = createControl(
                motion, animation, true, null, null, animationExporter)
            const animationExportSpy = createSpy(
                control, "exportAnimationAsRequested")
            const motionExportSpy = createSpy(control, "exportMotionAsRequested")
            compare(control.timeBased, false)
            compare(control.saveFrameVisible, false)
            compare(control.canSaveFrame, false)
            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, true)
            control.exportMotionRequested()
            compare(animationExportSpy.count, 1)
            compare(motionExportSpy.count, 0)
            animationExporter.busy = true
            compare(control.canExportMotion, false)
            control.exportMotionRequested()
            compare(animationExportSpy.count, 1)
            animationExporter.busy = false
            control.animationExportService = null
            compare(control.exportMotionVisible, true)
            compare(control.canExportMotion, true)
            control.exportMotionRequested()
            compare(animationExportSpy.count, 2)
            compare(control.position, 0.5)
            control.togglePlaybackRequested()
            compare(animation.playing, true)
            compare(motion.toggles, 0)
            control.seekRequested(0.75)
            compare(animation.lastSeek, 3)
            control.stepRequested(-10)
            compare(animation.lastSeek, 0)
            control.stepRequested(10)
            compare(animation.lastSeek, 4)
            control.imageAnimated = false
            compare(control.timeBased, true)
            control.togglePlaybackRequested()
            compare(motion.toggles, 1)
        }

        function test_coverFrameIntentStaysAtCapabilityIntentBoundary() {
            const motion = createTemporaryObject(motionComponent, testCase)
            const cover = createTemporaryObject(
                preferredCoverCoordinatorComponent, testCase)
            const control = createControl(
                motion, null, false, null, null, null, cover)

            compare(control.coverFrameVisible, true)
            compare(control.canSetCoverFrame, true)
            compare(control.coverFramePreferred, false)
            compare(control.currentIsPreferredCover, false)

            control.coverFrameRequested()
            compare(cover.setCalls, 1)
            compare(cover.clearCalls, 0)

            cover.hasPreferred = true
            cover.currentIsPreferred = true
            compare(control.coverFramePreferred, true)
            compare(control.currentIsPreferredCover, true)
            control.coverFrameRequested()
            compare(cover.clearCalls, 1)

            cover.currentIsPreferred = false
            cover.canSetCurrent = false
            compare(control.coverFrameVisible, true)
            compare(control.canSetCoverFrame, true)
            control.coverFrameRequested()
            compare(cover.clearCalls, 2)

            control.interactionEnabled = false
            control.coverFrameRequested()
            compare(cover.setCalls, 1)
            compare(cover.clearCalls, 2)
        }

        function test_missingControllersAndUnknownDurationAreSafe() {
            const control = createControl(null, null, false)
            compare(control.playing, false)
            compare(control.frameCount, 0)
            control.togglePlaybackRequested()
            control.seekRequested(0.5)
            control.stepRequested(1)
            const motion = createTemporaryObject(motionComponent, testCase, {durationMs: 0})
            control.motionSession = motion
            compare(control.canScrub, false)
            compare(control.position, 0)
            control.seekRequested(0.5)
            compare(motion.lastSeek, -1)
        }
    }
}
