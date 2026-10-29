import QtQuick
import QtTest
import "../qml"

TestCase {
    name: "EditorSessionHistory"

    Component {
        id: sessionComponent
        EditorSession {}
    }

    function test_exportSettingsArePerFormatAndResetWithImage() {
        const session = createTemporaryObject(sessionComponent, this)
        session.exportFormat = "png"
        session.setExportSetting("compression", 100)
        session.exportFormat = "jpeg"
        session.setExportSetting("progressive", true)
        session.exportFormat = "png"
        compare(session.exportValues().compression, 100)
        compare(session.exportValues().progressive, undefined)
        session.clear(true)
        compare(Object.keys(session.exportSettings).length, 0)
    }
    function test_everyLookHasIndependentValuesAndUndo_data() {
        return [
            {tag:"Mono", look:"bw", values:{saturation:-1,contrast:.1,highlights:-.08}},
            {tag:"Pop", look:"pop", values:{exposure:.06,contrast:.22,saturation:.18,vibrance:.2,warmth:.1,sharpen:.16}},
            {tag:"Vivid", look:"vivid", values:{contrast:.12,highlights:-.06,shadows:.08,saturation:.12,vibrance:.32,sharpen:.12}},
            {tag:"Cinema", look:"cinematic", values:{exposure:-.04,contrast:.16,highlights:-.16,shadows:.1,saturation:-.1,tint:.08,vignette:.22}},
            {tag:"Soft", look:"soft", values:{exposure:.08,contrast:-.1,highlights:-.08,shadows:.14,saturation:-.05,warmth:.06,blur:.08}},
            {tag:"Original", look:"original", values:{}}
        ]
    }
    function test_everyLookHasIndependentValuesAndUndo(data) {
        const session = createTemporaryObject(sessionComponent, this)
        session.beginInteraction(); session.exposure = .9; session.finishInteraction()
        session.applyPreset(data.look)
        for (const key of ["exposure","contrast","highlights","shadows","saturation","vibrance","warmth","tint","sharpen","blur","vignette"])
            compare(session[key], data.values[key] || 0, data.tag + " " + key)
        const history = session.undoStack.length
        session.applyPreset(data.look); compare(session.undoStack.length, history)
        session.undo(); compare(session.exposure, .9)
        session.redo(); compare(session.exposure, data.values.exposure || 0)
    }
    function test_fixedSeedMixedHistory_data() {
        return [17, 104729, 20261029, 0x5eed].map(function(seed) {return {tag:String(seed), seed:seed}})
    }
    function test_fixedSeedMixedHistory(data) {
        const session = createTemporaryObject(sessionComponent, this)
        let seed = data.seed, current = [0,0,0,false], back = [], forward = []
        let undos = 0, redos = 0
        const actions = {}
        // Use the generator's high bits. Its low three bits cycle predictably
        // and can correlate reset/undo with empty history.
        function random() { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed >>> 8 }
        for (let step = 0; step < 120; ++step) {
            // Start each seed with a real edit/undo/redo, then mix all actions.
            const action = step < 3 ? [0,4,5][step] : random() % 8, old = current.slice()
            actions[action] = true
            if (action === 0 || action === 1) {
                const value = step === 0 ? .4 : (random() % 21 - 10) / 10
                session.beginInteraction(); session.setAdjustment(action === 0 ? "exposure" : "contrast", value); session.finishInteraction()
                current[action] = value
            } else if (action === 2) { session.rotateRight(); current[2] = (current[2] + 1) % 4 }
            else if (action === 3) { session.toggleFlipHorizontal(); current[3] = !current[3] }
            else if (action === 4) {
                session.undo()
                if (back.length) { undos++; forward.push(current); current = back.pop() }
            } else if (action === 5) {
                session.redo()
                if (forward.length) { redos++; back.push(current); current = forward.pop() }
            } else if (action === 6) { session.reset(); current = [0,0,0,false] }
            else { session.clear(true); current = [0,0,0,false]; back = []; forward = [] }
            if (action <= 3 || action === 6) {
                if (JSON.stringify(old) !== JSON.stringify(current)) { back.push(old); if (back.length > 50) back.shift(); forward = [] }
            }
            const context = "seed=" + data.seed + " step=" + step + " action=" + action
            compare([session.exposure, session.contrast, session.quarterTurns, session.flipHorizontal], current, context)
            compare(session.canUndo, back.length > 0, context); compare(session.canRedo, forward.length > 0, context)
            compare(session.cropWidth, 1, context); compare(session.cropHeight, 1, context)
        }
        compare(Object.keys(actions).length, 8, "all action types, seed=" + data.seed)
        verify(undos > 0 && redos > 0, "nonempty undo/redo history, seed=" + data.seed)
    }
    function test_allCropRatiosKeepAcceptedShape_data() {
        const rows = []
        for (const size of [[997,613], [61,97], [1,1], [2,3]])
            for (const ratio of [1,4/3,3/4,3/2,2/3,16/9,9/16,50,.02,1000,.001])
                rows.push({tag:size.join("x") + "-" + ratio, w:size[0],h:size[1],ratio:ratio})
        return rows
    }
    function test_allCropRatiosKeepAcceptedShape(data) {
        const session = createTemporaryObject(sessionComponent, this)
        const normalized = data.ratio / (data.w / data.h)
        const accepted = normalized >= .02 - 1e-10 && normalized <= 50 + 1e-10
        compare(session.applyCropRatio(data.ratio, data.w, data.h), accepted)
        if (!accepted) { verify(!session.cropMode); compare(session.cropWidth, 1); return }
        fuzzyCompare(session.draftCropWidth * data.w / (session.draftCropHeight * data.h), data.ratio, .000001)
        verify(session.draftCropX >= 0 && session.draftCropY >= 0)
        verify(session.draftCropX + session.draftCropWidth <= 1.000001)
        verify(session.draftCropY + session.draftCropHeight <= 1.000001)
        session.applyDraftCrop()
        session.undo(); compare(session.cropWidth, 1)
        session.redo(); fuzzyCompare(session.cropWidth * data.w / (session.cropHeight * data.h), data.ratio, .000001)
    }

    function test_smallerCursorCanvasKeepsHotspotInsideImage() {
        const session = createTemporaryObject(sessionComponent, this)
        session.exportFormat = "cur"
        session.setExportSetting("iconSize", 256)
        session.setExportSetting("hotspotX", 255)
        session.setExportSetting("hotspotY", 99)
        session.setExportSetting("iconSize", 16)
        compare(session.exportValues().hotspotX, 15)
        compare(session.exportValues().hotspotY, 15)
        session.exportFormat = "png"
        session.exportFormat = "cur"
        compare(session.exportValues().hotspotX, 15)
        compare(session.exportValues().hotspotY, 15)
    }

    function test_equalSnapshotsIgnorePropertyOrder() {
        const session = createTemporaryObject(sessionComponent, this)
        const current = session.snapshot()
        const reordered = {}
        for (const key of Object.keys(current).reverse())
            reordered[key] = current[key]
        verify(session.snapshotsEqual(current, reordered))
        session.commitCheckpoint(reordered)
        verify(!session.canUndo)
        reordered.exposure = 0.25
        verify(!session.snapshotsEqual(current, reordered))
    }

    function test_presetPreservesGeometryAndResetIsUndoable() {
        const session = createTemporaryObject(sessionComponent, this)
        session.cropX = 0.2
        session.cropWidth = 0.5
        session.quarterTurns = 1
        session.flipHorizontal = true
        session.applyPreset("soft")
        compare(session.cropX, 0.2)
        compare(session.cropWidth, 0.5)
        compare(session.quarterTurns, 1)
        compare(session.flipHorizontal, true)
        const values = session.editValues()
        compare(values.soften, session.blur)
        verify(values.blur === undefined)
        values.cropX = 0.9
        compare(session.cropX, 0.2)
        session.reset()
        verify(!session.hasAdjustments)
        session.undo()
        compare(session.cropX, 0.2)
        compare(session.blur, 0.08)
        session.redo()
        verify(!session.hasAdjustments)
    }

    function test_historyRemainsBoundedAndNewEditsDiscardRedo() {
        const session = createTemporaryObject(sessionComponent, this)
        for (let i = 1; i <= 60; ++i) {
            session.beginInteraction()
            session.setAdjustment("exposure", i / 100)
            session.finishInteraction()
        }
        compare(session.undoStack.length, 50)
        session.undo()
        verify(session.canRedo)
        session.beginInteraction()
        session.setAdjustment("contrast", 0.4)
        session.finishInteraction()
        verify(!session.canRedo)
        compare(session.undoStack.length, 50)
    }

    function test_clearRetiresDraftInteractionAndCanPreserveExportSettings() {
        const session = createTemporaryObject(sessionComponent, this)
        session.exportFormat = "webp"
        session.exportScale = 0.5
        session.exportQuality = 72
        session.beginInteraction()
        session.setAdjustment("exposure", 0.4)
        session.beginCrop()
        session.updateDraftCrop(0.2, 0.3, 0.5, 0.4)
        session.compareOriginal = true

        session.clear(false)
        session.finishInteraction()
        verify(!session.hasAdjustments)
        verify(!session.cropMode)
        verify(!session.compareOriginal)
        verify(!session.canUndo)
        verify(!session.canRedo)
        compare(session.interactionSnapshot, null)
        compare(session.draftCropX, 0)
        compare(session.draftCropY, 0)
        compare(session.draftCropWidth, 1)
        compare(session.draftCropHeight, 1)
        compare(session.exportFormat, "webp")
        compare(session.exportScale, 0.5)
        compare(session.exportQuality, 72)

        session.clear(true)
        compare(session.exportFormat, "original")
        compare(session.exportScale, 1)
        compare(session.exportQuality, 95)
    }

    function test_rotationAndMirrorCyclesPreserveCropAndHistory() {
        const session = createTemporaryObject(sessionComponent, this)
        session.beginCrop()
        session.updateDraftCrop(0.1, 0.2, 0.4, 0.6)
        session.applyDraftCrop()
        const original = session.snapshot()

        for (let turn = 0; turn < 4; ++turn)
            session.rotateRight()
        fuzzyCompare(session.cropX, original.cropX, 0.000001)
        fuzzyCompare(session.cropY, original.cropY, 0.000001)
        fuzzyCompare(session.cropWidth, original.cropWidth, 0.000001)
        fuzzyCompare(session.cropHeight, original.cropHeight, 0.000001)
        compare(session.quarterTurns, 0)

        session.toggleFlipHorizontal()
        session.toggleFlipHorizontal()
        session.toggleFlipVertical()
        session.toggleFlipVertical()
        fuzzyCompare(session.cropX, original.cropX, 0.000001)
        fuzzyCompare(session.cropY, original.cropY, 0.000001)
        verify(!session.flipHorizontal)
        verify(!session.flipVertical)

        session.rotateLeft()
        session.resetRotation()
        fuzzyCompare(session.cropX, original.cropX, 0.000001)
        fuzzyCompare(session.cropY, original.cropY, 0.000001)
        compare(session.quarterTurns, 0)
        session.undo()
        compare(session.quarterTurns, 3)
        session.redo()
        compare(session.quarterTurns, 0)
    }

    function test_cancelCropPreservesAppliedEditsAndRedo() {
        const session = createTemporaryObject(sessionComponent, this)
        session.applyPreset("soft")
        session.undo()
        verify(session.canRedo)
        session.beginCrop()
        session.updateDraftCrop(-1, 2, Infinity, NaN)
        verify(session.draftCropX >= 0 && session.draftCropX < 1)
        verify(session.draftCropY >= 0 && session.draftCropY < 1)
        verify(session.draftCropWidth > 0)
        verify(session.draftCropHeight > 0)
        verify(session.draftCropX + session.draftCropWidth <= 1)
        verify(session.draftCropY + session.draftCropHeight <= 1)
        session.cancelCrop()
        verify(!session.cropMode)
        verify(!session.hasAdjustments)
        verify(!session.canUndo)
        verify(session.canRedo)
        session.redo()
        compare(session.blur, 0.08)
    }
}
