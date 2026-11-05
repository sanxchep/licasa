/*!
    FullscreenSideMenu.qml
    ----------------------
    Fullscreen settings drawer for Licasa.

    Author attribution: Sanxchep
*/

import QtQuick
import "components"

Item {
    id: root

    required property bool fullscreen

    property bool panelOpen: false
    property real cornerControlSize: 48
    property real cornerControlMargin: 24
    property bool infoOverlayEnabled: true
    property bool smoothMotionEnabled: true
    property bool launcherBlocked: false
    readonly property bool panelVisible: panelOpen || panel.revealProgress > 0.001
    property bool startInFullscreenEnabled: true
    property var backgroundModeManager: null
    property var languageManager: null
    property string appVersion: ""
    property string releaseDate: ""
    property bool installedAsSnap: false

    property real fullscreenBackgroundOpacity: 0.70
    property bool transparencyCheckerboardEnabled: false
    property real transparencyCheckerboardOpacity: 0.70
    property bool windowedTransparencyCheckerboardEnabled: false
    property real windowedTransparencyOpacity: 0.70
    property bool animatedImagePlaybackEnabled: true
    property real animationSpeed: 1.0
    property bool smoothImageScalingEnabled: true
    property bool mipmapImageScalingEnabled: true
    property bool pixelAlignedRenderingEnabled: true
    property bool fullResolutionRenderingEnabled: true
    property bool colorManagedRenderingEnabled: true
    property int maximumImageMemoryMiB: 8192
    property int minimumImageMemoryMiB: 384
    property int maximumImageMemoryLimitMiB: 32768
    property real currentImageMegapixels: 0.0
    property bool currentImageLimitedToPreview: false
    property real cropShieldOpacity: 0.70
    property bool advancedOpen: false
    property alias scrollPosition: settingsFlick.contentY

    readonly property bool pointerInsidePanel: root.panelOpen
        && (panelHover.hovered || settingsScrollBar.dragging)

    signal infoOverlayChanged(bool enabled)
    signal smoothMotionChanged(bool enabled)
    signal startInFullscreenChanged(bool enabled)
    signal fullscreenBackgroundOpacityRequested(real value)
    signal transparencyCheckerboardChanged(bool enabled)
    signal transparencyCheckerboardOpacityRequested(real value)
    signal windowedTransparencyCheckerboardChanged(bool enabled)
    signal windowedTransparencyOpacityRequested(real value)
    signal animatedImagePlaybackChanged(bool enabled)
    signal animationSpeedRequested(real value)
    signal smoothImageScalingChanged(bool enabled)
    signal mipmapImageScalingChanged(bool enabled)
    signal pixelAlignedRenderingChanged(bool enabled)
    signal fullResolutionRenderingChanged(bool enabled)
    signal colorManagedRenderingChanged(bool enabled)
    signal maximumImageMemoryMiBRequested(int value)
    signal cropShieldOpacityRequested(real value)
    signal keyboardShortcutsRequested()
    signal quitRequested()
    signal externalUrlRequested(url targetUrl)

    readonly property url sourceRepositoryUrl: "https://github.com/sanxchep/licasa"
    readonly property url issueReportUrl: "https://github.com/sanxchep/licasa/issues/new"

    readonly property int panelMotionDuration: smoothMotionEnabled ? 220 : 0
    readonly property int scrimMotionDuration: smoothMotionEnabled ? 180 : 0
    readonly property int switchMotionDuration: smoothMotionEnabled ? 160 : 0
    readonly property int buttonMotionDuration: smoothMotionEnabled ? 140 : 0

    function scrollSettingsByWheel(wheel) {
        const maximumContentY = Math.max(
            0,
            settingsFlick.contentHeight - settingsFlick.height
        )
        const pixelDelta = wheel.pixelDelta.y
        const scrollDelta = pixelDelta !== 0
            ? pixelDelta
            : wheel.angleDelta.y / 120.0 * 72.0

        if (scrollDelta !== 0) {
            settingsFlick.contentY = Math.max(
                0,
                Math.min(maximumContentY, settingsFlick.contentY - scrollDelta)
            )
        }
        wheel.accepted = true
    }

    function restoreScrollPosition(position) {
        settingsFlick.contentY = Math.max(0, Math.min(
            Math.max(0, settingsFlick.contentHeight - settingsFlick.height), position))
    }

    function scrollByPage(direction) {
        settingsFlick.contentY = Math.max(0, Math.min(
            Math.max(0, settingsFlick.contentHeight - settingsFlick.height),
            settingsFlick.contentY + direction * settingsFlick.height * 0.8
        ))
    }

    function revealFocusedItem(item) {
        let ancestor = item
        while (ancestor && ancestor !== settingsFlick)
            ancestor = ancestor.parent
        if (!ancestor || !item)
            return
        const position = item.mapToItem(settingsFlick.contentItem, 0, 0)
        const top = position.y
        const bottom = top + item.height
        const target = top < settingsFlick.contentY
            ? top - 8 : bottom > settingsFlick.contentY + settingsFlick.height
                ? bottom - settingsFlick.height + 8 : settingsFlick.contentY
        settingsFlick.contentY = Math.max(0, Math.min(
            Math.max(0, settingsFlick.contentHeight - settingsFlick.height), target))
    }

    function estimatedLargeImageMemoryText() {
        const gibibytes = maximumImageMemoryMiB / 1024.0
        return qsTr("Estimated per-image budget: ") + gibibytes.toFixed(2) + qsTr(" GiB")
    }

    function currentImageMegapixelsText() {
        const decimals = currentImageMegapixels < 100.0 ? 1 : 0
        return currentImageMegapixels.toFixed(decimals) + qsTr(" MP")
    }

    // Controls consume their own arrows first (sliders, fields and tool tabs).
    // An otherwise unhandled arrow dismisses the drawer without browsing/zooming.
    Keys.priority: Keys.AfterItem
    Keys.onPressed: function(event) {
        if (!root.panelOpen || event.modifiers !== Qt.NoModifier
                || [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down].indexOf(event.key) < 0)
            return
        event.accepted = true
        root.panelOpen = false
    }

    anchors.fill: parent
    visible: fullscreen || panelOpen
    enabled: visible

    onFullscreenChanged: {
        if (!fullscreen)
            panelOpen = false
    }
    // Keep the shell cheap at startup; preserve controls after their first use.
    function ensureContents() {
        if (settingsContent.status === Loader.Null)
            settingsContent.setSource(Qt.resolvedUrl("FullscreenSettingsContent.qml"), { menu: root })
    }
    Component.onCompleted: { if (panelOpen) ensureContents() }

    onPanelOpenChanged: {
        if (panelOpen) {
            if (launcherBlocked) {
                panelOpen = false
                return
            }
            ensureContents()
            closeButton.forceActiveFocus()
        } else if (fullscreen && !launcherBlocked)
            menuButton.forceActiveFocus()
    }

    Rectangle {
        id: scrim
        objectName: "settingsScrim"
        anchors.fill: parent
        color: "#66000000"
        visible: root.panelOpen || opacity > 0
        opacity: root.panelOpen ? 1 : 0

        Behavior on opacity {
            NumberAnimation {
                duration: root.scrimMotionDuration
                easing.type: Easing.OutCubic
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: root.panelOpen
            onClicked: root.panelOpen = false
        }
    }

    RoundIconButton {
        id: menuButton
        objectName: "settingsMenuButton"
        visible: root.fullscreen && !root.panelOpen && hideProgress < 1
        enabled: !root.launcherBlocked
        Accessible.ignored: root.launcherBlocked
        property real hideProgress: root.launcherBlocked ? 1 : 0
        Behavior on hideProgress {
            NumberAnimation { duration: root.panelMotionDuration; easing.type: Easing.OutCubic }
        }
        width: root.cornerControlSize
        height: width
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: root.cornerControlMargin
            - (root.cornerControlMargin + width + 8) * hideProgress
        anchors.bottomMargin: root.cornerControlMargin
        iconName: "settings"
        accessibleName: root.panelOpen ? qsTr("Close settings") : qsTr("Open settings")
        checkable: true
        checked: root.panelOpen
        animationDuration: root.buttonMotionDuration
        onClicked: root.panelOpen = !root.panelOpen
    }

    Rectangle {
        id: panel
        objectName: "settingsPanel"
        enabled: root.panelOpen
        visible: root.panelOpen || revealProgress > 0.001
        width: Math.min(400, Math.max(280, root.width - 40))
        height: root.height - 40
        y: 20
        // Keep geometry changes separate from the drawer motion. A fullscreen
        // resize must place a closed drawer offscreen immediately.
        property real revealProgress: root.panelOpen ? 1 : 0
        x: root.width + 24 - (width + 44) * revealProgress
        radius: 28
        color: "#FA1A1A1A"
        border.width: 1
        border.color: "#30FFFFFF"
        antialiasing: true

        Behavior on revealProgress {
            NumberAnimation {
                duration: root.panelMotionDuration
                easing.type: Easing.OutCubic
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: function(wheel) { root.scrollSettingsByWheel(wheel) }
        }

        HoverHandler { id: panelHover }

        Item {
            anchors.fill: parent
            anchors.margins: 20

            Text {
                id: titleText
                x: 0
                anchors.verticalCenter: closeButton.verticalCenter
                text: qsTr("Settings")
                color: "white"
                font.pixelSize: 22
                font.weight: Font.DemiBold
            }

            RoundIconButton {
                id: closeButton
                objectName: "settingsCloseButton"
                width: 36
                height: 36
                anchors.top: parent.top
                anchors.right: parent.right
                iconName: "close"
                accessibleName: qsTr("Close settings")
                baseColor: "#2B2B2B"
                hoverColor: "#393939"
                animationDuration: root.buttonMotionDuration
                onClicked: root.panelOpen = false
            }

            Rectangle {
                id: divider
                x: 0
                y: Math.max(titleText.height, closeButton.height) + 16
                width: parent.width
                height: 1
                color: "#18FFFFFF"
            }

            Flickable {
                id: settingsFlick
                objectName: "settingsFlick"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: divider.bottom
                anchors.topMargin: 16
                anchors.bottom: parent.bottom
                clip: true
                contentWidth: width
                contentHeight: settingsContent.height
                boundsBehavior: Flickable.StopAtBounds

                Loader {
                    id: settingsContent
                    objectName: "settingsContent"
                    width: parent.width
                    height: item ? (item as Item).implicitHeight : 0
                }
            }

            InteractiveScrollBar {
                id: settingsScrollBar

                flickable: settingsFlick
                width: 16
                z: 20
                anchors.right: parent.right
                anchors.rightMargin: -19
                y: settingsFlick.y
                height: settingsFlick.height
                minimumHandleHeight: 42
            }
        }
    }
}
