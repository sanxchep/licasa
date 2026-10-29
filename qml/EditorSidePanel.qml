/*!
    EditorSidePanel.qml
    -------------------
    Persistent, mode-based editing workspace for Licasa.
*/

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import "components"

Item {
    id: root

    required property bool panelOpen
    required property bool hasImage

    property bool smoothMotionEnabled: true
    property bool launcherBlocked: false
    readonly property bool panelVisible: panelOpen || panel.revealProgress > 0.001
    property real cornerControlSize: 48
    property real cornerControlMargin: 24
    property bool compareOriginal: false
    property bool hasAdjustments: false
    property bool saveBusy: false
    property bool sourceChanged: false
    property url imageSource: ""
    property bool overwriteConfirmationOpen: false
    readonly property Item focusedItem: Window.window ? Window.window.activeFocusItem : null
    readonly property EditorNumber focusedNumber: focusedItem as EditorNumber
    onFocusedItemChanged: {
        if (panelOpen) focusRevealTimer.restart()
    }
    Timer {
        id: focusRevealTimer
        interval: 0
        onTriggered: { if (root.panelOpen) root.revealFocusedItem(root.focusedItem) }
    }
    readonly property bool pendingNumber: focusedNumber !== null && focusedNumber.dirty
    readonly property bool canOverwrite: panelOpen && hasImage && (hasAdjustments || pendingNumber)
        && !saveBusy && !cropMode && !sourceChanged
    property string saveStatus: ""
    property bool cropMode: false
    property string cropAspectPreset: "free"
    property int cropOutputWidth: 0
    property int cropOutputHeight: 0
    property real cropImageWidth: 0
    property real cropImageHeight: 0
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
    property var exportSettings: ({})
    property var exportFormats: []
    property string estimatedSize: ""
    property string exportSourceFormat: "png"
    signal exportSettingRequested(string name, var value)
    signal exportRequested()
    signal historyRequested(bool redo)
    Connections {
        target: root.focusedNumber
        function onHistoryRequested(redo) { root.historyRequested(redo) }
    }
    property int unscaledOutputWidth: Math.max(1, Math.round(outputWidth / exportScale))
    property int unscaledOutputHeight: Math.max(1, Math.round(outputHeight / exportScale))
    property int outputWidth: 0
    property int outputHeight: 0

    signal panelOpenChangedByUser(bool open)
    signal toolActivated(string tool)
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
        if (sourceChanged) return "File changed. Reopen to continue."
        switch (activeTool) {
        case "crop": return cropMode ? "Adjust the frame, then apply" : "Frame, rotate, and mirror"
        case "looks": return "One-click non-destructive styles"
        case "export": return "Format, resolution, and quality"
        default: return "Light, color, and detail"
        }
    }

    function requestSaveConfirmation() {
        finishPendingInput(true)
        if (!canOverwrite || !hasAdjustments)
            return
        overwriteConfirmationOpen = true
        overwriteNoButton.forceActiveFocus()
    }

    function cancelSaveConfirmation(restoreFocus = true) {
        overwriteConfirmationOpen = false
        if (restoreFocus && canOverwrite) {
            if (activeTool === "export") toolTabRepeater.itemAt(3).forceActiveFocus()
            else saveButton.forceActiveFocus()
        }
    }

    function confirmOverwrite() {
        if (!overwriteConfirmationOpen || !canOverwrite || !hasAdjustments)
            return
        overwriteConfirmationOpen = false
        root.saveRequested()
    }

    onCanOverwriteChanged: {
        if (!canOverwrite)
            overwriteConfirmationOpen = false
    }
    onImageSourceChanged: overwriteConfirmationOpen = false
    onActiveToolChanged: overwriteConfirmationOpen = false

    function activateTool(tool) {
        if (cropMode && tool !== "crop")
            return
        finishPendingInput(true)
        activeTool = tool
        flick.contentY = 0
        toolActivated(tool)
    }

    function finishPendingInput(apply) {
        const input = focusedNumber
        if (!input) return
        let ancestor = input.parent
        while (ancestor && ancestor !== root) ancestor = ancestor.parent
        if (ancestor) input.finishEditing(apply)
    }

    function scrollByPage(direction) {
        flick.contentY = Math.max(0, Math.min(
            Math.max(0, flick.contentHeight - flick.height),
            flick.contentY + direction * flick.height * 0.8
        ))
    }

    function revealFocusedItem(item) {
        let ancestor = item
        while (ancestor && ancestor !== flick)
            ancestor = ancestor.parent
        if (!ancestor || !item)
            return
        const position = item.mapToItem(flick.contentItem, 0, 0)
        const top = position.y
        const bottom = top + item.height
        const target = top < flick.contentY ? top - 8
            : bottom > flick.contentY + flick.height
                ? bottom - flick.height + 8 : flick.contentY
        flick.contentY = Math.max(0, Math.min(
            Math.max(0, flick.contentHeight - flick.height), target))
    }

    onPanelOpenChanged: {
        if (!panelOpen || toolTabRepeater.count === 0)
            return
        const toolIndex = ["adjust", "crop", "looks", "export"].indexOf(activeTool)
        toolTabRepeater.itemAt(Math.max(0, toolIndex)).forceActiveFocus()
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

    // Controls consume their own arrows first (sliders, fields and tool tabs).
    // An otherwise unhandled arrow dismisses the drawer without browsing/zooming.
    Keys.priority: Keys.AfterItem
    Keys.onPressed: function(event) {
        if (overwriteConfirmationOpen && event.key === Qt.Key_Escape) {
            cancelSaveConfirmation()
            event.accepted = true
            return
        }
        if (!root.panelOpen || event.modifiers !== Qt.NoModifier
                || [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down].indexOf(event.key) < 0)
            return
        event.accepted = true
        root.panelOpenChangedByUser(false)
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
        objectName: "editorMenuButton"
        visible: root.hasImage && !root.panelOpen
            && panel.x <= -panel.width - 17 && hideProgress < 1
        enabled: !root.launcherBlocked
        Accessible.ignored: root.launcherBlocked
        property real hideProgress: root.launcherBlocked ? 1 : 0
        Behavior on hideProgress {
            NumberAnimation { duration: root.panelMotionDuration; easing.type: Easing.OutCubic }
        }
        width: root.cornerControlSize
        height: width
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.cornerControlMargin
            - (root.cornerControlMargin + width + 8) * hideProgress
        anchors.bottomMargin: root.cornerControlMargin
        iconName: "edit"
        accessibleName: "Open editor"
        animationDuration: root.buttonMotionDuration
        onClicked: {
            if (!root.launcherBlocked)
                root.panelOpenChangedByUser(true)
        }
    }

    MouseArea {
        objectName: "editorDismissArea"
        anchors.fill: parent
        // Keep the crop canvas available for drawing and dragging a crop.
        enabled: root.panelOpen && !root.cropMode
        onClicked: root.panelOpenChangedByUser(false)
    }

    Rectangle {
        id: panelShadow
        width: panel.width + 12
        height: panel.height + 12
        x: panel.x - 6
        y: panel.y - 6
        radius: panel.radius + 6
        visible: root.panelOpen || panel.x > -panel.width - 17
        color: "#26000000"
    }

    Rectangle {
        id: panel
        objectName: "editorPanel"
        clip: true
        enabled: root.panelOpen
        width: root.panelWidth
        height: root.height - 24
        // The hidden position follows the window size without animating it.
        property real revealProgress: root.panelOpen ? 1 : 0
        x: -width - 18 + (width + 18 + root.panelLeftMargin) * revealProgress
        y: 12
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
            onPressed: root.forceActiveFocus()
            onWheel: function(wheel) { root.scrollEditorByWheel(wheel) }
        }

        HoverHandler { id: panelHover }

        Item {
            id: header
            enabled: !root.overwriteConfirmationOpen
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
                anchors.rightMargin: 20
                anchors.top: parent.top
                anchors.topMargin: 16
                spacing: 6

                HeaderButton {
                    width: 64
                    height: 34
                    text: "Reset"
                    enabled: (root.hasAdjustments || root.pendingNumber) && !root.cropMode
                    highlighted: root.hasAdjustments && !root.cropMode
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.resetRequested()
                }

                HeaderButton {
                    objectName: "editorCloseButton"
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
            enabled: !root.overwriteConfirmationOpen
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.leftMargin: 20
            anchors.rightMargin: 20
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
                    id: toolTabRepeater
                    model: [
                        { "key": "adjust", "label": "Adjust" },
                        { "key": "crop", "label": "Crop" },
                        { "key": "looks", "label": "Looks" },
                        { "key": "export", "label": "Export" }
                    ]

                    Rectangle {
                        required property var modelData
                        objectName: "editorToolTab_" + modelData.key

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
                            if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                                const tools = ["adjust", "crop", "looks", "export"]
                                const next = (tools.indexOf(modelData.key)
                                    + (event.key === Qt.Key_Right ? 1 : -1) + tools.length)
                                    % tools.length
                                if (!root.cropMode || tools[next] === "crop") {
                                    root.activateTool(tools[next])
                                    toolTabRepeater.itemAt(next).forceActiveFocus()
                                }
                                event.accepted = true
                                return
                            }
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
            objectName: "editorScrollArea"
            enabled: !root.overwriteConfirmationOpen
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: toolBar.bottom
            anchors.bottom: footer.top
            anchors.margins: 20
            anchors.topMargin: 12
            anchors.bottomMargin: 16
            clip: true
            contentWidth: width
            contentHeight: contentColumn.implicitHeight
            onContentHeightChanged: focusRevealTimer.restart()
            onHeightChanged: focusRevealTimer.restart()
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: contentColumn
                enabled: !root.sourceChanged
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
                    imageWidth: root.cropImageWidth
                    imageHeight: root.cropImageHeight
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
                        text: "Every look remains fully undoable."
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
                    exportFormats: root.exportFormats
                    sourceFormat: root.exportSourceFormat
                    exportSettings: root.exportSettings
                    estimatedSize: root.estimatedSize
                    saveBusy: root.saveBusy
                    hasImage: root.hasImage
                    onSettingRequested: function(name, value) { root.exportSettingRequested(name, value) }
                    onExportRequested: root.exportRequested()
                    unscaledWidth: root.unscaledOutputWidth
                    unscaledHeight: root.unscaledOutputHeight
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
            enabled: !root.overwriteConfirmationOpen

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

        MouseArea {
            objectName: "editorOverwriteDismissArea"
            anchors.fill: parent
            z: 2
            enabled: root.overwriteConfirmationOpen
            visible: enabled
            onClicked: root.cancelSaveConfirmation()
        }

        Item {
            id: overwriteConfirmation
            objectName: "editorOverwriteConfirmation"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: footer.top
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            anchors.bottomMargin: 12
            height: warningBody.implicitHeight + 28
            z: 30
            clip: true
            visible: root.overwriteConfirmationOpen || revealProgress > 0
            enabled: root.overwriteConfirmationOpen
            property real revealProgress: root.overwriteConfirmationOpen ? 1 : 0
            Accessible.role: Accessible.Dialog
            Accessible.name: "Overwrite original?"
            Accessible.description: "This replaces the original image. The original will be lost. Continue?"
            Accessible.ignored: !root.overwriteConfirmationOpen

            Behavior on revealProgress {
                NumberAnimation {
                    duration: root.smoothMotionEnabled ? 180 : 0
                    easing.type: Easing.OutCubic
                }
            }

            Rectangle {
                anchors.fill: parent
                radius: 16
                color: "#292318"
                border.width: 1
                border.color: "#D9B34C"
                opacity: overwriteConfirmation.revealProgress
                transform: Translate {
                    y: (overwriteConfirmation.height + 8) * (1 - overwriteConfirmation.revealProgress)
                }

                MouseArea { anchors.fill: parent }

                Column {
                    id: warningBody
                    x: 14
                    y: 14
                    width: parent.width - 28
                    spacing: 10

                    Text {
                        width: parent.width
                        text: "⚠  Overwrite original?"
                        color: "#F0CC70"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                        renderType: Text.NativeRendering
                    }

                    Text {
                        width: parent.width
                        text: overwriteConfirmation.Accessible.description
                        color: "#E6FFFFFF"
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        renderType: Text.NativeRendering
                    }

                    Row {
                        width: parent.width
                        spacing: 8
                        layoutDirection: Qt.RightToLeft

                        ActionChip {
                            id: overwriteYesButton
                            objectName: "editorOverwriteYesButton"
                            width: 68
                            text: "Yes"
                            checked: true
                            accent: "#8A681E"
                            enabled: root.overwriteConfirmationOpen && root.canOverwrite
                            animationDuration: root.buttonMotionDuration
                            KeyNavigation.tab: overwriteNoButton
                            KeyNavigation.backtab: overwriteNoButton
                            onClicked: root.confirmOverwrite()
                        }

                        ActionChip {
                            id: overwriteNoButton
                            objectName: "editorOverwriteNoButton"
                            width: 68
                            text: "No"
                            animationDuration: root.buttonMotionDuration
                            KeyNavigation.tab: overwriteYesButton
                            KeyNavigation.backtab: overwriteYesButton
                            onClicked: root.cancelSaveConfirmation()
                        }
                    }
                }
            }
        }

        Item {
            id: footer
            objectName: "editorFooter"
            z: 3
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            property real revealProgress: root.activeTool === "export" && !root.cropMode ? 0 : 1
            Behavior on revealProgress { NumberAnimation { duration: root.smoothMotionEnabled ? 180 : 0; easing.type: Easing.OutCubic } }
            enabled: root.activeTool !== "export" || root.cropMode
            opacity: revealProgress
            visible: revealProgress > .001
            anchors.bottomMargin: 20 - 60 * (1 - revealProgress)
            height: root.cropMode ? 44 : 40

            Row {
                visible: root.cropMode
                anchors.fill: parent
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

            Row {
                visible: !root.cropMode
                anchors.fill: parent
                spacing: 10

                ActionChip {
                    id: saveButton
                    objectName: "editorSaveButton"
                    width: (parent.width - parent.spacing) * 0.38
                    height: parent.height
                    text: root.saveBusy ? "Saving…" : "Save"
                    enabled: root.canOverwrite
                    checked: root.hasAdjustments && !root.saveBusy
                    accent: "#48FFFFFF"
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.requestSaveConfirmation()
                }

                ActionChip {
                    objectName: "editorSaveCopyButton"
                    width: parent.width - x
                    height: parent.height
                    text: "Save as a copy"
                    enabled: !root.saveBusy && root.hasImage && !root.sourceChanged
                    animationDuration: root.buttonMotionDuration
                    onClicked: {
                        root.cancelSaveConfirmation(false)
                        root.saveAsRequested()
                    }
                }
            }

        }
    }
}
