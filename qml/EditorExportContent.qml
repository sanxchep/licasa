pragma ComponentBehavior: Bound

import QtQuick
import "components"

Column {
    id: root

    required property string exportFormat
    required property real exportScale
    required property int exportQuality
    required property int outputWidth
    required property int outputHeight
    required property string saveStatus
    required property int buttonMotionDuration

    signal exportFormatRequested(string format)
    signal exportScaleRequested(real value)
    signal exportQualityRequested(int value)

    spacing: 12

    SectionCard {
        width: parent.width
        title: "File format"
        subtitle: "Choose how the edited image is encoded"

        Flow {
            width: parent.width
            spacing: 8

            Repeater {
                model: [
                    { "key": "original", "label": "Original" },
                    { "key": "png", "label": "PNG" },
                    { "key": "jpeg", "label": "JPEG" },
                    { "key": "webp", "label": "WebP" }
                ]

                ActionChip {
                    required property var modelData
                    objectName: "editorExportFormat_" + modelData.key
                    text: modelData.label
                    checked: root.exportFormat === modelData.key
                    accent: "#48FFFFFF"
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.exportFormatRequested(modelData.key)
                }
            }
        }
    }

    SectionCard {
        width: parent.width
        title: "Resolution"
        subtitle: "Scale the final pixel dimensions"

        AdjustRow {
            objectName: "editorExportScaleControl"
            width: parent.width
            label: "Output scale"
            value: root.exportScale
            from: 0.1
            to: 2.0
            defaultValue: 1.0
            signedDisplay: false
            onValueRequested: function(v) { root.exportScaleRequested(v) }
        }

        Text {
            width: parent.width
            text: root.outputWidth > 0 && root.outputHeight > 0
                ? root.outputWidth + " × " + root.outputHeight + " pixels"
                : "Resolution unavailable"
            color: "#D0FFFFFF"
            font.pixelSize: 12
            font.weight: Font.Medium
            renderType: Text.NativeRendering
        }
    }

    SectionCard {
        visible: root.exportFormat === "jpeg" || root.exportFormat === "webp"
        width: parent.width
        title: "Compression"
        subtitle: "Higher quality creates a larger file"

        AdjustRow {
            width: parent.width
            label: "Quality"
            value: root.exportQuality / 100.0
            from: 0.1
            to: 1.0
            defaultValue: 0.95
            signedDisplay: false
            onValueRequested: function(v) {
                root.exportQualityRequested(Math.round(v * 100))
            }
        }
    }

    Text {
        visible: root.saveStatus.length > 0
        width: parent.width
        text: root.saveStatus
        color: "#C8FFFFFF"
        font.pixelSize: 11
        wrapMode: Text.Wrap
        renderType: Text.NativeRendering
    }
}
