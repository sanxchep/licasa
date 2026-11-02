import QtQuick
import QtTest
import "../qml"

Item {
    id: scene
    width: 640
    height: 600

    Component {
        id: drawerComponent
        FullscreenSideMenu {
            width: scene.width
            height: scene.height
            fullscreen: false
            smoothMotionEnabled: false
            languageManager: testLanguageManager
        }
    }

    TestCase {
        name: "Localization"
        when: windowShown

        function init() {
            verify(testLanguageManager.selectLanguage("en"))
        }

        function cleanup() {
            verify(testLanguageManager.selectLanguage("en"))
        }

        function openDrawer(width) {
            const drawer = createTemporaryObject(drawerComponent, scene, {width: width})
            verify(drawer)
            compare(findChild(drawer, "settingsContent").item, null)
            drawer.panelOpen = true
            verify(findChild(drawer, "settingsContent").item)
            return drawer
        }

        function test_languageSettingUsesRealCatalogAndAccessibleAnnotations() {
            const drawer = openDrawer(640)
            const choice = findChild(drawer, "languageChoice")
            const error = findChild(drawer, "languageSettingError")
            verify(choice)
            compare(findChild(drawer, "languageSettingAnnotation"), null)
            verify(!error.visible)
            compare(choice.currentValue, "en")
            compare(choice.Accessible.name, "Language")
            verify(choice.Accessible.description.indexOf("immediately") >= 0)
            verify(testLanguageManager.selectLanguage("sv"))
            compare(choice.currentValue, "sv")
            compare(choice.Accessible.name, "Språk")
            compare(choice.displayText, "Svenska")
            verify(choice.Accessible.description.indexOf("direkt") >= 0)
            verify(!error.visible)
            verify(testLanguageManager.selectLanguage("system"))
            compare(choice.currentValue, "system")
            compare(choice.displayText, "System default")
        }

        function test_keyboardSelectionAndEscapeKeepDrawerState() {
            const drawer = openDrawer(640)
            const choice = findChild(drawer, "languageChoice")
            drawer.maximumImageMemoryMiB = 4096
            const contents = findChild(drawer, "settingsContent").item
            choice.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryCompare(choice.popup, "visible", true)
            keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Return)
            compare(testLanguageManager.selectedLanguage, "sv")
            compare(choice.currentValue, "sv")
            compare(drawer.panelOpen, true)
            compare(drawer.maximumImageMemoryMiB, 4096)
            compare(findChild(drawer, "settingsContent").item, contents)
            verify(choice.activeFocus)
            keyClick(Qt.Key_Space)
            tryCompare(choice.popup, "visible", true)
            keyClick(Qt.Key_Escape)
            tryCompare(choice.popup, "visible", false)
            compare(drawer.panelOpen, true)
            verify(choice.activeFocus)
        }

        function test_languageSettingFitsNarrowDrawerWithoutSubtitle() {
            const drawer = openDrawer(320)
            const choice = findChild(drawer, "languageChoice")
            const title = findChild(drawer, "languageSettingTitle")
            verify(testLanguageManager.selectLanguage("sv"))
            compare(findChild(drawer, "languageSettingAnnotation"), null)
            verify(choice.y >= title.y + title.height)
            verify(choice.width <= findChild(drawer, "settingsPanel").width)
            verify(choice.height >= 40)
            verify(choice.visible)
        }

        function test_languageFailureRemainsVisibleAndAccessible() {
            const drawer = openDrawer(320)
            const choice = findChild(drawer, "languageChoice")
            const error = findChild(drawer, "languageSettingError")
            verify(!testLanguageManager.selectLanguage("not-offered"))
            verify(error.visible)
            verify(error.text.length > 0)
            verify(choice.Accessible.description.indexOf(error.text) >= 0)
            compare(choice.currentValue, "en")
            verify(testLanguageManager.selectLanguage("en"))
            verify(!error.visible)
        }
    }
}
