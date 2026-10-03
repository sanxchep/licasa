import QtQuick
import QtTest
import "../qml/ViewerMath.js" as ViewerMath

TestCase {
    name: "ViewerMath"

    function test_clamp() {
        compare(ViewerMath.clamp(-4, 0, 10), 0)
        compare(ViewerMath.clamp(6, 0, 10), 6)
        compare(ViewerMath.clamp(14, 0, 10), 10)
    }

    function test_fitScalePreservesAspectRatio() {
        compare(ViewerMath.fitScale(4000, 2000, 1000, 800, 0.02, 16.0), 0.25)
        compare(ViewerMath.fitScale(1000, 2000, 900, 500, 0.02, 16.0), 0.25)
        compare(ViewerMath.fitScale(100, 100, 10000, 10000, 0.02, 16.0), 16.0)
    }

    function test_editorFitStaysInsideSpaceBesidePanel() {
        const viewportWidth = 1600
        const viewportHeight = 900
        const panelBoundary = 420
        const margin = 24
        const padding = 0.88
        const availableWidth = viewportWidth - panelBoundary - margin
        const availableHeight = viewportHeight - margin * 2

        for (const size of [[4000, 3000], [3000, 4000], [6000, 2000]]) {
            const scale = ViewerMath.fitScaleForInsetWorkspace(
                size[0], size[1], viewportWidth, viewportHeight,
                panelBoundary, margin, padding, 0.02, 16.0)
            const scaledWidth = size[0] * scale
            const scaledHeight = size[1] * scale
            verify(scaledWidth <= availableWidth * padding + 0.01)
            verify(scaledHeight <= availableHeight * padding + 0.01)
            fuzzyCompare(Math.max(scaledWidth / availableWidth,
                                  scaledHeight / availableHeight), padding, 0.000001)
        }
    }

    function test_wheelFactorIsBoundedAndSymmetric() {
        const zoomIn = ViewerMath.wheelFactor(1.001, 500, 120)
        const zoomOut = ViewerMath.wheelFactor(1.001, -500, 120)
        verify(zoomIn > 1.0)
        verify(zoomOut < 1.0)
        fuzzyCompare(zoomIn * zoomOut, 1.0, 0.000001)
    }

    function test_nonFiniteInputsUseSafeFallbacks() {
        compare(ViewerMath.clamp(NaN, -2, 4), -2)
        compare(ViewerMath.fitScale(Infinity, 100, 800, 600, 0.02, 16.0), 1.0)
        compare(ViewerMath.wheelFactor(1.001, NaN, 120), 1.0)
        compare(ViewerMath.wheelFactor(Infinity, 120, 120), 1.0)
        compare(ViewerMath.scaledExtent(400, Infinity), 1)
    }

    function test_scaledWindowExtentRoundsOutward() {
        compare(ViewerMath.scaledExtent(400, 0.5), 200)
        compare(ViewerMath.scaledExtent(333, 0.5), 167)
    }

    function test_zoomedWindowStaysInsideSelectedMonitor() {
        const constrained = ViewerMath.constrainRectToBounds(
            3650, 300, 1200, 700,
            1920, 392, 1920, 1048
        )

        compare(constrained.x, 2640)
        compare(constrained.y, 392)
        compare(constrained.width, 1200)
        compare(constrained.height, 700)
    }

    function test_freeFullscreenPlacementKeepsOnlyAGrabStripVisible() {
        compare(ViewerMath.freeImagePosition(250, 200, 800, 48), 250)
        compare(ViewerMath.freeImagePosition(-500, 200, 800, 48), -152)
        compare(ViewerMath.freeImagePosition(900, 200, 800, 48), 752)

        // Very small images remain fully recoverable rather than disappearing.
        compare(ViewerMath.freeImagePosition(-20, 20, 800, 48), 0)
        compare(ViewerMath.freeImagePosition(900, 20, 800, 48), 780)
    }

    function test_fullscreenRecoveryDetectsLostImageBeforeClamping() {
        verify(ViewerMath.needsViewportRecovery(
            -500, 200, 200, 100, 0, 0, 800, 600, 48))
        verify(ViewerMath.needsViewportRecovery(
            790, 200, 200, 100, 0, 0, 800, 600, 48))
        verify(!ViewerMath.needsViewportRecovery(
            752, 200, 200, 100, 0, 0, 800, 600, 48))
        verify(!ViewerMath.needsViewportRecovery(
            250, 200, 200, 100, 0, 0, 800, 600, 48))
    }

    function test_fullscreenRecoverySupportsInsetEditorWorkspace() {
        verify(ViewerMath.needsViewportRecovery(
            100, 200, 300, 200, 420, 0, 1080, 900, 48))
        verify(!ViewerMath.needsViewportRecovery(
            420, 200, 300, 200, 420, 0, 1080, 900, 48))

        // A photo smaller than the grip only needs to remain fully visible.
        verify(!ViewerMath.needsViewportRecovery(
            500, 200, 20, 20, 420, 0, 1080, 900, 48))
    }

    function test_boundedRectRoundsOutwardAndClips() {
        const region = ViewerMath.boundedRect(10.4, 20.8, 100.2, 50.4, 200, 200)
        compare(region.x, 10)
        compare(region.y, 20)
        compare(region.width, 101)
        compare(region.height, 52)

        const clipped = ViewerMath.boundedRect(-10, 190, 40, 30, 200, 200)
        compare(clipped.x, 0)
        compare(clipped.y, 190)
        compare(clipped.width, 30)
        compare(clipped.height, 10)
    }
}
