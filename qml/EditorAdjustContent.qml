pragma ComponentBehavior: Bound

import QtQuick
import "components"

Column {
    id: root

    required property real exposure
    required property real contrast
    required property real highlights
    required property real shadows
    required property real saturation
    required property real vibrance
    required property real warmth
    required property real tint
    required property real sharpen
    required property real blur
    required property real vignette

    signal interactionStarted()
    signal interactionFinished()
    signal exposureRequested(real value)
    signal contrastRequested(real value)
    signal highlightsRequested(real value)
    signal shadowsRequested(real value)
    signal saturationRequested(real value)
    signal vibranceRequested(real value)
    signal warmthRequested(real value)
    signal tintRequested(real value)
    signal sharpenRequested(real value)
    signal blurRequested(real value)
    signal vignetteRequested(real value)

    spacing: 12

    SectionCard {
        width: parent.width
        title: qsTr("Light")
        subtitle: qsTr("Shape brightness and tonal range")

        AdjustRow {
            objectName: "editorExposureControl"
            width: parent.width
            label: qsTr("Exposure")
            value: root.exposure
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.exposureRequested(v) }
        }

        AdjustRow {
            objectName: "editorContrastControl"
            width: parent.width
            label: qsTr("Contrast")
            value: root.contrast
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.contrastRequested(v) }
        }

        AdjustRow {
            objectName: "editorHighlightsControl"
            width: parent.width
            label: qsTr("Highlights")
            value: root.highlights
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.highlightsRequested(v) }
        }

        AdjustRow {
            objectName: "editorShadowsControl"
            width: parent.width
            label: qsTr("Shadows")
            value: root.shadows
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.shadowsRequested(v) }
        }
    }

    SectionCard {
        width: parent.width
        title: qsTr("Color")
        subtitle: qsTr("Control intensity and color balance")

        AdjustRow {
            objectName: "editorSaturationControl"
            width: parent.width
            label: qsTr("Saturation")
            value: root.saturation
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.saturationRequested(v) }
        }

        AdjustRow {
            objectName: "editorVibranceControl"
            width: parent.width
            label: qsTr("Vibrance")
            value: root.vibrance
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.vibranceRequested(v) }
        }

        AdjustRow {
            objectName: "editorWarmthControl"
            width: parent.width
            label: qsTr("Warmth")
            value: root.warmth
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.warmthRequested(v) }
        }

        AdjustRow {
            objectName: "editorTintControl"
            width: parent.width
            label: qsTr("Tint")
            value: root.tint
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.tintRequested(v) }
        }
    }

    SectionCard {
        width: parent.width
        title: qsTr("Detail")
        subtitle: qsTr("Refine texture and edge focus")

        AdjustRow {
            objectName: "editorSharpenControl"
            width: parent.width
            label: qsTr("Sharpen")
            value: root.sharpen
            from: 0.0
            to: 1.0
            signedDisplay: false
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.sharpenRequested(v) }
        }

        AdjustRow {
            objectName: "editorSoftenControl"
            width: parent.width
            label: qsTr("Soften")
            value: root.blur
            from: 0.0
            to: 0.35
            signedDisplay: false
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.blurRequested(v) }
        }

        AdjustRow {
            objectName: "editorVignetteControl"
            width: parent.width
            label: qsTr("Vignette")
            value: root.vignette
            from: 0.0
            to: 1.0
            signedDisplay: false
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.vignetteRequested(v) }
        }
    }
}
