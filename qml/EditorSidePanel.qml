/*!
    EditorSidePanel.qml
    -------------------
    Persistent, mode-based editing workspace for Licasa.
*/

pragma ComponentBehavior: Bound

import QtQuick
import "components"

Item {
    id: root

    required property bool panelOpen
    required property bool hasImage

    property bool smoothMotionEnabled: true
    property real cornerControlSize: 48
    property real cornerControlMargin: 24
    property bool compareOriginal: false
    property bool hasAdjustments: false
    property bool saveBusy: false
    property string saveStatus: ""
    property bool cropMode: false
    property string cropAspectPreset: "free"
    property int cropOutputWidth: 0
    property int cropOutputHeight: 0
    property string cropEstimatedFileSize: ""
    property bool customAspectEditorOpen: false
    property string customAspectWidthText: "4"
    property string customAspectHeightText: "3"

    readonly property real panelLeftMargin: 12
    readonly property real panelWidth: Math.min(420, Math.max(360, width * 0.27))
    readonly property real workspaceBoundaryX: panelLeftMargin + panelWidth + 24
    readonly property bool pointerInsidePanel: root.panelOpen
        && (panelHover.hovered || editorScrollBar.dragging)

    property string activeTool: "adjust"

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
    property int rotationQuarterTurns: 0

    property string exportFormat: "original"
    property real exportScale: 1.0
    property int exportQuality: 95
    property int outputWidth: 0
    property int outputHeight: 0

    signal panelOpenChangedByUser(bool open)
    signal exposureRequested(real value)
    signal contrastRequested(real value)
    signal highlightsRequested(real value)
    signal shadowsRequested(real value)
    signal saturationRequested(real value)
    signal vibranceRequested(real value)
    signal warmthRequested(real value)
    signal tintRequested(real value)
    signal blurRequested(real value)
    signal sharpenRequested(real value)
    signal vignetteRequested(real value)
    signal adjustmentInteractionStarted()
    signal adjustmentInteractionFinished()
    signal flipHorizontalToggled()
    signal flipVerticalToggled()
    signal rotateLeftRequested()
    signal rotateRightRequested()
    signal rotationResetRequested()
    signal compareOriginalPressed()
    signal compareOriginalReleased()
    signal saveRequested()
    signal saveAsRequested()
    signal resetRequested()
    signal presetRequested(string presetName)
    signal cropStarted()
    signal cropCanceled()
    signal cropApplied()
    signal cropPresetRequested(string presetName)
    signal customCropAspectRatioRequested(real ratio)
    signal exportFormatRequested(string format)
    signal exportScaleRequested(real value)
    signal exportQualityRequested(int value)

    readonly property int panelMotionDuration: smoothMotionEnabled ? 220 : 0
    readonly property int buttonMotionDuration: smoothMotionEnabled ? 140 : 0
    readonly property string toolSubtitle: {
        switch (activeTool) {
        case "crop": return cropMode ? "Adjust the frame, then apply" : "Frame, rotate, and mirror"
        case "looks": return "One-click non-destructive styles"
        case "export": return "Format, resolution, and quality"
        default: return "Light, color, and detail"
        }
    }

    function activateTool(tool) {
        if (cropMode && tool !== "crop")
            return
        activeTool = tool
        flick.contentY = 0
    }

    function scrollEditorByWheel(wheel) {
        const maximumContentY = Math.max(0, flick.contentHeight - flick.height)
        const pixelDelta = wheel.pixelDelta.y
        const scrollDelta = pixelDelta !== 0
            ? pixelDelta
            : wheel.angleDelta.y / 120.0 * 72.0

        if (scrollDelta !== 0) {
            flick.contentY = Math.max(
                0,
                Math.min(maximumContentY, flick.contentY - scrollDelta)
            )
        }

        // Always consume wheel input inside the editor, including at either
        // boundary. Otherwise Qt delivers the unhandled event to the image
        // viewport underneath and the photo unexpectedly zooms.
        wheel.accepted = true
    }

    anchors.fill: parent
    visible: hasImage || panelOpen
    enabled: visible

    onCropModeChanged: {
        if (cropMode)
            activateTool("crop")
    }

    RoundIconButton {
        id: editorButton
        visible: root.hasImage && !root.panelOpen
        width: root.cornerControlSize
        height: width
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.cornerControlMargin
        anchors.bottomMargin: root.cornerControlMargin
        iconName: "edit"
        accessibleName: "Open editor"
        animationDuration: root.buttonMotionDuration
        onClicked: root.panelOpenChangedByUser(true)
    }

    Rectangle {
        id: panelShadow
        width: panel.width + 12
        height: panel.height + 12
        x: panel.x - 6
        y: panel.y - 6
        radius: panel.radius + 6
        visible: root.panelOpen
        color: "#26000000"
    }

    Rectangle {
        id: panel
        width: root.panelWidth
        height: root.height - 24
        x: root.panelOpen ? root.panelLeftMargin : -width - 18
        y: 12
        radius: 26
        color: "#F21A1A1A"
        border.width: 1
        border.color: "#30FFFFFF"
        antialiasing: true

        Behavior on x {
            NumberAnimation {
                duration: root.panelMotionDuration
                easing.type: Easing.OutCubic
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: function(wheel) { root.scrollEditorByWheel(wheel) }
        }

        HoverHandler { id: panelHover }

        Item {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 82

            Text {
                id: titleText
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.top: parent.top
                anchors.topMargin: 16
                text: root.hasAdjustments ? "Editor  •" : "Editor"
                color: "white"
                font.pixelSize: 22
                font.weight: Font.DemiBold
                renderType: Text.NativeRendering
            }

            Text {
                anchors.left: titleText.left
                anchors.top: titleText.bottom
                anchors.topMargin: 3
                text: root.toolSubtitle
                color: "#9FFFFFFF"
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.top: parent.top
                anchors.topMargin: 16
                spacing: 6

                HeaderButton {
                    width: 64
                    height: 34
                    text: "Reset"
                    enabled: root.hasAdjustments && !root.cropMode
                    highlighted: root.hasAdjustments && !root.cropMode
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.resetRequested()
                }

                HeaderButton {
                    width: 34
                    height: 34
                    text: "×"
                    emphasizedText: true
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.panelOpenChangedByUser(false)
                }
            }
        }

        Rectangle {
            id: toolBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            height: 46
            radius: 15
            color: "#D0121212"
            border.width: 1
            border.color: "#20FFFFFF"

            Row {
                id: toolTabs
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                Repeater {
                    model: [
                        { "key": "adjust", "label": "Adjust" },
                        { "key": "crop", "label": "Crop" },
                        { "key": "looks", "label": "Looks" },
                        { "key": "export", "label": "Export" }
                    ]

                    Rectangle {
                        required property var modelData

                        readonly property bool selected: root.activeTool === modelData.key
                        readonly property bool available: !root.cropMode || modelData.key === "crop"

                        width: (toolTabs.width - toolTabs.spacing * 3) / 4
                        height: toolTabs.height
                        radius: 11
                        activeFocusOnTab: available
                        color: selected ? "#38FFFFFF" : tabMouse.containsMouse && available
                            ? "#18FFFFFF" : "transparent"
                        opacity: available ? 1.0 : 0.36
                        border.width: activeFocus ? 2 : 0
                        border.color: "#C8FFFFFF"

                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.label
                        Accessible.focusable: available
                        Accessible.focused: activeFocus
                        Accessible.selected: selected
                        Accessible.onPressAction: root.activateTool(modelData.key)

                        Keys.onPressed: function(event) {
                            if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                                    && event.key !== Qt.Key_Enter) {
                                return
                            }
                            root.activateTool(modelData.key)
                            event.accepted = true
                        }

                        Behavior on color {
                            ColorAnimation { duration: root.buttonMotionDuration }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: parent.modelData.label
                            color: "white"
                            font.pixelSize: 11
                            font.weight: parent.selected ? Font.DemiBold : Font.Medium
                            renderType: Text.NativeRendering
                        }

                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            enabled: parent.available
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onPressed: parent.forceActiveFocus()
                            onClicked: root.activateTool(parent.modelData.key)
                        }
                    }
                }
            }
        }

        Flickable {
            id: flick
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: toolBar.bottom
            anchors.bottom: footer.top
            anchors.margins: 14
            anchors.topMargin: 12
            anchors.bottomMargin: 10
            clip: true
            contentWidth: width
            contentHeight: contentColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: contentColumn
                width: flick.width
                spacing: 12

                EditorAdjustContent {
                    visible: root.activeTool === "adjust"
                    width: parent.width
                    exposure: root.exposure
                    contrast: root.contrast
                    highlights: root.highlights
                    shadows: root.shadows
                    saturation: root.saturation
                    vibrance: root.vibrance
                    warmth: root.warmth
                    tint: root.tint
                    sharpen: root.sharpen
                    blur: root.blur
                    vignette: root.vignette
                    onInteractionStarted: root.adjustmentInteractionStarted()
                    onInteractionFinished: root.adjustmentInteractionFinished()
                    onExposureRequested: function(value) { root.exposureRequested(value) }
                    onContrastRequested: function(value) { root.contrastRequested(value) }
                    onHighlightsRequested: function(value) { root.highlightsRequested(value) }
                    onShadowsRequested: function(value) { root.shadowsRequested(value) }
                    onSaturationRequested: function(value) { root.saturationRequested(value) }
                    onVibranceRequested: function(value) { root.vibranceRequested(value) }
                    onWarmthRequested: function(value) { root.warmthRequested(value) }
                    onTintRequested: function(value) { root.tintRequested(value) }
                    onSharpenRequested: function(value) { root.sharpenRequested(value) }
                    onBlurRequested: function(value) { root.blurRequested(value) }
                    onVignetteRequested: function(value) { root.vignetteRequested(value) }
                }

                EditorCropContent {
                    id: cropContent
                    visible: root.activeTool === "crop"
                    width: parent.width
                    cropMode: root.cropMode
                    cropOutputWidth: root.cropOutputWidth
                    cropOutputHeight: root.cropOutputHeight
                    cropEstimatedFileSize: root.cropEstimatedFileSize
                    cropAspectPreset: root.cropAspectPreset
                    customAspectEditorOpen: root.customAspectEditorOpen
                    customAspectWidthText: root.customAspectWidthText
                    customAspectHeightText: root.customAspectHeightText
                    flipHorizontal: root.flipHorizontal
                    flipVertical: root.flipVertical
                    rotationQuarterTurns: root.rotationQuarterTurns
                    buttonMotionDuration: root.buttonMotionDuration
                    onCropStarted: root.cropStarted()
                    onCropPresetRequested: function(presetName) { root.cropPresetRequested(presetName) }
                    onCustomAspectEditorRequested: root.customAspectEditorOpen = true
                    onCustomAspectWidthTextEdited: function(value) { root.customAspectWidthText = value }
                    onCustomAspectHeightTextEdited: function(value) { root.customAspectHeightText = value }
                    onCustomCropAspectRatioRequested: function(ratio) {
                        root.customCropAspectRatioRequested(ratio)
                    }
                    onRotateLeftRequested: root.rotateLeftRequested()
                    onRotateRightRequested: root.rotateRightRequested()
                    onFlipHorizontalToggled: root.flipHorizontalToggled()
                    onFlipVerticalToggled: root.flipVerticalToggled()
                    onRotationResetRequested: root.rotationResetRequested()
                }

                SectionCard {
                    visible: root.activeTool === "looks"
                    width: parent.width
                    title: "Looks"
                    subtitle: "Every look remains fully undoable"

                    Flow {
                        width: parent.width
                        spacing: 8

                        ActionChip {
                            text: "Original"
                            checked: !root.hasAdjustments
                            animationDuration: root.buttonMotionDuration
                            onClicked: root.resetRequested()
                        }

                        ActionChip {
                            text: "Mono"
                            animationDuration: root.buttonMotionDuration
                            onClicked: root.presetRequested("bw")
                        }

                        ActionChip {
                            text: "Pop"
                            animationDuration: root.buttonMotionDuration
                            onClicked: root.presetRequested("pop")
                        }

                        ActionChip {
                            text: "Vivid"
                            animationDuration: root.buttonMotionDuration
                            onClicked: root.presetRequested("vivid")
                        }

                        ActionChip {
                            text: "Cinema"
                            animationDuration: root.buttonMotionDuration
                            onClicked: root.presetRequested("cinematic")
                        }

                        ActionChip {
                            text: "Soft"
                            animationDuration: root.buttonMotionDuration
                            onClicked: root.presetRequested("soft")
                        }
                    }

                    Text {
                        width: parent.width
                        text: "Tip: hold Original below to compare any look without changing it."
                        color: "#90FFFFFF"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        renderType: Text.NativeRendering
                    }
                }

                EditorExportContent {
                    visible: root.activeTool === "export"
                    width: parent.width
                    exportFormat: root.exportFormat
                    exportScale: root.exportScale
                    exportQuality: root.exportQuality
                    outputWidth: root.outputWidth
                    outputHeight: root.outputHeight
                    saveStatus: root.saveStatus
                    buttonMotionDuration: root.buttonMotionDuration
                    onExportFormatRequested: function(format) { root.exportFormatRequested(format) }
                    onExportScaleRequested: function(value) { root.exportScaleRequested(value) }
                    onExportQualityRequested: function(value) { root.exportQualityRequested(value) }
                }
            }
        }

        InteractiveScrollBar {
            id: editorScrollBar

            flickable: flick
            width: 16
            z: 20
            anchors.right: parent.right
            anchors.rightMargin: 1
            y: flick.y
            height: flick.height
            minimumHandleHeight: 40
            handleColor: "#5AFFFFFF"
            activeHandleColor: "#E0FFFFFF"
        }

        Rectangle {
            id: footer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: root.cropMode ? 76 : 88
            radius: panel.radius
            color: "#FA1A1A1A"

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: "#24FFFFFF"
            }

            Row {
                visible: root.cropMode
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                ActionChip {
                    width: (parent.width - parent.spacing) * 0.38
                    height: parent.height
                    text: "Cancel"
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.cropCanceled()
                }

                ActionChip {
                    width: (parent.width - parent.spacing) * 0.62
                    height: parent.height
                    text: "Apply crop"
                    checked: true
                    accent: "#48FFFFFF"
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.cropApplied()
                }
            }

            Column {
                visible: !root.cropMode
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Row {
                    width: parent.width
                    height: 34
                    spacing: 8

                    Rectangle {
                        width: 104
                        height: parent.height
                        radius: height / 2
                        color: originalMouse.pressed || root.compareOriginal
                            ? "#40FFFFFF" : "#18FFFFFF"
                        border.width: 1
                        border.color: "#28FFFFFF"

                        Text {
                            anchors.centerIn: parent
                            text: "Hold original"
                            color: "white"
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            renderType: Text.NativeRendering
                        }

                        MouseArea {
                            id: originalMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onPressed: root.compareOriginalPressed()
                            onReleased: root.compareOriginalReleased()
                            onCanceled: root.compareOriginalReleased()
                        }
                    }

                    ActionChip {
                        width: (parent.width - 120) * 0.45
                        height: parent.height
                        text: root.saveBusy ? "Saving…" : "Save"
                        enabled: !root.saveBusy && root.hasAdjustments
                        checked: root.hasAdjustments && !root.saveBusy
                        accent: "#48FFFFFF"
                        animationDuration: root.buttonMotionDuration
                        onClicked: root.saveRequested()
                    }

                    ActionChip {
                        width: parent.width - x
                        height: parent.height
                        text: "Export…"
                        enabled: !root.saveBusy
                        animationDuration: root.buttonMotionDuration
                        onClicked: {
                            if (root.activeTool === "export")
                                root.saveAsRequested()
                            else
                                root.activateTool("export")
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: root.activeTool === "export"
                        ? "Export… opens the destination picker"
                        : "Edits stay non-destructive until Save or Export"
                    color: "#7FFFFFFF"
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignHCenter
                    renderType: Text.NativeRendering
                }
            }
        }
    }
}
