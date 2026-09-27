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
        title: "Light"
        subtitle: "Shape brightness and tonal range"

        AdjustRow {
            objectName: "editorExposureControl"
            width: parent.width
            label: "Exposure"
            value: root.exposure
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.exposureRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Contrast"
            value: root.contrast
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.contrastRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Highlights"
            value: root.highlights
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.highlightsRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Shadows"
            value: root.shadows
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.shadowsRequested(v) }
        }
    }

    SectionCard {
        width: parent.width
        title: "Color"
        subtitle: "Control intensity and color balance"

        AdjustRow {
            width: parent.width
            label: "Saturation"
            value: root.saturation
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.saturationRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Vibrance"
            value: root.vibrance
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.vibranceRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Warmth"
            value: root.warmth
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.warmthRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Tint"
            value: root.tint
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.tintRequested(v) }
        }
    }

    SectionCard {
        width: parent.width
        title: "Detail"
        subtitle: "Refine texture and edge focus"

        AdjustRow {
            width: parent.width
            label: "Sharpen"
            value: root.sharpen
            from: 0.0
            to: 1.0
            signedDisplay: false
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.sharpenRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Soften"
            value: root.blur
            from: 0.0
            to: 0.35
            signedDisplay: false
            onInteractionStarted: root.interactionStarted()
            onInteractionFinished: root.interactionFinished()
            onValueRequested: function(v) { root.blurRequested(v) }
        }

        AdjustRow {
            width: parent.width
            label: "Vignette"
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
