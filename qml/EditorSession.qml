/*
    EditorSession.qml
    -----------------
    Non-visual editor state, history, crop, transform, preset, and export model.
*/

import QtQuick

QtObject {
    id: root

    property real exposure: 0.0
    property real contrast: 0.0
    property real highlights: 0.0
    property real shadows: 0.0
    property real saturation: 0.0
    property real vibrance: 0.0
    property real warmth: 0.0
    property real tint: 0.0
    property real blur: 0.0
    property real sharpen: 0.0
    property real vignette: 0.0

    property bool flipHorizontal: false
    property bool flipVertical: false
    property int quarterTurns: 0

    property real cropX: 0.0
    property real cropY: 0.0
    property real cropWidth: 1.0
    property real cropHeight: 1.0
    property bool cropMode: false
    property real draftCropX: 0.0
    property real draftCropY: 0.0
    property real draftCropWidth: 1.0
    property real draftCropHeight: 1.0

    property bool compareOriginal: false
    property string exportFormat: "original"
    property real exportScale: 1.0
    property int exportQuality: 95

    property var undoStack: []
    property var redoStack: []
    property var interactionSnapshot: null

    readonly property real epsilon: 0.0005
    readonly property bool hasAdjustments:
        Math.abs(exposure) > epsilon
        || Math.abs(contrast) > epsilon
        || Math.abs(highlights) > epsilon
        || Math.abs(shadows) > epsilon
        || Math.abs(saturation) > epsilon
        || Math.abs(vibrance) > epsilon
        || Math.abs(warmth) > epsilon
        || Math.abs(tint) > epsilon
        || blur > epsilon
        || sharpen > epsilon
        || vignette > epsilon
        || normalizedQuarterTurns(quarterTurns) !== 0
        || flipHorizontal
        || flipVertical
        || Math.abs(cropX) > epsilon
        || Math.abs(cropY) > epsilon
        || Math.abs(cropWidth - 1.0) > epsilon
        || Math.abs(cropHeight - 1.0) > epsilon
    readonly property bool canUndo: undoStack.length > 0
    readonly property bool canRedo: redoStack.length > 0

    signal editsChanged()

    function clamp(value, minimum, maximum) {
        return Number.isFinite(value)
            ? Math.max(minimum, Math.min(maximum, value))
            : minimum
    }

    function normalizedQuarterTurns(value) {
        if (!Number.isFinite(value))
            return 0

        const remainder = Math.trunc(value) % 4
        return remainder < 0 ? remainder + 4 : remainder
    }

    function snapshot() {
        return {
            "exposure": exposure,
            "contrast": contrast,
            "highlights": highlights,
            "shadows": shadows,
            "saturation": saturation,
            "vibrance": vibrance,
            "warmth": warmth,
            "tint": tint,
            "blur": blur,
            "sharpen": sharpen,
            "vignette": vignette,
            "quarterTurns": normalizedQuarterTurns(quarterTurns),
            "flipHorizontal": flipHorizontal,
            "flipVertical": flipVertical,
            "cropX": cropX,
            "cropY": cropY,
            "cropWidth": cropWidth,
            "cropHeight": cropHeight
        }
    }

    function snapshotsEqual(first, second) {
        if (!first || !second)
            return first === second
        const keys = Object.keys(first)
        if (keys.length !== Object.keys(second).length)
            return false
        for (const key of keys) {
            if (first[key] !== second[key])
                return false
        }
        return true
    }

    function restoreSnapshot(value) {
        if (!value)
            return

        exposure = value.exposure
        contrast = value.contrast
        highlights = value.highlights
        shadows = value.shadows
        saturation = value.saturation
        vibrance = value.vibrance
        warmth = value.warmth
        tint = value.tint
        blur = value.blur
        sharpen = value.sharpen
        vignette = value.vignette
        quarterTurns = value.quarterTurns
        flipHorizontal = value.flipHorizontal
        flipVertical = value.flipVertical
        cropX = value.cropX
        cropY = value.cropY
        cropWidth = value.cropWidth
        cropHeight = value.cropHeight
        cropMode = false
        compareOriginal = false
        editsChanged()
    }

    function commitCheckpoint(previousSnapshot) {
        if (!previousSnapshot || snapshotsEqual(previousSnapshot, snapshot()))
            return

        let nextUndo = undoStack.concat([previousSnapshot])
        if (nextUndo.length > 50)
            nextUndo = nextUndo.slice(nextUndo.length - 50)
        undoStack = nextUndo
        redoStack = []
    }

    function beginInteraction() {
        if (interactionSnapshot === null)
            interactionSnapshot = snapshot()
    }

    function finishInteraction() {
        const previous = interactionSnapshot
        interactionSnapshot = null
        commitCheckpoint(previous)
    }

    function undo() {
        finishInteraction()
        if (!canUndo)
            return

        const previous = undoStack[undoStack.length - 1]
        undoStack = undoStack.slice(0, undoStack.length - 1)
        redoStack = redoStack.concat([snapshot()])
        restoreSnapshot(previous)
    }

    function redo() {
        finishInteraction()
        if (!canRedo)
            return

        const next = redoStack[redoStack.length - 1]
        redoStack = redoStack.slice(0, redoStack.length - 1)
        undoStack = undoStack.concat([snapshot()])
        restoreSnapshot(next)
    }

    function setAdjustment(name, value) {
        const safeValue = Number.isFinite(value) ? value : 0.0
        switch (name) {
        case "blur":
            blur = clamp(safeValue, 0.0, 0.35)
            break
        case "sharpen":
            sharpen = clamp(safeValue, 0.0, 1.0)
            break
        case "vignette":
            vignette = clamp(safeValue, 0.0, 1.0)
            break
        case "exposure":
            exposure = clamp(safeValue, -1.0, 1.0)
            break
        case "contrast":
            contrast = clamp(safeValue, -1.0, 1.0)
            break
        case "highlights":
            highlights = clamp(safeValue, -1.0, 1.0)
            break
        case "shadows":
            shadows = clamp(safeValue, -1.0, 1.0)
            break
        case "saturation":
            saturation = clamp(safeValue, -1.0, 1.0)
            break
        case "vibrance":
            vibrance = clamp(safeValue, -1.0, 1.0)
            break
        case "warmth":
            warmth = clamp(safeValue, -1.0, 1.0)
            break
        case "tint":
            tint = clamp(safeValue, -1.0, 1.0)
            break
        default:
            return
        }
        editsChanged()
    }

    function resetAdjustments() {
        exposure = 0.0
        contrast = 0.0
        highlights = 0.0
        shadows = 0.0
        saturation = 0.0
        vibrance = 0.0
        warmth = 0.0
        tint = 0.0
        blur = 0.0
        sharpen = 0.0
        vignette = 0.0
    }

    function resetEditValues() {
        resetAdjustments()
        quarterTurns = 0
        flipHorizontal = false
        flipVertical = false
        cropX = 0.0
        cropY = 0.0
        cropWidth = 1.0
        cropHeight = 1.0
        cropMode = false
        compareOriginal = false
    }

    function reset() {
        const previous = snapshot()
        resetEditValues()
        commitCheckpoint(previous)
        editsChanged()
    }

    function clear(resetExportSettings) {
        resetEditValues()
        draftCropX = 0.0
        draftCropY = 0.0
        draftCropWidth = 1.0
        draftCropHeight = 1.0
        undoStack = []
        redoStack = []
        interactionSnapshot = null

        if (resetExportSettings) {
            exportFormat = "original"
            exportScale = 1.0
            exportQuality = 95
        }
    }

    function applyPreset(name) {
        const previous = snapshot()
        resetAdjustments()
        compareOriginal = false

        switch (name) {
        case "bw":
            saturation = -1.0
            contrast = 0.10
            highlights = -0.08
            break
        case "pop":
            exposure = 0.06
            contrast = 0.22
            saturation = 0.18
            vibrance = 0.20
            warmth = 0.10
            sharpen = 0.16
            break
        case "cinematic":
            exposure = -0.04
            contrast = 0.16
            highlights = -0.16
            shadows = 0.10
            saturation = -0.10
            tint = 0.08
            vignette = 0.22
            break
        case "soft":
            exposure = 0.08
            contrast = -0.10
            highlights = -0.08
            shadows = 0.14
            saturation = -0.05
            warmth = 0.06
            blur = 0.08
            break
        case "vivid":
            contrast = 0.12
            highlights = -0.06
            shadows = 0.08
            saturation = 0.12
            vibrance = 0.32
            sharpen = 0.12
            break
        default:
            break
        }

        commitCheckpoint(previous)
        editsChanged()
    }

    function toggleFlipHorizontal() {
        const previous = snapshot()
        flipHorizontal = !flipHorizontal
        cropX = 1.0 - cropX - cropWidth
        commitCheckpoint(previous)
        editsChanged()
    }

    function toggleFlipVertical() {
        const previous = snapshot()
        flipVertical = !flipVertical
        cropY = 1.0 - cropY - cropHeight
        commitCheckpoint(previous)
        editsChanged()
    }

    function rotateLeft() {
        const previous = snapshot()
        const oldX = cropX
        const oldY = cropY
        const oldWidth = cropWidth
        const oldHeight = cropHeight
        quarterTurns = normalizedQuarterTurns(quarterTurns - 1)
        cropX = oldY
        cropY = 1.0 - oldX - oldWidth
        cropWidth = oldHeight
        cropHeight = oldWidth
        commitCheckpoint(previous)
        editsChanged()
    }

    function rotateRight() {
        const previous = snapshot()
        const oldX = cropX
        const oldY = cropY
        const oldWidth = cropWidth
        const oldHeight = cropHeight
        quarterTurns = normalizedQuarterTurns(quarterTurns + 1)
        cropX = 1.0 - oldY - oldHeight
        cropY = oldX
        cropWidth = oldHeight
        cropHeight = oldWidth
        commitCheckpoint(previous)
        editsChanged()
    }

    function resetRotation() {
        const previous = snapshot()
        let remaining = normalizedQuarterTurns(quarterTurns)
        let nextX = cropX
        let nextY = cropY
        let nextWidth = cropWidth
        let nextHeight = cropHeight
        while (remaining > 0) {
            const rotatedX = nextY
            const rotatedY = 1.0 - nextX - nextWidth
            const rotatedWidth = nextHeight
            const rotatedHeight = nextWidth
            nextX = rotatedX
            nextY = rotatedY
            nextWidth = rotatedWidth
            nextHeight = rotatedHeight
            remaining -= 1
        }
        quarterTurns = 0
        cropX = nextX
        cropY = nextY
        cropWidth = nextWidth
        cropHeight = nextHeight
        commitCheckpoint(previous)
        editsChanged()
    }

    function beginCrop() {
        if (cropMode)
            return

        compareOriginal = false
        draftCropX = cropX
        draftCropY = cropY
        draftCropWidth = cropWidth
        draftCropHeight = cropHeight
        cropMode = true
        editsChanged()
    }

    function updateDraftCrop(x, y, width, height) {
        const minimumExtent = 0.02
        draftCropX = clamp(x, 0.0, 1.0 - minimumExtent)
        draftCropY = clamp(y, 0.0, 1.0 - minimumExtent)
        draftCropWidth = clamp(width, minimumExtent, 1.0 - draftCropX)
        draftCropHeight = clamp(height, minimumExtent, 1.0 - draftCropY)
    }

    function cropPresetRatio(name) {
        switch (name) {
        case "square": return 1.0
        case "4:3": return 4.0 / 3.0
        case "3:2": return 3.0 / 2.0
        case "4:5": return 4.0 / 5.0
        case "16:9": return 16.0 / 9.0
        default: return 0.0
        }
    }

    function applyCropRatio(targetRatio, transformedWidth, transformedHeight) {
        if (!cropMode)
            beginCrop()

        if (!Number.isFinite(targetRatio)
                || !Number.isFinite(transformedWidth)
                || !Number.isFinite(transformedHeight)
                || targetRatio <= 0.0
                || transformedWidth <= 0
                || transformedHeight <= 0) {
            return
        }

        const imageRatio = transformedWidth / transformedHeight
        let width = 1.0
        let height = 1.0
        if (imageRatio > targetRatio)
            width = targetRatio / imageRatio
        else
            height = imageRatio / targetRatio
        updateDraftCrop((1.0 - width) / 2.0, (1.0 - height) / 2.0, width, height)
    }

    function applyCropPreset(name, transformedWidth, transformedHeight) {
        if (!cropMode)
            beginCrop()

        if (name === "original") {
            updateDraftCrop(0.0, 0.0, 1.0, 1.0)
            return
        }

        const targetRatio = cropPresetRatio(name)
        applyCropRatio(targetRatio, transformedWidth, transformedHeight)
    }

    function applyDraftCrop() {
        if (!cropMode)
            return

        const previous = snapshot()
        cropX = draftCropX
        cropY = draftCropY
        cropWidth = draftCropWidth
        cropHeight = draftCropHeight
        cropMode = false
        commitCheckpoint(previous)
        editsChanged()
    }

    function cancelCrop() {
        if (!cropMode)
            return
        cropMode = false
        editsChanged()
    }

    function editValues() {
        const values = snapshot()
        // The UI calls this control Blur; native pixel processing calls it Soften.
        values.soften = values.blur
        delete values.blur
        return values
    }

    function exportValues() {
        return {
            "format": exportFormat,
            "scale": exportScale,
            "quality": exportQuality
        }
    }
}
