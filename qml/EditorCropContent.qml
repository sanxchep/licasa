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
    property real imageWidth: 0
    property real imageHeight: 0

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
        if (!Number.isFinite(widthValue) || !Number.isFinite(heightValue)
                || widthValue < 0.01 || widthValue > 9999
                || heightValue < 0.01 || heightValue > 9999) return false
        if (imageWidth <= 0 || imageHeight <= 0) return true
        const normalized = (widthValue / heightValue) / (imageWidth / imageHeight)
        return normalized >= .02 - 1e-10 && normalized <= 50 + 1e-10
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

        Rectangle {
            objectName: "editorCropDimensions"
            width: parent.width
            height: 64
            radius: 12
            color: "#282828"
            border.width: 1
            border.color: "#32FFFFFF"
            Column {
                anchors.left: parent.left
                anchors.right: sizeLabel.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 12
                spacing: 4
                Text {
                    text: root.cropMode ? "Selected area" : "Image size"
                    color: "#A8FFFFFF"
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
                Text {
                    width: parent.width
                    text: root.cropMode && root.cropOutputWidth > 0
                        ? root.cropOutputWidth + " × " + root.cropOutputHeight
                        : root.imageWidth > 0 ? Math.round(root.imageWidth) + " × " + Math.round(root.imageHeight) : "Ready to frame"
                    color: "white"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    renderType: Text.NativeRendering
                }
            }
            Text {
                id: sizeLabel
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: root.cropMode && root.cropEstimatedFileSize.length > 0 ? root.cropEstimatedFileSize : "px"
                color: "#B3FFFFFF"
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
        }

        ActionChip {
            visible: !root.cropMode
            objectName: "editorStartCropButton"
            width: parent.width
            height: 44
            text: "Start crop"
            sectionStyle: true
            affirmative: true
            animationDuration: root.buttonMotionDuration
            onClicked: root.cropStarted()
        }

        ActionChip {
            visible: root.cropMode
            width: parent.width
            text: "Reset frame"
            sectionStyle: true
            height: 44
            animationDuration: root.buttonMotionDuration
            onClicked: root.cropPresetRequested("full")
        }
    }

    SectionCard {
        width: parent.width
        title: "Aspect ratio"

        Grid {
            id: ratioGrid
            objectName: "editorCropRatioGrid"
            width: parent.width
            columns: 4
            spacing: 8

            Repeater {
                model: [
                    { "key": "free", "label": "Free", "ratio": 0 },
                    { "key": "original", "label": "Original", "ratio": root.imageHeight > 0 ? root.imageWidth / root.imageHeight : 1.5 },
                    { "key": "square", "label": "1:1", "ratio": 1 },
                    { "key": "4:3", "label": "4:3", "ratio": 4 / 3 },
                    { "key": "3:2", "label": "3:2", "ratio": 3 / 2 },
                    { "key": "4:5", "label": "4:5", "ratio": 4 / 5 },
                    { "key": "16:9", "label": "16:9", "ratio": 16 / 9 }
                ]

                CropRatioButton {
                    required property var modelData
                    objectName: "editorCropRatio_" + modelData.key
                    width: (ratioGrid.width - 3 * ratioGrid.spacing) / 4
                    text: modelData.label
                    ratio: modelData.ratio
                    checked: root.cropAspectPreset === modelData.key && !root.customAspectEditorOpen
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.cropPresetRequested(modelData.key)
                }
            }

            CropRatioButton {
                objectName: "editorCustomAspectChip"
                width: (ratioGrid.width - 3 * ratioGrid.spacing) / 4
                text: "Custom"
                custom: true
                checked: root.cropAspectPreset === "custom"
                    || root.customAspectEditorOpen
                animationDuration: root.buttonMotionDuration
                onClicked: root.openCustomAspectEditor()
            }
        }

        Rectangle {
            objectName: "editorCustomAspectFields"
            visible: root.customAspectEditorOpen
            width: parent.width
            height: 76
            radius: 12
            color: "#282828"
            border.width: 1
            border.color: "#32FFFFFF"

            Row {
                id: customFields
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                height: 58
                spacing: 8

                Column {
                    width: (customFields.width - 10 - customApply.width - 3 * customFields.spacing) / 2
                    spacing: 4

                    Text {
                        text: "Width"
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
                            Accessible.name: "Horizontal aspect ratio"
                            Accessible.description: "Enter the width proportion, then press Enter to apply."
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
                    width: 10
                    y: customWidthField.y + (customWidthField.height - height) / 2
                    text: ":"
                    color: "#C8FFFFFF"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    renderType: Text.NativeRendering
                }

                Column {
                    width: (customFields.width - 10 - customApply.width - 3 * customFields.spacing) / 2
                    spacing: 4

                    Text {
                        text: "Height"
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
                            Accessible.name: "Vertical aspect ratio"
                            Accessible.description: "Enter the height proportion, then press Enter to apply."
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
                    id: customApply
                    objectName: "editorApplyCustomAspectChip"
                    y: customWidthField.y
                    width: 72
                    height: 38
                    text: "Apply"
                    sectionStyle: true
                    textAlignment: Text.AlignHCenter
                    affirmative: true
                    enabled: root.customAspectRatioIsValid()
                    animationDuration: root.buttonMotionDuration
                    onClicked: root.applyCustomAspectRatio()
                }
            }
        }
    }

    SectionCard {
        width: parent.width
        title: "Rotate & mirror"

        Grid {
            id: orientationGrid
            objectName: "editorCropOrientationGrid"
            width: parent.width
            columns: 2
            spacing: 8

            ActionChip {
                width: (orientationGrid.width - orientationGrid.spacing) / 2
                height: 44
                sectionStyle: true
                text: "↶  Rotate left"
                textColor: "#DDFFFFFF"
                enabled: !root.cropMode
                animationDuration: root.buttonMotionDuration
                onClicked: root.rotateLeftRequested()
            }

            ActionChip {
                width: (orientationGrid.width - orientationGrid.spacing) / 2
                height: 44
                sectionStyle: true
                text: "↷  Rotate right"
                textColor: "#DDFFFFFF"
                enabled: !root.cropMode
                animationDuration: root.buttonMotionDuration
                onClicked: root.rotateRightRequested()
            }

            ActionChip {
                width: (orientationGrid.width - orientationGrid.spacing) / 2
                height: 44
                sectionStyle: true
                affirmative: root.flipHorizontal
                text: "Flip horizontal"
                textColor: "#DDFFFFFF"
                enabled: !root.cropMode
                checkable: true
                checked: root.flipHorizontal
                animationDuration: root.buttonMotionDuration
                onClicked: root.flipHorizontalToggled()
            }

            ActionChip {
                width: (orientationGrid.width - orientationGrid.spacing) / 2
                height: 44
                sectionStyle: true
                affirmative: root.flipVertical
                text: "Flip vertical"
                textColor: "#DDFFFFFF"
                enabled: !root.cropMode
                checkable: true
                checked: root.flipVertical
                animationDuration: root.buttonMotionDuration
                onClicked: root.flipVerticalToggled()
            }

        }

        ActionChip {
            width: parent.width
            height: 44
            sectionStyle: true
            visible: root.rotationQuarterTurns !== 0
            enabled: !root.cropMode
            text: "Reset " + (root.rotationQuarterTurns * 90) + "°"
            textColor: "#DDFFFFFF"
            animationDuration: root.buttonMotionDuration
            onClicked: root.rotationResetRequested()
        }
    }
}
