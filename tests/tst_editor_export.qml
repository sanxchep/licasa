import QtQuick
import QtTest
import "../qml"

Item {
    width: 1200; height: 1200
    Component {
        id: fixture
        Item {
            width: 380; height: 3200
            property alias session: session
            property alias content: content
            EditorSession { id: session }
            EditorExportContent {
                id: content
                width: parent.width
                exportFormats: testEditor.formats()
                exportFormat: session.exportFormat
                exportScale: session.exportScale
                exportQuality: session.exportQuality
                exportSettings: session.exportSettings
                sourceFormat: "png"
                unscaledWidth: 997; unscaledHeight: 613
                outputWidth: Math.max(1, Math.round(997 * exportScale))
                outputHeight: Math.max(1, Math.round(613 * exportScale))
                saveStatus: ""; buttonMotionDuration: 0
                onExportFormatRequested: function(v) { session.exportFormat = v }
                onExportScaleRequested: function(v) { session.exportScale = v }
                onExportQualityRequested: function(v) { session.exportQuality = v; session.setExportSetting("quality", v) }
                onSettingRequested: function(k, v) { session.setExportSetting(k, v) }
            }
        }
    }
    TestCase {
        name: "EditorExportSettings"
        when: windowShown
        function child(item, name) {
            if (item.objectName === name && item.visible) return item
            for (let i = 0; i < item.children.length; ++i) {
                const found = child(item.children[i], name)
                if (found) return found
            }
            return null
        }
        function create(format) {
            const f = createTemporaryObject(fixture, parent)
            verify(f); f.session.exportFormat = format
            wait(0)
            return f
        }
        function choose(f, name, value) {
            const box = child(f, name); verify(box, name)
            let target = -1
            for (let i = 0; i < box.model.length; ++i) if (box.model[i].value === value) target = i
            verify(target >= 0, name + ": " + value)
            // A real keyboard activation; assigning currentIndex alone does not
            // run the production onActivated binding.
            box.currentIndex = target === 0 ? 1 : target - 1
            box.forceActiveFocus()
            keyClick(target === 0 ? Qt.Key_Up : Qt.Key_Down)
            wait(0)
        }
        function toggle(f, name, expected) {
            const control = child(f, name); verify(control, name)
            verify(testEditor.accessible(control).name.length > 0)
            verify(testEditor.press(control)); wait(0)
            compare(child(f, name).checked, expected, name)
        }
        function number(f, name, value) {
            const field = child(f, name); verify(field, name)
            field.forceActiveFocus(); field.selectAll()
            keyClick(Qt.Key_Backspace)
            for (let i = 0; i < value.length; ++i) keyClick(Qt.Key_0 + Number(value[i]))
            keyClick(Qt.Key_Enter)
            return field
        }
        function test_everyFormatSettingsAndRestore_data() {
            return testEditor.formats().map(function(f) { return {tag:f.key, format:f.key} })
        }
        function test_exportActionUsesAvailableSpaceWithoutOverlappingSettings_data() {
            return testEditor.formats().map(function(f) { return {tag:f.key, format:f.key} })
        }
        function test_exportActionUsesAvailableSpaceWithoutOverlappingSettings(data) {
            const f = create(data.format), content = f.content
            const action = child(f, "editorExportButton")
            const settings = child(f, "editorExportSettings")
            const natural = content.height
            content.minimumHeight = natural + 240
            tryCompare(content, "height", natural + 240)
            fuzzyCompare(action.mapToItem(content, 0, action.height).y, content.height, 1)
            verify(action.mapToItem(content, 0, 0).y >= settings.height + 240)
            content.saveStatus = "Ready to export your image."
            wait(0)
            fuzzyCompare(action.mapToItem(content, 0, action.height).y, content.height, 1)
            content.minimumHeight = 1
            wait(0)
            verify(content.height > content.minimumHeight)
            verify(action.mapToItem(content, 0, 0).y >= settings.height + 12)
            fuzzyCompare(action.mapToItem(content, 0, action.height).y, content.height, 1)
        }
        function test_everyFormatSettingsAndRestore(data) {
            const f = create(data.format), format = data.format
            if (["png", "tiff"].indexOf(format) >= 0) {
                for (const v of format === "png" ? [0, 100, 50] : [0, 50]) {
                    choose(f, "editorExport_compression", v); compare(f.session.exportValues().compression, v)
                }
            }
            if (["png", "bmp", "jpeg", "tiff"].indexOf(format) >= 0) {
                choose(f, "editorExport_colorMode", 1); compare(f.session.exportValues().colorMode, 1)
                if (format === "png" || format === "bmp") {
                    choose(f, "editorExport_colorMode", 2)
                    for (const count of [8, 64, 216]) { choose(f, "editorExport_paletteColors", count); compare(f.session.exportValues().paletteColors, count) }
                    toggle(f, "editorExport_ditherToggle", false)
                    compare(f.session.exportValues().dither, false)
                }
            }
            if (format === "jpeg") {
                toggle(f, "editorExport_progressiveToggle", true); compare(f.session.exportValues().progressive, true)
                toggle(f, "editorExport_optimizedToggle", false); compare(f.session.exportValues().optimized, false)
            }
            if (format === "webp") {
                verify(child(f, "editorExport_qualitySlider"))
                toggle(f, "editorExport_losslessToggle", true)
                verify(!child(f, "editorExport_qualitySlider")); compare(f.session.exportValues().lossless, true)
                toggle(f, "editorExport_losslessToggle", false)
                verify(child(f, "editorExport_qualitySlider"))
            }
            if (["jpeg", "webp"].indexOf(format) >= 0) {
                const row = child(f, "editorExport_qualitySlider")
                number(row, "adjustmentValueInput", "71")
                compare(f.session.exportValues().quality, 71)
            }
            if (["pbm", "pgm", "ppm"].indexOf(format) >= 0) {
                choose(f, "editorExport_ascii", true); compare(f.session.exportValues().ascii, true)
                choose(f, "editorExport_ascii", false); compare(f.session.exportValues().ascii, false)
            }
            if (["pbm", "wbmp", "xbm", "xpm"].indexOf(format) >= 0) {
                number(child(f, "editorExport_thresholdSlider"), "adjustmentValueInput", "173")
                compare(f.session.exportValues().threshold, 173)
                toggle(f, "editorExport_ditherToggle", false); compare(f.session.exportValues().dither, false)
                if (format !== "xpm") { toggle(f, "editorExport_invertToggle", true); compare(f.session.exportValues().invert, true) }
                else { choose(f, "editorExport_paletteColors", 8); compare(f.session.exportValues().paletteColors, 8) }
            }
            if (["ico", "cur", "icns"].indexOf(format) >= 0) {
                choose(f, "editorExportIconSize", 256); compare(f.session.exportValues().iconSize, 256)
                choose(f, "editorExportIconCrop", true); compare(f.session.exportValues().iconCrop, true)
                if (format === "cur") {
                    number(f, "editorExport_hotspotXNumber", "255"); number(f, "editorExport_hotspotYNumber", "254")
                    compare(f.session.exportValues().hotspotX, 255); compare(f.session.exportValues().hotspotY, 254)
                    choose(f, "editorExportIconSize", 16)
                    compare(f.session.exportValues().hotspotX, 15); compare(f.session.exportValues().hotspotY, 15)
                    compare(child(f, "editorExport_hotspotYNumber").maximum, 15)
                }
            } else {
                number(f, "editorExportHeight", "307")
                compare(f.content.outputHeight, 307); compare(f.content.outputWidth, 499)
                number(f, "editorExportHeight", "0"); compare(f.content.outputHeight, 6)
                number(f, "editorExportHeight", "99999"); compare(f.content.outputHeight, 1226)
            }
            if (["png", "webp", "tiff", "ico", "cur", "icns", "xpm"].indexOf(format) >= 0) {
                verify(!child(f, "editorExport_matte"))
                toggle(f, "editorExport_keepAlphaToggle", false); compare(f.session.exportValues().keepAlpha, false)
            }
            choose(f, "editorExport_matte", "#000000"); compare(f.session.exportValues().matte, "#000000")
            choose(f, "editorExport_matte", "#ffffff"); compare(f.session.exportValues().matte, "#ffffff")
            verify(testEditor.press(child(f, "editorExportAdvanced")))
            toggle(f, "editorExportNearest", true); compare(f.session.exportValues().nearest, true)
            if (["png", "jpeg", "tiff"].indexOf(format) >= 0) { toggle(f, "editorExportSrgb", true); compare(f.session.exportValues().srgb, true) }
            else verify(!child(f, "editorExportSrgb"))
            if (["png", "jpeg", "tiff", "bmp"].indexOf(format) >= 0) {
                number(f, "editorExportDpi", "300"); compare(f.session.exportValues().dpi, 300)
            } else verify(!child(f, "editorExportDpi"))
            const expected = JSON.stringify(f.session.exportValues())
            choose(f, "editorExportFormat", format === "png" ? "jpeg" : "png")
            choose(f, "editorExportFormat", format)
            compare(JSON.stringify(f.session.exportValues()), expected)
            const requested = createTemporaryObject(spyComponent, this, {target:f.content, signalName:"exportRequested"})
            const action = child(f, "editorExportButton")
            verify(testEditor.press(action)); compare(requested.count, 1)
            f.content.saveBusy = true; verify(testEditor.press(action)); compare(requested.count, 1)
            f.content.saveBusy = false; f.content.hasImage = false
            verify(testEditor.press(action)); compare(requested.count, 1)
        }
        function test_sameAsSourceUsesEveryFormat_data() { return test_everyFormatSettingsAndRestore_data() }
        function test_sameAsSourceUsesEveryFormat(data) {
            const f = create("original")
            f.content.sourceFormat = data.format
            compare(f.content.effectiveFormat, data.format)
            compare(f.content.icon, ["ico", "cur", "icns"].indexOf(data.format) >= 0)
            compare(f.content.mono, ["pbm", "wbmp", "xbm"].indexOf(data.format) >= 0)
            compare(f.session.exportValues().format, "original")
            verify(child(f, "editorExportButton").text.indexOf(data.format.toUpperCase()) >= 0 || data.format === "webp")
        }
    }
    Component { id: spyComponent; SignalSpy {} }
}
