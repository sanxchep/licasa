pragma ComponentBehavior: Bound

import QtQuick
import "components"

Column {
    id: root

    required property bool cropMode
    required property int cropOutputWidth
    required property int cropOutputHeight
    required property string cropEstimatedFileSize
    required property string cropAspectPreset
    required property bool customAspectEditorOpen
    required property string customAspectWidthText
    required property string customAspectHeightText
    required property bool flipHorizontal
    required property bool flipVertical
    required property int rotationQuarterTurns
    required property int buttonMotionDuration

    signal cropStarted()
    signal cropPresetRequested(string presetName)
    signal customAspectEditorRequested()
    signal customAspectWidthTextEdited(string value)
    signal customAspectHeightTextEdited(string value)
    signal customCropAspectRatioRequested(real ratio)
    signal rotateLeftRequested()
    signal rotateRightRequested()
    signal flipHorizontalToggled()
    signal flipVerticalToggled()
    signal rotationResetRequested()

    spacing: 12

    function customAspectRatioIsValid() {
        const widthValue = Number(customAspectWidthText)
        const heightValue = Number(customAspectHeightText)
        return Number.isFinite(widthValue) && Number.isFinite(heightValue)
            && widthValue > 0 && heightValue > 0
    }

    function openCustomAspectEditor() {
        customAspectEditorRequested()
        customAspectFocusTimer.restart()
    }

    function applyCustomAspectRatio() {
        if (!customAspectRatioIsValid())
            return

        const widthValue = Number(customAspectWidthText)
        const heightValue = Number(customAspectHeightText)
        customCropAspectRatioRequested(widthValue / heightValue)
    }

    Timer {
        id: customAspectFocusTimer
        interval: 0
        repeat: false

        onTriggered: {
            customWidthInput.forceActiveFocus()
            customWidthInput.selectAll()
        }
    }

    SectionCard {
        width: parent.width
        title: root.cropMode ? "Crop active" : "Crop & frame"
        subtitle: root.cropMode
            ? "Drag inside to move; use corners or edges to resize"
            : "Start a precise, non-destructive crop"

        Text {
            width: parent.width
            text: root.cropMode && root.cropOutputWidth > 0
                ? root.cropOutputWidth + " × " + root.cropOutputHeight
                    + " px selected"
                    + (root.cropEstimatedFileSize.length > 0
                        ? "  (" + root.cropEstimatedFileSize + ")"
                        : "")
                : "The photo stays fully visible while you frame it."
            color: "#C7FFFFFF"
            font.pixelSize: 11
            wrapMode: Text.Wrap
            renderType: Text.NativeRendering
        }

        ActionChip {
            visible: !root.cropMode
            text: "Start crop"
            checked: true
            accent: "#48FFFFFF"
            animationDuration: root.buttonMotionDuration
            onClicked: root.cropStarted()
        }

        ActionChip {
            visible: root.cropMode
            text: "Reset frame"
            animationDuration: root.buttonMotionDuration
            onClicked: root.cropPresetRequested("full")
        }
    }

    SectionCard {
        width: parent.width
        title: "Aspect ratio"
        subtitle: root.customAspectEditorOpen
                && root.cropAspectPreset !== "custom"
            ? "Enter horizontal and vertical proportions"
            : root.cropAspectPreset === "free"
                ? "Freeform resizing"
                : "Ratio locked while dragging corners"

        Flow {
            width: parent.width
            spacing: 8

            Repeater {
                model: [
                    { "key": "free", "label": "Free" },
                    { "key": "original", "label": "Original" },
                    { "key": "square", "label": "1:1" },
                    { "key": "4:3", "label": "4:3" },
                    { "key": "3:2", "label": "3:2" },
                    { "key": "4:5", "label": "4:5" },
                    { "key": "16:9", "label": "16:9" }
                ]

                ActionChip {
                    required property var modelData
                    text: modelData.label
                    checked: root.cropAspectPreset === modelData.key
                    accent: "#48FFFFFF"
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.cropPresetRequested(modelData.key)
                }
            }

                ActionChip {
                    objectName: "editorCustomAspectChip"
                    text: "Custom"
                checked: root.cropAspectPreset === "custom"
                    || root.customAspectEditorOpen
                accent: "#48FFFFFF"
                animationDuration: root.buttonMotionDuration
                onClicked: root.openCustomAspectEditor()
            }
        }

        Rectangle {
            visible: root.customAspectEditorOpen
            width: parent.width
            height: 76
            radius: 16
            color: "#151515"
            border.width: 1
            border.color: "#24FFFFFF"

            Row {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                height: 58
                spacing: 8

                Column {
                    width: 76
                    spacing: 4

                    Text {
                        text: "Horizontal"
                        color: "#99FFFFFF"
                        font.pixelSize: 10
                        renderType: Text.NativeRendering
                    }

                    Rectangle {
                        id: customWidthField
                        width: parent.width
                        height: 38
                        radius: 11
                        color: "#24FFFFFF"
                        border.width: customWidthInput.activeFocus ? 2 : 1
                        border.color: customWidthInput.activeFocus
                            ? "#D8FFFFFF" : "#28FFFFFF"

                        TextInput {
                            id: customWidthInput
                            objectName: "customAspectWidthInput"
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: root.customAspectWidthText
                            color: "white"
                            selectionColor: "#D8FFFFFF"
                            selectedTextColor: "#161616"
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            horizontalAlignment: TextInput.AlignHCenter
                            verticalAlignment: TextInput.AlignVCenter
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                            activeFocusOnTab: true
                            selectByMouse: true
                            validator: DoubleValidator {
                                bottom: 0.01
                                top: 9999
                                decimals: 3
                                notation: DoubleValidator.StandardNotation
                            }
                            onTextEdited: root.customAspectWidthTextEdited(text)
                            onAccepted: root.applyCustomAspectRatio()
                        }
                    }
                }

                Text {
                    y: 28
                    text: ":"
                    color: "#C8FFFFFF"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    renderType: Text.NativeRendering
                }

                Column {
                    width: 76
                    spacing: 4

                    Text {
                        text: "Vertical"
                        color: "#99FFFFFF"
                        font.pixelSize: 10
                        renderType: Text.NativeRendering
                    }

                    Rectangle {
                        width: parent.width
                        height: 38
                        radius: 11
                        color: "#24FFFFFF"
                        border.width: customHeightInput.activeFocus ? 2 : 1
                        border.color: customHeightInput.activeFocus
                            ? "#D8FFFFFF" : "#28FFFFFF"

                        TextInput {
                            id: customHeightInput
                            objectName: "customAspectHeightInput"
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: root.customAspectHeightText
                            color: "white"
                            selectionColor: "#D8FFFFFF"
                            selectedTextColor: "#161616"
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            horizontalAlignment: TextInput.AlignHCenter
                            verticalAlignment: TextInput.AlignVCenter
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                            activeFocusOnTab: true
                            selectByMouse: true
                            validator: DoubleValidator {
                                bottom: 0.01
                                top: 9999
                                decimals: 3
                                notation: DoubleValidator.StandardNotation
                            }
                            onTextEdited: root.customAspectHeightTextEdited(text)
                            onAccepted: root.applyCustomAspectRatio()
                        }
                    }
                }

                ActionChip {
                    objectName: "editorApplyCustomAspectChip"
                    y: 20
                    width: Math.max(68, parent.width - x)
                    height: 38
                    text: "Apply"
                    checked: root.cropAspectPreset === "custom"
                    enabled: root.customAspectRatioIsValid()
                    accent: "#48FFFFFF"
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.applyCustomAspectRatio()
                }
            }
        }
    }

    SectionCard {
        width: parent.width
        title: "Rotate & mirror"
        subtitle: root.cropMode
            ? "Apply or cancel the frame before transforming"
            : "Change orientation without losing quality"

        Flow {
            width: parent.width
            spacing: 8

            ActionChip {
                text: "↶  Left"
                enabled: !root.cropMode
                animationDuration: root.buttonMotionDuration
                onClicked: root.rotateLeftRequested()
            }

            ActionChip {
                text: "↷  Right"
                enabled: !root.cropMode
                animationDuration: root.buttonMotionDuration
                onClicked: root.rotateRightRequested()
            }

            ActionChip {
                text: "Mirror H"
                enabled: !root.cropMode
                checked: root.flipHorizontal
                animationDuration: root.buttonMotionDuration
                onClicked: root.flipHorizontalToggled()
            }

            ActionChip {
                text: "Mirror V"
                enabled: !root.cropMode
                checked: root.flipVertical
                animationDuration: root.buttonMotionDuration
                onClicked: root.flipVerticalToggled()
            }

            ActionChip {
                visible: root.rotationQuarterTurns !== 0
                enabled: !root.cropMode
                text: "Reset " + (root.rotationQuarterTurns * 90) + "°"
                checked: true
                accent: "#48FFFFFF"
                animationDuration: root.buttonMotionDuration
                onClicked: root.rotationResetRequested()
            }
        }
    }
}
