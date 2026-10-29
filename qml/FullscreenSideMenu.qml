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
        return "Estimated per-image budget: " + gibibytes.toFixed(2) + " GiB"
    }

    function currentImageMegapixelsText() {
        const decimals = currentImageMegapixels < 100.0 ? 1 : 0
        return currentImageMegapixels.toFixed(decimals) + " MP"
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
    onPanelOpenChanged: {
        if (panelOpen) {
            if (launcherBlocked) {
                panelOpen = false
                return
            }
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
        accessibleName: root.panelOpen ? "Close settings" : "Open settings"
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
                text: "Settings"
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
                accessibleName: "Close settings"
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
                contentHeight: settingsColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: settingsColumn
                    width: settingsFlick.width
                    spacing: 12

                    ToggleRow {
                        width: parent.width
                        title: "Info overlay"
                        subtitle: "Show file name and zoom details"
                        checked: root.infoOverlayEnabled
                        buttonMotionDuration: root.buttonMotionDuration
                        switchMotionDuration: root.switchMotionDuration

                        onToggled: function(value) {
                            root.infoOverlayChanged(value)
                        }
                    }

                    ToggleRow {
                        width: parent.width
                        title: "Smooth motion"
                        subtitle: "Animate menu and switches"
                        checked: root.smoothMotionEnabled
                        buttonMotionDuration: root.buttonMotionDuration
                        switchMotionDuration: root.switchMotionDuration

                        onToggled: function(value) {
                            root.smoothMotionChanged(value)
                        }
                    }

                    ToggleRow {
                        width: parent.width
                        title: "Open in fullscreen"
                        subtitle: "Use fullscreen when opening or showing images"
                        checked: root.startInFullscreenEnabled
                        buttonMotionDuration: root.buttonMotionDuration
                        switchMotionDuration: root.switchMotionDuration

                        onToggled: function(value) {
                            root.startInFullscreenChanged(value)
                        }
                    }

                    ToggleRow {
                        id: backgroundServiceRow
                        objectName: "backgroundServiceRow"
                        visible: root.backgroundModeManager
                            && root.backgroundModeManager.supported
                        width: parent.width
                        title: "Background process"
                        subtitle: "Keep a lightweight background process open. Starts at next login."
                        checked: root.backgroundModeManager
                            ? root.backgroundModeManager.enabled : false
                        buttonMotionDuration: root.buttonMotionDuration
                        switchMotionDuration: root.switchMotionDuration

                        onToggled: function(value) {
                            root.backgroundModeManager.setEnabled(value)
                        }
                    }

                    ValueRow {
                        width: parent.width
                        title: "Background opacity"
                        subtitle: "Dark backdrop behind the image in fullscreen"
                        value: root.fullscreenBackgroundOpacity
                        step: 0.05
                        buttonMotionDuration: root.buttonMotionDuration

                        onValueChangedByUser: function(value) {
                            root.fullscreenBackgroundOpacityRequested(value)
                        }
                    }

                    ToggleRow {
                        width: parent.width
                        title: "Full screen transparency grid"
                        subtitle: "Show a checkerboard behind transparent pixels in full screen"
                        checked: root.transparencyCheckerboardEnabled
                        buttonMotionDuration: root.buttonMotionDuration
                        switchMotionDuration: root.switchMotionDuration

                        onToggled: function(value) {
                            root.transparencyCheckerboardChanged(value)
                        }
                    }

                    ValueRow {
                        width: parent.width
                        title: "Grid opacity"
                        subtitle: root.transparencyCheckerboardEnabled
                            ? "Strength of the transparency checkerboard"
                            : "Turn on the full screen grid to adjust"
                        value: root.transparencyCheckerboardOpacity
                        step: 0.05
                        enabled: root.transparencyCheckerboardEnabled
                        opacity: enabled ? 1.0 : 0.7
                        buttonMotionDuration: root.buttonMotionDuration

                        onValueChangedByUser: function(value) {
                            root.transparencyCheckerboardOpacityRequested(value)
                        }
                    }

                    ToggleRow {
                        width: parent.width
                        title: "Floating transparency grid"
                        subtitle: "Show a checkerboard instead of solid backing in floating mode"
                        checked: root.windowedTransparencyCheckerboardEnabled
                        buttonMotionDuration: root.buttonMotionDuration
                        switchMotionDuration: root.switchMotionDuration

                        onToggled: function(value) {
                            root.windowedTransparencyCheckerboardChanged(value)
                        }
                    }

                    ValueRow {
                        width: parent.width
                        title: "Floating transparency opacity"
                        subtitle: root.windowedTransparencyCheckerboardEnabled
                            ? "Strength of the floating checkerboard"
                            : "Strength of the solid backing for transparent pixels"
                        value: root.windowedTransparencyOpacity
                        step: 0.05
                        buttonMotionDuration: root.buttonMotionDuration

                        onValueChangedByUser: function(value) {
                            root.windowedTransparencyOpacityRequested(value)
                        }
                    }

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: "#18FFFFFF"
                    }

                    ActionChip {
                        objectName: "advancedSettingsButton"
                        width: parent.width
                        text: root.advancedOpen
                            ? "Advanced settings  ▲"
                            : "Advanced settings  ▼"
                        checkable: true
                        checked: root.advancedOpen
                        accent: "#34FFFFFF"
                        animationDuration: root.buttonMotionDuration
                        onClicked: root.advancedOpen = !root.advancedOpen
                    }

                    SectionCard {
                        visible: root.advancedOpen
                        width: parent.width
                        title: "Rendering quality"

                        ToggleRow {
                            width: parent.width
                            title: "Smooth scaling"
                            subtitle: "Filter pixels while zooming and fitting"
                            checked: root.smoothImageScalingEnabled
                            buttonMotionDuration: root.buttonMotionDuration
                            switchMotionDuration: root.switchMotionDuration
                            onToggled: function(value) {
                                root.smoothImageScalingChanged(value)
                            }
                        }

                        ToggleRow {
                            width: parent.width
                            title: "Mipmap downscaling"
                            subtitle: "Sharper text and detail when zoomed out"
                            checked: root.mipmapImageScalingEnabled
                            buttonMotionDuration: root.buttonMotionDuration
                            switchMotionDuration: root.switchMotionDuration
                            onToggled: function(value) {
                                root.mipmapImageScalingChanged(value)
                            }
                        }

                        ToggleRow {
                            width: parent.width
                            title: "Pixel-aligned placement"
                            subtitle: "Avoid half-pixel blur at the image origin"
                            checked: root.pixelAlignedRenderingEnabled
                            buttonMotionDuration: root.buttonMotionDuration
                            switchMotionDuration: root.switchMotionDuration
                            onToggled: function(value) {
                                root.pixelAlignedRenderingChanged(value)
                            }
                        }

                        ToggleRow {
                            width: parent.width
                            title: "Full-resolution rendering"
                            subtitle: "Promote previews to the original resolution"
                            checked: root.fullResolutionRenderingEnabled
                            buttonMotionDuration: root.buttonMotionDuration
                            switchMotionDuration: root.switchMotionDuration
                            onToggled: function(value) {
                                root.fullResolutionRenderingChanged(value)
                            }
                        }

                        ToggleRow {
                            width: parent.width
                            title: "Color-managed rendering"
                            subtitle: "Convert embedded profiles to display sRGB"
                            checked: root.colorManagedRenderingEnabled
                            buttonMotionDuration: root.buttonMotionDuration
                            switchMotionDuration: root.switchMotionDuration
                            onToggled: function(value) {
                                root.colorManagedRenderingChanged(value)
                            }
                        }
                    }

                    ValueRow {
                        visible: root.advancedOpen
                        objectName: "maximumImageMemoryControl"
                        width: parent.width
                        title: "Maximum working memory per image"
                        subtitle: root.estimatedLargeImageMemoryText()
                            + "\nImages above this budget open in a bounded preview"
                        from: root.minimumImageMemoryMiB
                        to: root.maximumImageMemoryLimitMiB
                        value: root.maximumImageMemoryMiB
                        step: 128
                        displayMultiplier: 1.0 / 1024.0
                        displayDecimals: 2
                        displaySuffix: " GiB"
                        accessibleUnit: "MiB"
                        buttonMotionDuration: root.buttonMotionDuration
                        onValueChangedByUser: function(value) {
                            root.maximumImageMemoryMiBRequested(
                                Math.max(root.minimumImageMemoryMiB,
                                    root.minimumImageMemoryMiB
                                        + Math.round((value - root.minimumImageMemoryMiB) / 128)
                                            * 128)
                            )
                        }
                    }

                    ValueRow {
                        visible: root.advancedOpen
                        width: parent.width
                        title: "Crop shield opacity"
                        subtitle: "Dim only the area outside the crop frame"
                        value: root.cropShieldOpacity
                        step: 0.05
                        buttonMotionDuration: root.buttonMotionDuration
                        onValueChangedByUser: function(value) {
                            root.cropShieldOpacityRequested(value)
                        }
                    }

                    SectionCard {
                        visible: root.advancedOpen
                        width: parent.width
                        title: "Animated Playback Settings"

                        ToggleRow {
                            width: parent.width
                            title: "Play animated images"
                            checked: root.animatedImagePlaybackEnabled
                            buttonMotionDuration: root.buttonMotionDuration
                            switchMotionDuration: root.switchMotionDuration
                            onToggled: function(value) {
                                root.animatedImagePlaybackChanged(value)
                            }
                        }

                        ValueRow {
                            width: parent.width
                            title: "Playback speed"
                            from: 0.25
                            to: 2.0
                            value: root.animationSpeed
                            step: 0.25
                            displayMultiplier: 100
                            displayDecimals: 0
                            displaySuffix: "%"
                            enabled: root.animatedImagePlaybackEnabled
                            opacity: enabled ? 1.0 : 0.7
                            buttonMotionDuration: root.buttonMotionDuration
                            onValueChangedByUser: function(value) {
                                root.animationSpeedRequested(value)
                            }
                        }
                    }

                    ActionChip {
                        objectName: "quitLicasaButton"
                        width: parent.width
                        text: "Fully Quit Licasa"
                        subtitle: "Closes the viewer and all Licasa background processes."
                        sectionStyle: true
                        destructive: true
                        animationDuration: root.buttonMotionDuration
                        Accessible.description: subtitle + " Shortcut Control Shift Q."
                        onClicked: root.quitRequested()
                    }

                    ActionChip {
                        objectName: "viewSourceButton"
                        width: parent.width
                        text: "View Source"
                        sectionStyle: true
                        animationDuration: root.buttonMotionDuration
                        Accessible.description: "Open the Licasa source repository on GitHub."
                        onClicked: root.externalUrlRequested(root.sourceRepositoryUrl)
                    }

                    ActionChip {
                        objectName: "reportIssueButton"
                        width: parent.width
                        text: "Report a Bug / Fix / Suggestion"
                        sectionStyle: true
                        animationDuration: root.buttonMotionDuration
                        Accessible.description: "Open a new GitHub issue for a bug, fix, or suggestion."
                        onClicked: root.externalUrlRequested(root.issueReportUrl)
                    }

                    SectionCard {
                        objectName: "aboutSection"
                        width: parent.width
                        title: "About Licasa"
                        subtitle: "Version " + root.appVersion
                        contentTopSpacing: 4
                        color: "transparent"
                        radius: 12
                        border.width: 1
                        border.color: "#24FFFFFF"

                        Text {
                            objectName: "aboutReleaseDate"
                            visible: root.releaseDate.length > 0
                            width: parent.width
                            text: "Released " + root.releaseDate
                            color: "#B3FFFFFF"
                            font.pixelSize: 12
                        }

                        Text {
                            width: parent.width
                            visible: root.installedAsSnap
                            text: "Fully quitting Licasa allows Snap updates. Open pictures and unsaved changes are kept until you finish them."
                            color: "#B3FFFFFF"
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }

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
