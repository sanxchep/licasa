pragma ComponentBehavior: Bound

import QtQuick
import "components"

Column {
    id: content
    required property FullscreenSideMenu menu
    spacing: 12

    ToggleRow {
        width: parent.width
        title: "Info overlay"
        subtitle: "Show file name and zoom details"
        checked: content.menu.infoOverlayEnabled
        buttonMotionDuration: content.menu.buttonMotionDuration
        switchMotionDuration: content.menu.switchMotionDuration

        onToggled: function(value) {
            content.menu.infoOverlayChanged(value)
        }
    }

    ToggleRow {
        width: parent.width
        title: "Smooth motion"
        subtitle: "Animate menu and switches"
        checked: content.menu.smoothMotionEnabled
        buttonMotionDuration: content.menu.buttonMotionDuration
        switchMotionDuration: content.menu.switchMotionDuration

        onToggled: function(value) {
            content.menu.smoothMotionChanged(value)
        }
    }

    ToggleRow {
        width: parent.width
        title: "Open in fullscreen"
        subtitle: "Use fullscreen when opening or showing images"
        checked: content.menu.startInFullscreenEnabled
        buttonMotionDuration: content.menu.buttonMotionDuration
        switchMotionDuration: content.menu.switchMotionDuration

        onToggled: function(value) {
            content.menu.startInFullscreenChanged(value)
        }
    }

    ToggleRow {
        id: backgroundServiceRow
        objectName: "backgroundServiceRow"
        visible: content.menu.backgroundModeManager
            && content.menu.backgroundModeManager.supported
        width: parent.width
        title: "Background process"
        subtitle: "Keep a lightweight background process open. Starts at next login."
        checked: content.menu.backgroundModeManager
            ? content.menu.backgroundModeManager.enabled : false
        buttonMotionDuration: content.menu.buttonMotionDuration
        switchMotionDuration: content.menu.switchMotionDuration

        onToggled: function(value) {
            content.menu.backgroundModeManager.setEnabled(value)
        }
    }

    ValueRow {
        width: parent.width
        title: "Background opacity"
        subtitle: "Dark backdrop behind the image in fullscreen"
        value: content.menu.fullscreenBackgroundOpacity
        step: 0.05
        buttonMotionDuration: content.menu.buttonMotionDuration

        onValueChangedByUser: function(value) {
            content.menu.fullscreenBackgroundOpacityRequested(value)
        }
    }

    ToggleRow {
        width: parent.width
        title: "Full screen transparency grid"
        subtitle: "Show a checkerboard behind transparent pixels in full screen"
        checked: content.menu.transparencyCheckerboardEnabled
        buttonMotionDuration: content.menu.buttonMotionDuration
        switchMotionDuration: content.menu.switchMotionDuration

        onToggled: function(value) {
            content.menu.transparencyCheckerboardChanged(value)
        }
    }

    ValueRow {
        width: parent.width
        title: "Grid opacity"
        subtitle: content.menu.transparencyCheckerboardEnabled
            ? "Strength of the transparency checkerboard"
            : "Turn on the full screen grid to adjust"
        value: content.menu.transparencyCheckerboardOpacity
        step: 0.05
        enabled: content.menu.transparencyCheckerboardEnabled
        opacity: enabled ? 1.0 : 0.7
        buttonMotionDuration: content.menu.buttonMotionDuration

        onValueChangedByUser: function(value) {
            content.menu.transparencyCheckerboardOpacityRequested(value)
        }
    }

    ToggleRow {
        width: parent.width
        title: "Floating transparency grid"
        subtitle: "Show a checkerboard instead of solid backing in floating mode"
        checked: content.menu.windowedTransparencyCheckerboardEnabled
        buttonMotionDuration: content.menu.buttonMotionDuration
        switchMotionDuration: content.menu.switchMotionDuration

        onToggled: function(value) {
            content.menu.windowedTransparencyCheckerboardChanged(value)
        }
    }

    ValueRow {
        width: parent.width
        title: "Floating transparency opacity"
        subtitle: content.menu.windowedTransparencyCheckerboardEnabled
            ? "Strength of the floating checkerboard"
            : "Strength of the solid backing for transparent pixels"
        value: content.menu.windowedTransparencyOpacity
        step: 0.05
        buttonMotionDuration: content.menu.buttonMotionDuration

        onValueChangedByUser: function(value) {
            content.menu.windowedTransparencyOpacityRequested(value)
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
        text: content.menu.advancedOpen
            ? "Advanced settings  ▲"
            : "Advanced settings  ▼"
        checkable: true
        checked: content.menu.advancedOpen
        accent: "#34FFFFFF"
        animationDuration: content.menu.buttonMotionDuration
        onClicked: content.menu.advancedOpen = !content.menu.advancedOpen
    }

    SectionCard {
        visible: content.menu.advancedOpen
        width: parent.width
        title: "Rendering quality"

        ToggleRow {
            width: parent.width
            title: "Smooth scaling"
            subtitle: "Filter pixels while zooming and fitting"
            checked: content.menu.smoothImageScalingEnabled
            buttonMotionDuration: content.menu.buttonMotionDuration
            switchMotionDuration: content.menu.switchMotionDuration
            onToggled: function(value) {
                content.menu.smoothImageScalingChanged(value)
            }
        }

        ToggleRow {
            width: parent.width
            title: "Mipmap downscaling"
            subtitle: "Sharper text and detail when zoomed out"
            checked: content.menu.mipmapImageScalingEnabled
            buttonMotionDuration: content.menu.buttonMotionDuration
            switchMotionDuration: content.menu.switchMotionDuration
            onToggled: function(value) {
                content.menu.mipmapImageScalingChanged(value)
            }
        }

        ToggleRow {
            width: parent.width
            title: "Pixel-aligned placement"
            subtitle: "Avoid half-pixel blur at the image origin"
            checked: content.menu.pixelAlignedRenderingEnabled
            buttonMotionDuration: content.menu.buttonMotionDuration
            switchMotionDuration: content.menu.switchMotionDuration
            onToggled: function(value) {
                content.menu.pixelAlignedRenderingChanged(value)
            }
        }

        ToggleRow {
            width: parent.width
            title: "Full-resolution rendering"
            subtitle: "Promote previews to the original resolution"
            checked: content.menu.fullResolutionRenderingEnabled
            buttonMotionDuration: content.menu.buttonMotionDuration
            switchMotionDuration: content.menu.switchMotionDuration
            onToggled: function(value) {
                content.menu.fullResolutionRenderingChanged(value)
            }
        }

        ToggleRow {
            width: parent.width
            title: "Color-managed rendering"
            subtitle: "Convert embedded profiles to display sRGB"
            checked: content.menu.colorManagedRenderingEnabled
            buttonMotionDuration: content.menu.buttonMotionDuration
            switchMotionDuration: content.menu.switchMotionDuration
            onToggled: function(value) {
                content.menu.colorManagedRenderingChanged(value)
            }
        }
    }

    ValueRow {
        visible: content.menu.advancedOpen
        objectName: "maximumImageMemoryControl"
        width: parent.width
        title: "Maximum working memory per image"
        subtitle: content.menu.estimatedLargeImageMemoryText()
            + "\nImages above this budget open in a bounded preview"
        from: content.menu.minimumImageMemoryMiB
        to: content.menu.maximumImageMemoryLimitMiB
        value: content.menu.maximumImageMemoryMiB
        step: 128
        displayMultiplier: 1.0 / 1024.0
        displayDecimals: 2
        displaySuffix: " GiB"
        accessibleUnit: "MiB"
        buttonMotionDuration: content.menu.buttonMotionDuration
        onValueChangedByUser: function(value) {
            content.menu.maximumImageMemoryMiBRequested(
                Math.max(content.menu.minimumImageMemoryMiB,
                    content.menu.minimumImageMemoryMiB
                        + Math.round((value - content.menu.minimumImageMemoryMiB) / 128)
                            * 128)
            )
        }
    }

    ValueRow {
        visible: content.menu.advancedOpen
        width: parent.width
        title: "Crop shield opacity"
        subtitle: "Dim only the area outside the crop frame"
        value: content.menu.cropShieldOpacity
        step: 0.05
        buttonMotionDuration: content.menu.buttonMotionDuration
        onValueChangedByUser: function(value) {
            content.menu.cropShieldOpacityRequested(value)
        }
    }

    SectionCard {
        visible: content.menu.advancedOpen
        width: parent.width
        title: "Animated Playback Settings"

        ToggleRow {
            width: parent.width
            title: "Play animated images"
            checked: content.menu.animatedImagePlaybackEnabled
            buttonMotionDuration: content.menu.buttonMotionDuration
            switchMotionDuration: content.menu.switchMotionDuration
            onToggled: function(value) {
                content.menu.animatedImagePlaybackChanged(value)
            }
        }

        ValueRow {
            width: parent.width
            title: "Playback speed"
            from: 0.25
            to: 2.0
            value: content.menu.animationSpeed
            step: 0.25
            displayMultiplier: 100
            displayDecimals: 0
            displaySuffix: "%"
            enabled: content.menu.animatedImagePlaybackEnabled
            opacity: enabled ? 1.0 : 0.7
            buttonMotionDuration: content.menu.buttonMotionDuration
            onValueChangedByUser: function(value) {
                content.menu.animationSpeedRequested(value)
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
        animationDuration: content.menu.buttonMotionDuration
        Accessible.description: subtitle + " Shortcut Control Shift Q."
        onClicked: content.menu.quitRequested()
    }

    ActionChip {
        objectName: "viewSourceButton"
        width: parent.width
        text: "View Source"
        sectionStyle: true
        animationDuration: content.menu.buttonMotionDuration
        Accessible.description: "Open the Licasa source repository on GitHub."
        onClicked: content.menu.externalUrlRequested(content.menu.sourceRepositoryUrl)
    }

    ActionChip {
        objectName: "reportIssueButton"
        width: parent.width
        text: "Report a Bug / Fix / Suggestion"
        sectionStyle: true
        animationDuration: content.menu.buttonMotionDuration
        Accessible.description: "Open a new GitHub issue for a bug, fix, or suggestion."
        onClicked: content.menu.externalUrlRequested(content.menu.issueReportUrl)
    }

    SectionCard {
        objectName: "aboutSection"
        width: parent.width
        title: "About Licasa"
        subtitle: "Version " + content.menu.appVersion
        contentTopSpacing: 4
        color: "transparent"
        radius: 12
        border.width: 1
        border.color: "#24FFFFFF"

        Text {
            objectName: "aboutReleaseDate"
            visible: content.menu.releaseDate.length > 0
            width: parent.width
            text: "Released " + content.menu.releaseDate
            color: "#B3FFFFFF"
            font.pixelSize: 12
        }

        Text {
            width: parent.width
            visible: content.menu.installedAsSnap
            text: "Fully quitting Licasa allows Snap updates. Open pictures and unsaved changes are kept until you finish them."
            color: "#B3FFFFFF"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
    }

}
