import QtQuick
import QtTest
import "../qml"
import "support"

Item {
    width: 1400
    height: 1000
    Component { id: windowComponent; EditorWindow {} }
    TestCase {
        name: "KeyboardShortcuts"
        when: windowShown
        property var windows: []

        function cleanup() {
            for (const main of windows) { main.clearImageState(); main.destroy() }
            windows = []
            testPreviewRequests.reset()
            verify(testLanguageManager.selectLanguage("en"))
        }

        function window() {
            const main = windowComponent.createObject(null, {languageManager: testLanguageManager})
            verify(main)
            windows.push(main)
            main.smoothMenuMotionEnabled = false
            main.viewerPreferences.fullResolutionRenderingEnabled = false
            findChild(main, "imageViewport").sourceOverride = "image://previewtest/shortcuts?licasa_stage=full"
            main.currentImageUrl = "file:///fixture.png"
            main.residentImageUrl = "file:///fixture.png"
            main.naturalImageSize = Qt.size(640, 480)
            main.width = 1400; main.height = 1000
            main.show(); main.requestActivate()
            tryCompare(main, "active", true)
            return main
        }

        function open(main) {
            testEditor.key(main, Qt.Key_F1)
            tryCompare(main, "shortcutsVisible", true)
            const help = findChild(main, "keyboardShortcutsDialog")
            verify(help)
            tryVerify(function() { return findChild(main, "closeKeyboardShortcutsButton").activeFocus })
            return help
        }

        function close(main) {
            testEditor.key(main, Qt.Key_Escape)
            tryCompare(main, "shortcutsVisible", false)
            compare(findChild(main, "shortcutsLoader").item, null)
        }

        function test_lazyEntryAndLifecycle() {
            const main = window()
            compare(findChild(main, "shortcutsLoader").item, null)
            compare(main.shortcutsVisible, false)
            const help = open(main)
            compare(main.lifecycleBusyReason, "modal-dialog")
            compare(findChild(main, "viewerScene").enabled, false)
            compare(testEditor.accessible(findChild(main, "shortcutsCard")).name, "Keyboard shortcuts")
            const capture = findChild(main, "shortcutsBackdropCapture")
            if (!help.blurAvailable) compare(capture.sourceItem, null)
            verify(capture.textureSize.width <= 640 && capture.textureSize.height <= 384)
            close(main)
            compare(findChild(main, "viewerScene").enabled, true)
            verify(main.lifecycleBusyReason !== "modal-dialog")
        }

        function test_settingsEntryRestoresScrollFocusAndOneEscapeLayer() {
            const main = window()
            const menu = findChild(main, "settingsMenuButton").parent
            menu.panelOpen = true
            menu.advancedOpen = true
            const button = findChild(main, "keyboardShortcutsButton")
            verify(button)
            button.forceActiveFocus()
            const scroll = findChild(main, "settingsFlick")
            tryVerify(function() {
                menu.revealFocusedItem(button)
                const position = button.mapToItem(scroll.contentItem, 0, 0).y
                return position >= scroll.contentY - 1
                    && position + button.height <= scroll.contentY + scroll.height + 1
            })
            const position = scroll.contentY
            verify(testEditor.press(button))
            tryCompare(main, "shortcutsVisible", true)
            close(main)
            tryCompare(main, "activeFocusItem", button)
            compare(menu.panelOpen, true)
            compare(menu.advancedOpen, true)
            fuzzyCompare(scroll.contentY, position, 1)
            testEditor.key(main, Qt.Key_Escape)
            compare(menu.panelOpen, false)
        }

        function test_pageKeysScrollSettingsAndEveryKeycapIsNamed() {
            const main = window()
            const menu = findChild(main, "settingsMenuButton").parent
            menu.panelOpen = true
            const scroll = findChild(main, "settingsFlick")
            scroll.contentY = 0
            testEditor.key(main, Qt.Key_PageDown)
            verify(scroll.contentY > 0)
            testEditor.key(main, Qt.Key_PageUp)
            fuzzyCompare(scroll.contentY, 0, 1)
            const help = open(main)
            let count = 0
            function inspect(item) {
                if (item.objectName === "shortcutKeycap") {
                    verify(item.displayedKey.length > 0, "Every displayed key must have a readable label")
                    ++count
                }
                if (item.children) for (let i = 0; i < item.children.length; ++i) inspect(item.children[i])
            }
            inspect(help)
            verify(count > 40)
            close(main)
        }

        function test_modalKeysAndPointerCannotReachTheViewer() {
            const main = window()
            main.editPanelOpen = true
            main.editorExposure = .24
            const fullscreen = main.fullScreenMode, source = main.currentImageUrl
            open(main)
            for (const key of [Qt.Key_F11, Qt.Key_O, Qt.Key_S, Qt.Key_Z, Qt.Key_R])
                testEditor.key(main, key, key === Qt.Key_F11 ? Qt.NoModifier : Qt.ControlModifier)
            for (const key of [Qt.Key_Left, Qt.Key_Right, Qt.Key_E, Qt.Key_M, Qt.Key_Y, Qt.Key_F1])
                testEditor.key(main, key)
            compare(main.fullScreenMode, fullscreen)
            compare(main.currentImageUrl, source)
            compare(main.editorExposure, .24)
            compare(main.editPanelOpen, true)
            compare(main.fileDialogVisible, false)
            const help = findChild(main, "keyboardShortcutsDialog")
            mouseClick(help, 2, 2)
            mouseWheel(help, 2, 2, 0, -120, Qt.NoButton)
            compare(main.shortcutsVisible, true)
            compare(main.editorExposure, .24)
            close(main)
            testEditor.key(main, Qt.Key_R, Qt.ControlModifier)
            compare(main.editorExposure, 0)
        }

        function test_focusTrapAndKeyboardScroll() {
            const main = window()
            open(main)
            const button = findChild(main, "closeKeyboardShortcutsButton")
            const focus = findChild(main, "shortcutsListFocus")
            const list = findChild(main, "shortcutsList")
            for (let i = 0; i < 8; ++i) {
                testEditor.key(main, Qt.Key_Tab)
                verify(main.activeFocusItem === button || main.activeFocusItem === focus)
            }
            testEditor.key(main, Qt.Key_Backtab)
            verify(focus.activeFocus)
            testEditor.key(main, Qt.Key_PageDown)
            verify(list.contentY > 0)
            testEditor.key(main, Qt.Key_End)
            fuzzyCompare(list.contentY, list.contentHeight - list.height, 1)
            testEditor.key(main, Qt.Key_Home)
            compare(list.contentY, 0)
            testEditor.key(main, Qt.Key_Tab)
            verify(button.activeFocus)
            testEditor.key(main, Qt.Key_Space)
            tryCompare(main, "shortcutsVisible", false)
        }

        function test_helpPreservesUnfinishedNumericInput() {
            const main = window()
            main.editPanelOpen = true
            const field = findChild(findChild(main, "editorExposureControl"), "adjustmentValueInput")
            field.forceActiveFocus()
            testEditor.key(main, Qt.Key_A, Qt.ControlModifier)
            testEditor.text(main, "37.5")
            compare(field.dirty, true)
            compare(main.editorExposure, 0)
            open(main)
            compare(field.text, "37.5")
            compare(field.dirty, true)
            compare(main.editorExposure, 0)
            close(main)
            tryCompare(main, "activeFocusItem", field)
            compare(field.text, "37.5")
            compare(field.dirty, true)
            testEditor.key(main, Qt.Key_Return)
            compare(main.editorExposure, .375)
        }

        function test_confirmationKeepsPriorityOverHelp() {
            const main = window()
            main.editPanelOpen = true
            main.editorExposure = .1
            main.requestImageOverwrite()
            const editor = findChild(main, "editorMenuButton").parent
            compare(editor.overwriteConfirmationOpen, true)
            testEditor.key(main, Qt.Key_F1)
            compare(main.shortcutsVisible, false)
            compare(editor.overwriteConfirmationOpen, true)
        }

        function test_responsiveTranslatedList_data() {
            return [{tag:"english",language:"en",w:1000,h:850},
                {tag:"swedish-narrow",language:"sv",w:320,h:420},
                {tag:"swedish-short",language:"sv",w:420,h:240}]
        }
        function test_responsiveTranslatedList(data) {
            const main = window()
            verify(testLanguageManager.selectLanguage(data.language))
            main.width = data.w; main.height = data.h
            open(main)
            const card = findChild(main, "shortcutsCard")
            const help = findChild(main, "keyboardShortcutsDialog")
            verify(card.x >= 0 && card.y >= 0)
            verify(card.x + card.width <= main.width && card.y + card.height <= main.height)
            const header = findChild(main, "shortcutsHeader")
            const headerY = header.y
            waitForRendering(card)
            verify(testEditor.screenshot(main, "shortcuts-top-" + data.tag))
            const list = findChild(main, "shortcutsList")
            testEditor.key(main, Qt.Key_End)
            verify(list.contentY > 0)
            compare(header.y, headerY)
            const name = testEditor.accessible(card).name
            compare(name, data.language === "sv" ? "Kortkommandon" : "Keyboard shortcuts")
            waitForRendering(card)
            verify(testEditor.screenshot(main, "shortcuts-" + data.tag))
            verify(testLanguageManager.selectLanguage(data.language === "sv" ? "en" : "sv"))
            compare(testEditor.accessible(card).name, data.language === "sv" ? "Keyboard shortcuts" : "Kortkommandon")
            close(main)
        }

        function test_immediateReopenKeepsTheNewModalFocused() {
            const main = window()
            open(main)
            main.dismissKeyboardShortcuts(true)
            main.openKeyboardShortcuts()
            wait(0)
            verify(findChild(main, "closeKeyboardShortcutsButton").activeFocus)
            close(main)
        }

        function test_floatingAndTinyWindowsKeepCloseReachable() {
            const main = window()
            main.fullScreenMode = false
            main.width = 160; main.height = 140
            open(main)
            compare(main.fullScreenMode, false)
            const help = findChild(main, "keyboardShortcutsDialog")
            const closeButton = findChild(main, "closeKeyboardShortcutsButton")
            const p = closeButton.mapToItem(help, 0, 0)
            verify(p.x >= 0 && p.y >= 0)
            verify(p.x + closeButton.width <= main.width && p.y + closeButton.height <= main.height)
            close(main)
            compare(main.fullScreenMode, false)
            compare(main.visible, true)
        }

        function test_reopenCreatesOnlyTheInvokingWindowsHelp() {
            const first = window()
            const second = window()
            open(second)
            compare(first.shortcutsVisible, false)
            close(second)
            open(second)
            compare(first.shortcutsVisible, false)
            close(second)
        }
    }
}
