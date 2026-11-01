pragma ComponentBehavior: Bound

import QtQuick
import "components"

Column {
    id: content
    required property EditorSidePanel editor
    required property real viewportHeight
    enabled: !content.editor.sourceChanged
    spacing: 12

    EditorAdjustContent {
        visible: content.editor.activeTool === "adjust"
        width: parent.width
        exposure: content.editor.exposure
        contrast: content.editor.contrast
        highlights: content.editor.highlights
        shadows: content.editor.shadows
        saturation: content.editor.saturation
        vibrance: content.editor.vibrance
        warmth: content.editor.warmth
        tint: content.editor.tint
        sharpen: content.editor.sharpen
        blur: content.editor.blur
        vignette: content.editor.vignette
        onInteractionStarted: content.editor.adjustmentInteractionStarted()
        onInteractionFinished: content.editor.adjustmentInteractionFinished()
        onExposureRequested: function(value) { content.editor.exposureRequested(value) }
        onContrastRequested: function(value) { content.editor.contrastRequested(value) }
        onHighlightsRequested: function(value) { content.editor.highlightsRequested(value) }
        onShadowsRequested: function(value) { content.editor.shadowsRequested(value) }
        onSaturationRequested: function(value) { content.editor.saturationRequested(value) }
        onVibranceRequested: function(value) { content.editor.vibranceRequested(value) }
        onWarmthRequested: function(value) { content.editor.warmthRequested(value) }
        onTintRequested: function(value) { content.editor.tintRequested(value) }
        onSharpenRequested: function(value) { content.editor.sharpenRequested(value) }
        onBlurRequested: function(value) { content.editor.blurRequested(value) }
        onVignetteRequested: function(value) { content.editor.vignetteRequested(value) }
    }

    EditorCropContent {
        id: cropContent
        visible: content.editor.activeTool === "crop"
        width: parent.width
        cropMode: content.editor.cropMode
        cropOutputWidth: content.editor.cropOutputWidth
        cropOutputHeight: content.editor.cropOutputHeight
        imageWidth: content.editor.cropImageWidth
        imageHeight: content.editor.cropImageHeight
        cropEstimatedFileSize: content.editor.cropEstimatedFileSize
        cropAspectPreset: content.editor.cropAspectPreset
        customAspectEditorOpen: content.editor.customAspectEditorOpen
        customAspectWidthText: content.editor.customAspectWidthText
        customAspectHeightText: content.editor.customAspectHeightText
        flipHorizontal: content.editor.flipHorizontal
        flipVertical: content.editor.flipVertical
        rotationQuarterTurns: content.editor.rotationQuarterTurns
        buttonMotionDuration: content.editor.buttonMotionDuration
        onCropStarted: content.editor.cropStarted()
        onCropPresetRequested: function(presetName) { content.editor.cropPresetRequested(presetName) }
        onCustomAspectEditorRequested: content.editor.customAspectEditorOpen = true
        onCustomAspectWidthTextEdited: function(value) { content.editor.customAspectWidthText = value }
        onCustomAspectHeightTextEdited: function(value) { content.editor.customAspectHeightText = value }
        onCustomCropAspectRatioRequested: function(ratio) {
            content.editor.customCropAspectRatioRequested(ratio)
        }
        onRotateLeftRequested: content.editor.rotateLeftRequested()
        onRotateRightRequested: content.editor.rotateRightRequested()
        onFlipHorizontalToggled: content.editor.flipHorizontalToggled()
        onFlipVerticalToggled: content.editor.flipVerticalToggled()
        onRotationResetRequested: content.editor.rotationResetRequested()
    }

    SectionCard {
        visible: content.editor.activeTool === "looks"
        width: parent.width
        title: "Looks"
        subtitle: "Every look remains fully undoable"

        Flow {
            width: parent.width
            spacing: 8

            ActionChip {
                text: "Original"
                checked: !content.editor.hasAdjustments
                animationDuration: content.editor.buttonMotionDuration
                onClicked: content.editor.resetRequested()
            }

            ActionChip {
                text: "Mono"
                animationDuration: content.editor.buttonMotionDuration
                onClicked: content.editor.presetRequested("bw")
            }

            ActionChip {
                text: "Pop"
                animationDuration: content.editor.buttonMotionDuration
                onClicked: content.editor.presetRequested("pop")
            }

            ActionChip {
                text: "Vivid"
                animationDuration: content.editor.buttonMotionDuration
                onClicked: content.editor.presetRequested("vivid")
            }

            ActionChip {
                text: "Cinema"
                animationDuration: content.editor.buttonMotionDuration
                onClicked: content.editor.presetRequested("cinematic")
            }

            ActionChip {
                text: "Soft"
                animationDuration: content.editor.buttonMotionDuration
                onClicked: content.editor.presetRequested("soft")
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
        visible: content.editor.activeTool === "export"
        width: parent.width
        minimumHeight: content.viewportHeight
        exportFormat: content.editor.exportFormat
        exportScale: content.editor.exportScale
        exportQuality: content.editor.exportQuality
        exportFormats: content.editor.exportFormats
        sourceFormat: content.editor.exportSourceFormat
        exportSettings: content.editor.exportSettings
        estimatedSize: content.editor.estimatedSize
        saveBusy: content.editor.saveBusy
        hasImage: content.editor.hasImage
        onSettingRequested: function(name, value) { content.editor.exportSettingRequested(name, value) }
        onExportRequested: content.editor.exportRequested()
        unscaledWidth: content.editor.unscaledOutputWidth
        unscaledHeight: content.editor.unscaledOutputHeight
        outputWidth: content.editor.outputWidth
        outputHeight: content.editor.outputHeight
        saveStatus: content.editor.saveStatus
        buttonMotionDuration: content.editor.buttonMotionDuration
        onExportFormatRequested: function(format) { content.editor.exportFormatRequested(format) }
        onExportScaleRequested: function(value) { content.editor.exportScaleRequested(value) }
        onExportQualityRequested: function(value) { content.editor.exportQualityRequested(value) }
    }
}
