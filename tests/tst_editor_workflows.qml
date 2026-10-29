import QtQuick
import QtTest
import "../qml"
import "support"

Item {
    width: 1400
    height: 1100
    Component { id: windowComponent; EditorWindow {} }
    Component { id: sessionComponent; EditorSession {} }
    Component { id: spyComponent; SignalSpy {} }
    TestCase {
        name: "EditorWorkflows"
        when: windowShown
        property var windows: []
        function cleanup() {
            tryCompare(testImageSaveService, "busy", false, 10000)
            for (const main of windows) { main.clearImageState(); main.destroy() }
            windows = []
            testPreviewRequests.reset()
        }
        function window(realFiles) {
            const main = windowComponent.createObject(null, { realFiles: !!realFiles })
            verify(main)
            windows.push(main)
            main.smoothMenuMotionEnabled = false
            main.viewerPreferences.fullResolutionRenderingEnabled = false
            const viewport = findChild(main, "imageViewport")
            if (!realFiles) {
                viewport.sourceOverride = "image://previewtest/editor-workflow?licasa_stage=full"
                main.currentImageUrl = "file:///fixture.png"
                main.residentImageUrl = "file:///fixture.png"
                main.naturalImageSize = Qt.size(640, 480)
            }
            main.width = 1400; main.height = 1100
            main.show(); main.requestActivate()
            tryCompare(main, "active", true)
            main.editPanelOpen = true
            return main
        }
        function input(main, name) {
            const row = findChild(main, "editor" + name + "Control")
            verify(row)
            const field = findChild(row, "adjustmentValueInput")
            verify(field)
            return field
        }
        function type(main, field, value) {
            field.forceActiveFocus()
            tryCompare(main, "activeFocusItem", field)
            field.selectAll()
            testEditor.key(main, Qt.Key_Backspace)
            testEditor.text(main, value)
            compare(field.text, value)
        }
        function spy(target, name) {
            return createTemporaryObject(spyComponent, this, { target: target, signalName: name })
        }
        function buttonNamed(item, text) {
            if (item.visible && item.text === text && typeof item.activate === "function") return item
            if (item.children) for (let i = 0; i < item.children.length; ++i) {
                const found = buttonNamed(item.children[i], text)
                if (found) return found
            }
            return null
        }
        function test_realCropPresetPixels_data() {
            const rows = []
            for (const turn of [0, 1]) for (const pair of [["1:1",1],["4:3",4/3],["3:2",3/2],["4:5",4/5],["16:9",16/9],["Original",0]])
                rows.push({tag:pair[0]+"-turn"+turn,label:pair[0],ratio:pair[1],turn:turn})
            return rows
        }
        function test_realCropPresetPixels(data) {
            const source = testEditor.image("crop-" + data.label.replace(":", "-") + data.turn + ".png")
            const digest = testEditor.digest(source), main = realWindow(source)
            main.editorQuarterTurns = data.turn
            const panel = findChild(main, "editorMenuButton").parent
            panel.activateTool("crop")
            const button = buttonNamed(main.contentItem, data.label)
            verify(button, data.label); verify(testEditor.press(button))
            verify(main.editorCropMode)
            main.applyDraftCrop()
            const destination = testEditor.destination("crop-output.png")
            const expected = main.editorEditValues()
            main.saveEditedImage(destination)
            tryCompare(testImageSaveService, "busy", false, 10000)
            verify(testEditor.matchesEdits(source, destination, expected))
            const decoded = testEditor.decodedInfo(destination)
            verify(decoded.valid)
            const w = data.turn ? 61 : 97, h = data.turn ? 97 : 61
            const ratio = data.ratio || w/h
            const idealW = Math.min(w,h*ratio), idealH = Math.min(h,w/ratio)
            verify(Math.abs(decoded.width-idealW) <= 2 && Math.abs(decoded.height-idealH) <= 2)
            compare(testEditor.digest(source), digest)
        }
        function test_visibleLooksReachTheirOwnModel_data() {
            return [{tag:"Mono",label:"Mono",key:"saturation",value:-1}, {tag:"Pop",label:"Pop",key:"exposure",value:.06},
                {tag:"Vivid",label:"Vivid",key:"vibrance",value:.32}, {tag:"Cinema",label:"Cinema",key:"vignette",value:.22},
                {tag:"Soft",label:"Soft",key:"soften",value:.08}, {tag:"Original",label:"Original",key:"exposure",value:0}]
        }
        function test_visibleLooksReachTheirOwnModel(data) {
            const main = window(false)
            main.beginEditorInteraction(); main.editorExposure = .8; main.finishEditorInteraction()
            findChild(main, "editorMenuButton").parent.activateTool("looks")
            const button = buttonNamed(main.contentItem, data.label)
            verify(button); verify(testEditor.press(button))
            compare(main.editorEditValues()[data.key], data.value)
            main.undoEditorChange(); compare(main.editorExposure, .8)
            main.redoEditorChange(); compare(main.editorEditValues()[data.key], data.value)
        }
        function test_saveShortcutCommitsBeforeConsent() {
            const main = window(false)
            const field = input(main, "Exposure")
            type(main, field, "25")
            testEditor.key(main, Qt.Key_S, Qt.ControlModifier)
            compare(main.editorExposure, .25)
            const editor = findChild(main, "editorMenuButton").parent
            verify(editor.overwriteConfirmationOpen)
            compare(main.imageSaveService.pending, null)
            verify(testEditor.press(findChild(main, "editorOverwriteNoButton")))
            compare(main.imageSaveService.pending, null)
        }
        function test_accessibleCopyCapturesTypedDraft() {
            const main = window(false)
            type(main, input(main, "Exposure"), "25")
            verify(testEditor.press(findChild(main, "editorSaveCopyButton")))
            verify(main.pendingCopyIntent)
            compare(main.pendingCopyIntent.edits.exposure, .25)
        }
        function test_toolActivationCommitsTypedDraft() {
            const main = window(false)
            type(main, input(main, "Exposure"), "25")
            const editor = findChild(main, "editorMenuButton").parent
            editor.activateTool("export")
            compare(main.editorExposure, .25)
        }
        function test_extremeCropIsRejectedWithoutChangingDraft() {
            const session = createTemporaryObject(sessionComponent, this)
            session.beginCrop()
            session.applyCropRatio(1, 1000, 1000)
            const before = [session.draftCropX, session.draftCropY, session.draftCropWidth, session.draftCropHeight]
            compare(session.applyCropRatio(1000, 1000, 1000), false)
            compare([session.draftCropX, session.draftCropY, session.draftCropWidth, session.draftCropHeight], before)
        }
        function realWindow(source) {
            const main = window(true)
            main.imageSaveService = testImageSaveService
            main.loadImageUrl(source, true)
            const viewport = findChild(main, "imageViewport")
            tryCompare(viewport, "imageStatus", Image.Ready, 10000)
            main.editPanelOpen = true
            return main
        }
        function chooseFile(main, destination) {
            const loader = findChild(main, "saveDialogLoader")
            tryVerify(function() { return loader.item !== null }, 5000)
            const dialog = loader.item
            verify(dialog.visible)
            dialog.selectedFile = destination
            dialog.accepted()
            dialog.close()
        }
        function test_everyAdjustmentReachesTheCorrectModelAndSave_data() {
            const rows = []
            for (const name of ["Exposure", "Contrast", "Highlights", "Shadows", "Saturation", "Vibrance", "Warmth", "Tint", "Sharpen", "Soften", "Vignette"])
                rows.push({ tag: name, name: name, key: name === "Soften" ? "soften" : name.toLowerCase(), value: name === "Soften" || name === "Sharpen" || name === "Vignette" ? 13.7 : -13.7 })
            return rows
        }
        function test_everyAdjustmentReachesTheCorrectModelAndSave(data) {
            const main = window(false)
            type(main, input(main, data.name), String(data.value))
            testEditor.key(main, Qt.Key_Enter)
            const edits = main.editorEditValues()
            fuzzyCompare(edits[data.key], data.value / 100, .000001)
            for (const key of ["exposure", "contrast", "highlights", "shadows", "saturation", "vibrance", "warmth", "tint", "sharpen", "soften", "vignette"])
                if (key !== data.key) compare(edits[key], 0)
            testEditor.key(main, Qt.Key_Z, Qt.ControlModifier)
            compare(main.editorEditValues()[data.key], 0)
            testEditor.key(main, Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier)
            fuzzyCompare(main.editorEditValues()[data.key], data.value / 100, .000001)
            main.saveEditedImage("file:///copy.png")
            fuzzyCompare(main.imageSaveService.pending.edits[data.key], data.value / 100, .000001)
            main.imageSaveService.complete("")
        }
        function test_resetAndSourceReplacementRetireDraft_data() {
            return [{ tag: "reset", reset: true }, { tag: "source", reset: false }]
        }
        function test_resetAndSourceReplacementRetireDraft(data) {
            const main = window(false)
            const field = input(main, "Exposure")
            type(main, field, "70")
            if (data.reset) main.resetEditorAdjustments()
            else main.clearEditorSessionState(true)
            compare(main.editorExposure, 0)
            compare(field.dirty, false)
            field.finishEditing(true)
            compare(main.editorExposure, 0)
            main.undoEditorChange()
            compare(main.editorExposure, 0)
        }
        function test_realAccessibleCopySavesTypedPixels() {
            const source = testEditor.image("accessible-source.png")
            const digest = testEditor.digest(source)
            const main = realWindow(source)
            type(main, input(main, "Exposure"), "100")
            verify(testEditor.press(findChild(main, "editorSaveCopyButton")))
            compare(main.pendingCopyIntent.edits.exposure, 1)
            const destination = testEditor.destination("accessible-copy.png")
            chooseFile(main, destination)
            tryCompare(testImageSaveService, "busy", false, 10000)
            verify(testEditor.exists(destination))
            compare(testEditor.pixel(destination, 0, 0), [60, 120, 180, 255])
            compare(testEditor.digest(source), digest)
        }
        function test_copyShortcutCancelAndReopenKeepsSnapshot() {
            const main = window(false)
            type(main, input(main, "Exposure"), "31")
            testEditor.key(main, Qt.Key_S, Qt.ControlModifier | Qt.ShiftModifier)
            verify(main.pendingCopyIntent)
            compare(main.pendingCopyIntent.edits.exposure, .31)
            const loader = findChild(main, "saveDialogLoader")
            tryVerify(function() { return loader.item !== null }, 5000)
            loader.item.rejected(); loader.item.close()
            compare(main.imageSaveService.pending, null)
            main.editorExposure = .7
            main.openSaveAsDialog()
            compare(main.pendingCopyIntent.edits.exposure, .7)
            main.editorExposure = .9
            compare(main.pendingCopyIntent.edits.exposure, .7)
            loader.item.rejected(); loader.item.close()
        }
        function test_realExportEveryFormat_data() {
            return testEditor.formats().map(function(f) { return { tag: f.key, format: f.key, suffix: f.suffix } })
        }
        function test_realExportEveryFormat(data) {
            const source = testEditor.image("export-" + data.format + "-source.png")
            const digest = testEditor.digest(source)
            const main = realWindow(source)
            main.editorExposure = .2
            main.editorExportFormat = data.format
            main.editorExportScale = .5
            findChild(main, "editorMenuButton").parent.activateTool("export")
            verify(testEditor.press(findChild(main, "editorExportButton")))
            const destination = testEditor.destination("workflow-export." + data.suffix)
            chooseFile(main, destination)
            tryCompare(testImageSaveService, "busy", false, 10000)
            verify(testEditor.exists(destination))
            const info = testEditor.decodedInfo(destination)
            verify(info.valid)
            compare(info.width, ["ico", "cur", "icns"].indexOf(data.format) >= 0 ? 64 : 49)
            compare(info.height, ["ico", "cur", "icns"].indexOf(data.format) >= 0 ? 64 : 31)
            compare(String(main.residentImageUrl), String(source))
            compare(main.editorExposure, .2)
            compare(testEditor.digest(source), digest)
        }
        function test_realCopyRetainsEligibleSourceFormat_data() { return test_realExportEveryFormat_data() }
        function test_realCopyRetainsEligibleSourceFormat(data) {
            const source = testEditor.formatImage("copy-source-" + data.format, data.format)
            verify(String(source).length > 0)
            const digest = testEditor.digest(source)
            if (data.format === "icns" && !testEditor.inspect(source).withinBudget) {
                // This reader cannot report ICNS dimensions before decoding. Admission
                // must remain bounded, even though exported ICNS readback succeeds.
                const destination = testEditor.destination("unsafe-icns-copy.icns")
                testImageSaveService.save(source, destination, {exposure:.2}, {format:"original",copyOnly:true}, this)
                tryCompare(testImageSaveService, "busy", false, 10000)
                verify(!testEditor.exists(destination))
                compare(testEditor.digest(source), digest)
                return
            }
            const main = realWindow(source)
            main.editorExposure = .2
            main.editorExportFormat = "jpeg"
            main.editorExportScale = 2
            const destination = testEditor.destination("copy-retained." + data.suffix)
            main.saveEditedImage(destination)
            tryCompare(testImageSaveService, "busy", false, 10000)
            verify(testEditor.exists(destination))
            const originalInfo = testEditor.inspect(source)
            const copyInfo = testEditor.inspect(destination)
            compare(copyInfo.width, originalInfo.width)
            compare(copyInfo.height, originalInfo.height)
            compare(copyInfo.format, originalInfo.format)
            compare(testEditor.digest(source), digest)
        }
        function test_realMultiwindowSaveProtectsPeer_data() {
            return ["overwrite", "copy", "export"].map(function(kind) { return {tag:kind, kind:kind} })
        }
        function test_realMultiwindowSaveProtectsPeer(data) {
            const source = testEditor.image("shared-" + data.kind + ".png")
            const before = testEditor.copy(source, "baseline-" + data.kind + ".png")
            const owner = realWindow(source)
            const peer = realWindow(source)
            owner.editorExposure = .2
            peer.beginEditorInteraction(); peer.editorExposure = .7; peer.finishEditorInteraction()
            if (data.kind === "overwrite") owner.saveCurrentImage()
            else {
                if (data.kind === "export") owner.pendingCopyIntent = { source: String(source), revision: owner.imageRevision, edits:owner.editorEditValues(), options:{format:"png",scale:1,copyOnly:true}, kind:"export" }
                owner.saveEditedImage(testEditor.destination("owner-" + data.kind + ".png"))
            }
            tryCompare(testImageSaveService, "busy", false, 10000)
            compare(peer.editorExposure, .7)
            if (data.kind === "overwrite") {
                verify(peer.editorSourceChanged)
                peer.undoEditorChange(); compare(peer.editorExposure, .7)
            } else {
                peer.undoEditorChange(); compare(peer.editorExposure, 0)
                peer.redoEditorChange(); compare(peer.editorExposure, .7)
            }
            const destination = testEditor.destination("peer-" + data.kind + ".png")
            peer.saveEditedImage(destination)
            tryCompare(testImageSaveService, "busy", false, 10000)
            if (data.kind === "overwrite") {
                verify(!testEditor.exists(destination), "A stale source must not silently receive peer edits a second time")
                verify(peer.editorSaveStatus.indexOf("changed") >= 0)
            } else {
                verify(testEditor.matchesEdits(before, destination, {exposure:.7}))
            }
        }
        function test_realLateSavePreservesNewerEditsAndRetry() {
            const source = testEditor.image("late-source.png")
            const main = realWindow(source)
            main.editorExposure = .2
            const destination = testEditor.destination("late-copy.png")
            main.saveEditedImage(destination)
            main.beginEditorInteraction(); main.editorExposure = .8; main.finishEditorInteraction()
            tryCompare(testImageSaveService, "busy", false, 10000)
            compare(main.editorExposure, .8)
            verify(testEditor.matchesEdits(source, destination, {exposure:.2}))
            const failed = testEditor.destination("missing/fail.png")
            main.saveEditedImage(failed)
            tryCompare(testImageSaveService, "busy", false, 10000)
            compare(main.editorExposure, .8)
            verify(!testEditor.exists(failed))
            main.saveEditedImage(testEditor.destination("retry.png"))
            tryCompare(testImageSaveService, "busy", false, 10000)
            verify(testEditor.exists(testEditor.destination("retry.png")))
        }
        function test_peerOverwriteRetainsUncommittedDraft() {
            const source = testEditor.image("peer-draft-source.png")
            const owner = realWindow(source), peer = realWindow(source)
            owner.editorExposure = .2
            type(peer, input(peer, "Contrast"), "25")
            compare(peer.editorContrast, 0)
            owner.saveCurrentImage()
            tryCompare(testImageSaveService, "busy", false, 10000)
            verify(peer.editorSourceChanged)
            compare(peer.editorContrast, .25)
            compare(input(peer, "Contrast").text, "+25")
            verify(!input(peer, "Contrast").dirty)
            compare(owner.editorContrast, 0)
        }
        function test_estimateFreshnessAndErrorRecovery() {
            const main = window(false)
            const panel = findChild(main, "editorMenuButton").parent
            panel.activateTool("export")
            const service = main.imageSaveService
            tryVerify(function() { return service.estimates.length > 0 }, 2000)
            const old = -main.exportEstimateRevision - 1
            service.sizeEstimateReady(main.residentImageUrl, old, 2048, "", main)
            verify(main.exportEstimateText.indexOf("2.0") >= 0)
            service.sizeEstimateReady(main.residentImageUrl, old, 999999, "", this)
            service.sizeEstimateReady("file:///wrong.png", old, 999999, "", main)
            verify(main.exportEstimateText.indexOf("2.0") >= 0)
            main.editorExportQuality = 72
            main.editorExportScale = .5
            main.editorExportFormat = "jpeg"
            compare(main.exportEstimateText, "Estimating…")
            service.sizeEstimateReady(main.residentImageUrl, old, -1, "stale error", main)
            compare(main.exportEstimateText, "Estimating…"); compare(main.editorSaveStatus, "")
            const current = -main.exportEstimateRevision - 1
            service.sizeEstimateReady(main.residentImageUrl, current, -1, "test estimate failure", main)
            compare(main.exportEstimateText, "Size unavailable"); compare(main.editorSaveStatus, "test estimate failure")
            service.sizeEstimateReady(main.residentImageUrl, current, 4096, "", main)
            compare(main.editorSaveStatus, "")
            verify(main.exportEstimateText.indexOf("4.0") >= 0)
            main.setEditPanelOpen(false)
            const closed = main.exportEstimateText
            service.sizeEstimateReady(main.residentImageUrl, current, 999999, "", main)
            compare(main.exportEstimateText, closed)
            main.setEditPanelOpen(true)
            compare(main.exportEstimateText, "Estimating…")
            service.sizeEstimateReady(main.residentImageUrl, current, 999999, "", main)
            compare(main.exportEstimateText, "Estimating…")
            tryVerify(function() { return service.estimates.length >= 2 }, 2000)
            const last = service.estimates[service.estimates.length - 1]
            compare(last.options.format, "jpeg"); compare(last.options.scale, .5)
            verify(last.owner === main)
            panel.activateTool("adjust")
            service.sizeEstimateReady(main.residentImageUrl, last.revision, 999999, "", main)
            compare(main.exportEstimateText, "")
        }
        function test_clipboardPrecisionAndTextUndo_data() {
            return [
                {tag:"negative-zero", draft:"-0.000", expected:0},
                {tag:"precision", draft:"12.3456", expected:.123},
                {tag:"comma-rejected", draft:"12,5", expected:.2},
                {tag:"overflow-rejected", draft:Array(400).join("9"), expected:.2},
                {tag:"trim", draft:"  -10.5  ", expected:-.105}
            ]
        }
        function test_clipboardPrecisionAndTextUndo(data) {
            const main = window(false), field = input(main, "Exposure")
            main.beginEditorInteraction(); main.editorExposure = .2; main.finishEditorInteraction()
            field.forceActiveFocus(); field.selectAll()
            testEditor.clipboard(data.draft); testEditor.key(main, Qt.Key_V, Qt.ControlModifier)
            compare(field.text, data.draft)
            compare(main.editorExposure, .2)
            testEditor.key(main, Qt.Key_Enter)
            fuzzyCompare(main.editorExposure, data.expected, .000001)
            const a11y = testEditor.accessible(field)
            compare(a11y.name, "Exposure"); compare(a11y.role, 42) // EditableText on both supported Qt versions.
            compare(a11y.value, field.text)
            compare(a11y.description, "-100 to 100%")
            verify(!a11y.invisible && !a11y.disabled)
            type(main, field, "99")
            testEditor.key(main, Qt.Key_Z, Qt.ControlModifier)
            fuzzyCompare(main.editorExposure, data.expected, .000001)
            testEditor.key(main, Qt.Key_Escape)
            fuzzyCompare(main.editorExposure, data.expected, .000001)
        }
        function test_drawerBoundsFocusWheelAndReversal_data() {
            return [{tag:"normal",w:1400,h:1000,smooth:true}, {tag:"short-narrow",w:420,h:480,smooth:false}]
        }
        function test_drawerBoundsFocusWheelAndReversal(data) {
            const main = realWindow(testEditor.image("layout-source.png"))
            main.width = data.w; main.height = data.h
            main.smoothMenuMotionEnabled = data.smooth
            const editor = findChild(main, "editorMenuButton").parent
            const frame = findChild(main, "editorPanel"), scroll = findChild(main, "editorScrollArea")
            tryCompare(frame, "revealProgress", 1)
            verify(findChild(main, "notificationCenter").z < editor.z)
            verify(frame.x >= 0 && frame.x + frame.width <= main.width + 1)
            tryVerify(function() { return frame.y >= 0 && frame.y + frame.height <= main.height + 1 }, 1000, [frame.y,frame.height,main.height].join(","))
            const names = {}
            input(main, "Exposure").forceActiveFocus()
            for (let i = 0; i < 100; ++i) {
                const focus = main.activeFocusItem
                if (focus && focus.objectName === "adjustmentValueInput") {
                    const label = testEditor.accessible(focus).name; names[label] = true
                    const point = focus.mapToItem(scroll, 0, 0)
                    verify(point.y >= -1 && point.y + focus.height <= scroll.height + 1, label + " must scroll into view")
                    verify(focus.width >= 60)
                }
                testEditor.key(main, Qt.Key_Tab)
            }
            for (const label of ["Exposure","Contrast","Highlights","Shadows","Saturation","Vibrance","Warmth","Tint","Sharpen","Soften","Vignette"]) verify(names[label], label + " reachable")
            const exposure = input(main, "Exposure")
            type(main, exposure, "22")
            testEditor.key(main, Qt.Key_Backtab)
            compare(main.editorExposure, .22); verify(main.activeFocusItem !== exposure)
            const zoom = main.currentScale
            scroll.contentY = 0
            mouseWheel(frame, 8, frame.height / 2, 0, 120, Qt.NoButton)
            compare(scroll.contentY, 0); compare(main.currentScale, zoom)
            scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height)
            const bottom = scroll.contentY
            mouseWheel(frame, 8, frame.height / 2, 0, -120, Qt.NoButton)
            compare(scroll.contentY, bottom); compare(main.currentScale, zoom)
            main.requestImageOverwrite()
            verify(editor.overwriteConfirmationOpen)
            compare(main.activeFocusItem.objectName, "editorOverwriteNoButton")
            for (let i = 0; i < 6; ++i) {
                testEditor.key(main, Qt.Key_Tab)
                verify(["editorOverwriteYesButton","editorOverwriteNoButton"].indexOf(main.activeFocusItem.objectName) >= 0)
            }
            waitForRendering(frame)
            verify(testEditor.screenshot(main, data.tag + "-warning"))
            testEditor.key(main, Qt.Key_Escape); verify(!editor.overwriteConfirmationOpen)
            for (const tool of ["crop", "looks", "export", "adjust", "export"]) {
                editor.activateTool(tool); main.setEditPanelOpen(false); main.setEditPanelOpen(true)
                tryCompare(frame, "revealProgress", 1)
                compare(main.activeFocusItem.objectName, "editorToolTab_" + tool)
            }
            tryCompare(findChild(main, "editorFooter"), "visible", false)
            for (let i = 0; i < 80; ++i) {
                testEditor.key(main, Qt.Key_Tab)
                verify(main.activeFocusItem.objectName !== "editorSaveButton" && main.activeFocusItem.objectName !== "editorSaveCopyButton")
            }
            verify(testEditor.press(findChild(main, "editorExportAdvanced")))
            const dpi = findChild(main, "editorExportDpi")
            dpi.forceActiveFocus()
            waitForRendering(frame)
            tryVerify(function() {
                const point = dpi.mapToItem(scroll, 0, 0)
                return point.y >= -1 && point.y + dpi.height <= scroll.height + 1
            }, 1000, "Expanded Advanced must keep the focused DPI field in view")
            verify(testEditor.screenshot(main, data.tag + "-advanced"))
            const action = findChild(main, "editorExportButton")
            action.forceActiveFocus()
            waitForRendering(frame)
            verify(testEditor.screenshot(main, data.tag + "-export-focus"))
            mouseMove(action, action.width / 2, action.height / 2)
            waitForRendering(frame)
            verify(testEditor.screenshot(main, data.tag + "-export-hover"))
        }
        function test_sourceChangedWhileDialogOpenCannotSaveStaleIntent() {
            const source = testEditor.image("dialog-stale.png")
            const main = realWindow(source)
            main.editorExposure = .3
            main.openSaveAsDialog()
            const changed = testEditor.image("dialog-stale.png", 83, 51)
            const digest = testEditor.digest(changed)
            const destination = testEditor.destination("stale-dialog-copy.png")
            chooseFile(main, destination)
            tryCompare(testImageSaveService, "busy", false, 10000)
            verify(!testEditor.exists(destination)); compare(testEditor.digest(source), digest)
            compare(main.editorExposure, .3); verify(main.editorSourceChanged)
            main.loadImageUrl(source, true)
            tryCompare(main, "editorSourceChanged", false)
        }
    }
}
