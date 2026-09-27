import QtQuick
import QtTest
import "../qml"

TestCase {
    name: "TemporalExportCoordinator"

    Component {
        id: coordinatorComponent
        TemporalExportCoordinator {}
    }
    Component {
        id: serviceComponent
        QtObject {
            property bool available: true
            property bool busy: false
            property string suggestedSuffix: "mp4"
            property var requests: []
            signal exportCompleted(url sourceUrl, url destinationUrl)
            signal exportFailed(url sourceUrl, string message)
            function exportCopy(source, destination) {
                requests.push([source.toString(), destination === undefined ? "" : destination.toString()])
            }
        }
    }

    function test_explicitInputsFollowNavigationAndCapabilities() {
        const motion = createTemporaryObject(serviceComponent, this)
        const animation = createTemporaryObject(serviceComponent, this)
        const coordinator = createTemporaryObject(coordinatorComponent, this, {
            sourceUrl: "file:///tmp/first.JPG",
            imageAnimated: false,
            motionExportService: motion,
            animationExportService: animation
        })
        verify(coordinator !== null)
        compare(coordinator.suggestedMotionExportUrl(), "file:///tmp/first-motion.mp4")
        coordinator.exportMotionCopy("file:///tmp/copy.mp4")
        compare(motion.requests.length, 1)
        compare(motion.requests[0][0], "file:///tmp/copy.mp4")
        motion.busy = true
        coordinator.exportMotionCopy("file:///tmp/ignored.mp4")
        motion.busy = false
        motion.available = false
        coordinator.exportMotionCopy("file:///tmp/ignored.mp4")
        compare(motion.requests.length, 1)
        compare(coordinator.suggestedMotionExportUrl(), "")

        coordinator.exportAnimationCopy("file:///tmp/ignored.gif")
        compare(animation.requests.length, 0)
        coordinator.sourceUrl = "file:///tmp/second.GIF"
        coordinator.imageAnimated = true
        compare(coordinator.suggestedAnimationExportUrl(), "file:///tmp/second-animation.gif")
        coordinator.exportAnimationCopy("file:///tmp/copy.gif")
        compare(animation.requests.length, 1)
        compare(animation.requests[0][0], "file:///tmp/second.GIF")
        compare(animation.requests[0][1], "file:///tmp/copy.gif")
        animation.busy = true
        coordinator.exportAnimationCopy("file:///tmp/ignored.gif")
        animation.busy = false
        coordinator.exportAnimationCopy("https://example.org/ignored.gif")
        coordinator.sourceUrl = ""
        coordinator.exportAnimationCopy("file:///tmp/ignored.gif")
        compare(animation.requests.length, 1)
    }
}
