import QtQuick
import QtTest
import "../qml"
import "../qml/ViewerMath.js" as ViewerMath

Item {
    id: testRoot

    width: 1000
    height: 800

    QtObject {
        id: host

        property real x: 100
        property real y: 100
        property real width: 400
        property real height: 200
        property bool visible: true
        property bool fullScreenMode: false
        property bool imageReady: true
        property real minScale: 0.02
        property real maxScale: 16.0
        property int zoomAnimationDurationMs: 0
        property real currentScale: 1.0
        property bool fitMode: false
        property real panX: 0.0
        property real panY: 0.0

        function hasImage() {
            return imageReady;
        }

        function imageWidth() {
            return 400;
        }

        function imageHeight() {
            return 200;
        }

        function clamp(value, minimum, maximum) {
            return ViewerMath.clamp(value, minimum, maximum);
        }

        function scaleForBoundingBox(maximumWidth, maximumHeight) {
            return ViewerMath.fitScale(imageWidth(), imageHeight(), maximumWidth, maximumHeight, minScale, maxScale);
        }

        function cancelZoomAnimations() {
            controller.cancelAnimation();
        }
    }

    QtObject {
        id: nativeOps

        property rect screenGeometry: Qt.rect(0, 0, 1000, 800)
        property rect nativeGeometry: Qt.rect(100, 100, 400, 200)
        property point cursorPosition: Qt.point(200, 200)
        property int setGeometryCount: 0
        property var geometrySamples: []
        property int setPositionCount: 0
        property int startSystemMoveCount: 0
        property bool systemMoveSupported: false
        property bool leftButtonPressed: false
        property bool gestureSupported: false
        property rect inputRectangle: Qt.rect(0, 0, 0, 0)
        property int swapCount: 0
        property int requestedFrames: 0

        function currentScreenGeometry() {
            return screenGeometry;
        }

        function globalCursorPosition() {
            return cursorPosition;
        }

        function currentWindowGeometry() {
            return nativeGeometry;
        }

        function startSystemMove() {
            startSystemMoveCount += 1;
            return systemMoveSupported;
        }

        function leftMouseButtonPressed() {
            return leftButtonPressed;
        }

        function traceZoom() {}

        function gestureWindowSupported() {
            return gestureSupported;
        }

        function setWindowInputRectangle(window, x, y, width, height) {
            inputRectangle = Qt.rect(x, y, width, height);
            return true;
        }

        function beginGestureWindows() {
            swapCount += 1;
            return true;
        }

        function endGestureWindows() {
            inputRectangle = Qt.rect(0, 0, 0, 0);
            swapCount += 1;
            return true;
        }

        function requestWindowFrame() {
            requestedFrames += 1;
            return true;
        }

        function setWindowGeometry(window, targetX, targetY, targetWidth, targetHeight) {
            nativeGeometry = Qt.rect(targetX, targetY, targetWidth, targetHeight);
            geometrySamples.push({
                "x": targetX,
                "y": targetY,
                "width": targetWidth,
                "height": targetHeight,
                "scale": controller.scale
            });
            window.x = targetX;
            window.y = targetY;
            window.width = targetWidth;
            window.height = targetHeight;
            setGeometryCount += 1;
            return true;
        }

        function setWindowPosition(window, targetX, targetY) {
            if (window === host) {
                nativeGeometry = Qt.rect(
                    targetX,
                    targetY,
                    nativeGeometry.width,
                    nativeGeometry.height
                );
            }
            window.x = targetX;
            window.y = targetY;
            setPositionCount += 1;
            return true;
        }
    }

    QtObject {
        id: gestureMock
        property bool readyForHandoff: false
        property bool visible: true
        property real x: 0
        property real y: 0
        property real opacity: 0
    }

    WindowedZoomController {
        id: controller

        hostWindow: host
        nativeOps: nativeOps
        gestureSurface: gestureMock
    }

    WindowedMoveController {
        id: moveController

        hostWindow: host
        nativeOps: nativeOps
        zoomController: controller
    }

    TestCase {
        name: "WindowedZoomController"

        function init() {
            host.x = 100;
            host.y = 100;
            host.width = 400;
            host.height = 200;
            host.visible = true;
            host.fullScreenMode = false;
            host.imageReady = true;
            host.currentScale = 1.0;
            host.zoomAnimationDurationMs = 0;
            host.fitMode = false;
            host.panX = 0.0;
            host.panY = 0.0;
            nativeOps.screenGeometry = Qt.rect(0, 0, 1000, 800);
            nativeOps.nativeGeometry = Qt.rect(100, 100, 400, 200);
            nativeOps.cursorPosition = Qt.point(200, 200);
            nativeOps.setGeometryCount = 0;
            nativeOps.geometrySamples = [];
            nativeOps.setPositionCount = 0;
            nativeOps.startSystemMoveCount = 0;
            nativeOps.systemMoveSupported = false;
            nativeOps.leftButtonPressed = false;
            moveController.cancel();
            controller.reset();
            gestureMock.readyForHandoff = false;
            gestureMock.visible = true;
            gestureMock.x = 0;
            gestureMock.y = 0;
            gestureMock.opacity = 0;
            nativeOps.gestureSupported = false;
            nativeOps.inputRectangle = Qt.rect(0, 0, 0, 0);
            nativeOps.swapCount = 0;
            nativeOps.requestedFrames = 0;
            controller.restoreGeometry = nativeOps.nativeGeometry;
            controller.restoreGeometryValid = true;
            controller.scale = 1.0;
            controller.targetScale = 1.0;
            controller.stateValid = true;
        }

        function cleanup() {
            moveController.cancel();
            controller.reset();
        }

        function test_manualMoveCrossesStaggeredMonitorWorkAreaBoundary() {
            host.x = 2100;
            host.y = 500;
            nativeOps.nativeGeometry = Qt.rect(2100, 500, 400, 200);
            nativeOps.cursorPosition = Qt.point(2200, 600);

            verify(moveController.begin());
            nativeOps.cursorPosition = Qt.point(100, 100);
            verify(moveController.update());

            compare(host.x, 0);
            compare(host.y, 0);
            compare(nativeOps.nativeGeometry, Qt.rect(0, 0, 400, 200));
            compare(nativeOps.setPositionCount, 1);

            moveController.finish();
            compare(moveController.active, false);
        }

        function test_clickDoesNotIssueANativeMove() {
            nativeOps.cursorPosition = Qt.point(200, 200);

            verify(moveController.begin());
            moveController.finish();

            compare(nativeOps.startSystemMoveCount, 1);
            compare(nativeOps.setPositionCount, 0);
            compare(moveController.active, false);
            compare(moveController.moved, false);
        }

        function test_screenBasedScaleLimitsPreserveAspectRatio() {
            fuzzyCompare(controller.minimumScale(), 0.25, 0.000001);
            fuzzyCompare(controller.openScale(), 0.75, 0.000001);
            fuzzyCompare(controller.maximumScale(), 2.0, 0.000001);
        }

        function test_zoomKeepsNativeWindowExactlyImageSized() {
            controller.zoomToScale(1.5, 200, 100);

            fuzzyCompare(controller.scale, 1.5, 0.000001);
            fuzzyCompare(controller.targetScale, 1.5, 0.000001);
            compare(nativeOps.nativeGeometry, Qt.rect(0, 50, 600, 300));
            compare(controller.restoreGeometry, nativeOps.nativeGeometry);
            fuzzyCompare(controller.imageX(), 0, 0.000001);
            fuzzyCompare(controller.imageY(), 0, 0.000001);
            compare(nativeOps.setGeometryCount, 1);

            controller.zoomToScale(0.75, 300, 150);

            fuzzyCompare(controller.scale, 0.75, 0.000001);
            compare(nativeOps.nativeGeometry, Qt.rect(150, 125, 300, 150));
            compare(controller.restoreGeometry, nativeOps.nativeGeometry);
            compare(nativeOps.setGeometryCount, 2);
        }

        function test_animatedZoomKeepsEveryFrameImageSizedOnStaggeredMonitor() {
            host.zoomAnimationDurationMs = 160;
            nativeOps.screenGeometry = Qt.rect(1920, 392, 1920, 1080);
            nativeOps.nativeGeometry = Qt.rect(2500, 392, 400, 200);
            host.x = 2500;
            host.y = 392;

            controller.zoomToScale(2.0, 200, 100);
            compare(controller.targetScale, 2.0);
            compare(controller.zoomAnimation.running, true);
            wait(70);
            verify(controller.scale > 1.0 && controller.scale < 2.0);

            // Retarget from the current animated frame, as consecutive wheel
            // events do on a mouse or touchpad.
            controller.zoomToScale(
                1.5,
                2700 - nativeOps.nativeGeometry.x,
                492 - nativeOps.nativeGeometry.y
            );
            tryCompare(controller.zoomAnimation, "running", false, 1000);
            tryVerify(function() {
                return nativeOps.nativeGeometry.width === 600
                    && nativeOps.nativeGeometry.height === 300
            }, 1000);
            fuzzyCompare(controller.scale, 1.5, 0.000001);
            compare(nativeOps.nativeGeometry.x, 2400);
            compare(nativeOps.nativeGeometry.width, 600);
            compare(nativeOps.nativeGeometry.height, 300);
            verify(nativeOps.geometrySamples.length > 2);
            for (const sample of nativeOps.geometrySamples) {
                compare(sample.width, ViewerMath.scaledExtent(400, sample.scale));
                compare(sample.height, ViewerMath.scaledExtent(200, sample.scale));
                verify(sample.x >= 1920 && sample.y >= 392);
                verify(sample.x + sample.width <= 3840);
                verify(sample.y + sample.height <= 1472);
            }
        }

        function test_gestureZoomPreparesFramesBeforeExactHandoff() {
            gestureMock.readyForHandoff = true;
            nativeOps.gestureSupported = true;
            host.zoomAnimationDurationMs = 160;

            controller.zoomToScale(1.5, 200, 100);
            compare(controller.gestureActive, true);
            compare(nativeOps.swapCount, 1);
            compare(nativeOps.nativeGeometry, Qt.rect(100, 100, 400, 200));
            wait(70);
            compare(nativeOps.setGeometryCount, 0);

            tryCompare(controller, "gesturePreparing", true, 1000);
            compare(nativeOps.setGeometryCount, 1);
            compare(nativeOps.nativeGeometry.width, 600);
            compare(nativeOps.nativeGeometry.height, 300);
            controller.gesturePrepareStartedAt = Date.now() - 100;
            controller.exactFrameSwapped();
            controller.exactFrameSwapped();
            compare(controller.gestureActive, true);
            compare(nativeOps.swapCount, 1);
            controller.exactFrameSwapped();

            compare(controller.gestureActive, false);
            compare(nativeOps.swapCount, 2);
            compare(nativeOps.nativeGeometry, Qt.rect(0, 50, 600, 300));
            compare(nativeOps.inputRectangle, Qt.rect(0, 0, 0, 0));
            compare(gestureMock.opacity, 0);
            compare(gestureMock.x, 0);
        }

        function test_gestureZoomUsesStaggeredMonitorBounds() {
            gestureMock.readyForHandoff = true;
            nativeOps.gestureSupported = true;
            nativeOps.screenGeometry = Qt.rect(1920, 327, 1920, 1080);
            nativeOps.nativeGeometry = Qt.rect(3400, 327, 400, 200);
            host.x = 3400;
            host.y = 327;
            host.zoomAnimationDurationMs = 160;

            controller.zoomToScale(2.0, 20, 20);
            compare(controller.gestureActive, true);
            compare(controller.gestureScreenGeometry, Qt.rect(1920, 327, 1920, 1080));
            compare(nativeOps.inputRectangle, Qt.rect(1480, 0, 400, 200));
            wait(190);
            compare(controller.gestureImageGeometry, Qt.rect(3040, 327, 800, 400));
            compare(nativeOps.setGeometryCount, 0);
        }

        function test_alternatingGestureKeepsStationaryPointerAnchored() {
            gestureMock.readyForHandoff = true;
            nativeOps.gestureSupported = true;
            host.zoomAnimationDurationMs = 160;

            controller.zoomToScale(1.5, 200, 100);
            wait(180);
            controller.zoomToScale(1.0, 300, 200);
            wait(180);

            compare(controller.gestureImageGeometry, Qt.rect(100, 100, 400, 200));
            compare(nativeOps.setGeometryCount, 0);
            compare(controller.gestureActive, true);
        }

        function test_cancelDuringGestureRestoresExactBoundsAndInput() {
            gestureMock.readyForHandoff = true;
            nativeOps.gestureSupported = true;
            host.zoomAnimationDurationMs = 160;

            controller.zoomToScale(1.5, 200, 100);
            compare(controller.gestureActive, true);
            wait(55);

            controller.cancelAnimation();

            compare(controller.gestureActive, false);
            compare(controller.gesturePreparing, false);
            compare(nativeOps.swapCount, 2);
            compare(nativeOps.inputRectangle, Qt.rect(0, 0, 0, 0));
            compare(nativeOps.nativeGeometry,
                    controller.exactGeometryForScale(controller.scale));
            compare(controller.geometryAnchorActive, false);
        }

        function test_firstFullscreenExitCentersOnTheCurrentMonitor() {
            controller.reset();
            controller.restoreGeometry = Qt.rect(2650, 820, 460, 220);
            controller.restoreGeometryValid = true;
            controller.restore();

            compare(nativeOps.nativeGeometry, Qt.rect(350, 325, 300, 150));
            compare(controller.restoreGeometry, Qt.rect(350, 325, 300, 150));
        }

        function test_idleImageWindowCanSitAtStaggeredMonitorTopEdge() {
            nativeOps.screenGeometry = Qt.rect(1920, 392, 1920, 1080);
            nativeOps.nativeGeometry = Qt.rect(2500, 392, 400, 200);
            host.x = 2500;
            host.y = 392;
            controller.restoreGeometry = nativeOps.nativeGeometry;
            controller.restoreGeometryValid = true;

            controller.restore();

            compare(nativeOps.nativeGeometry, Qt.rect(2500, 392, 400, 200));
            compare(controller.imageY(), 0);
            compare(controller.maximumScale(), 3.84);
        }

        function test_systemMoveRetainsAnExactImageSizedWindow() {
            nativeOps.systemMoveSupported = true;
            nativeOps.leftButtonPressed = true;
            nativeOps.cursorPosition = Qt.point(300, 200);

            verify(moveController.begin());
            compare(moveController.nativeMoveActive, true);
            compare(nativeOps.nativeGeometry, Qt.rect(100, 100, 400, 200));
            compare(nativeOps.startSystemMoveCount, 1);

            // Simulate the compositor moving the exact native window.
            nativeOps.nativeGeometry = Qt.rect(80, 60, 400, 200);
            host.x = 80;
            host.y = 60;
            verify(moveController.update());
            compare(nativeOps.setPositionCount, 0);
            fuzzyCompare(controller.imageX(), 0, 0.000001);
            fuzzyCompare(controller.imageY(), 0, 0.000001);

            nativeOps.leftButtonPressed = false;
            moveController.finish();
            compare(moveController.active, false);
            compare(moveController.nativeMoveActive, false);
            compare(controller.restoreGeometry, Qt.rect(80, 60, 400, 200));
        }

        function test_pointerZoomIsConstrainedToTheSelectedMonitor() {
            nativeOps.screenGeometry = Qt.rect(1920, 392, 1920, 1080);
            nativeOps.nativeGeometry = Qt.rect(3500, 1200, 400, 200);
            host.x = 3500;
            host.y = 1200;
            nativeOps.cursorPosition = Qt.point(3800, 1350);

            controller.zoomToScale(2.0, 300, 150);

            compare(nativeOps.nativeGeometry, Qt.rect(3040, 1050, 800, 400));
            compare(controller.restoreGeometry, nativeOps.nativeGeometry);
        }

        function test_resetClearsTransientZoomState() {
            controller.geometryAnchorActive = true;
            controller.suppressGeometryCapture = true;
            controller.reset();
            compare(controller.stateValid, false);
            compare(controller.scale, 1.0);
            compare(controller.targetScale, 1.0);
            compare(controller.geometryAnchorActive, false);
            fuzzyCompare(controller.imageX(), 0, 0.000001);
            fuzzyCompare(controller.imageY(), 0, 0.000001);
            compare(controller.suppressGeometryCapture, false);
        }
    }
}
