/*!
    InfoOverlays.qml
    ----------------
    Lightweight heads-up display overlays for compare mode,
    image metrics, and file name.
*/

import QtQuick

Item {
    id: root

    required property bool fullScreenMode
    required property bool infoOverlayEnabled
    required property bool compareOriginal
    required property bool imageReady
    required property bool limitedToPreview

    required property string currentFileName
    required property real imageWidth
    required property real imageHeight
    required property real currentScale

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 18
        visible: root.fullScreenMode && root.compareOriginal && root.imageReady
        width: compareText.implicitWidth + 22
        height: 28
        radius: height / 2
        color: "#161616"
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.10)

        Text {
            id: compareText
            anchors.centerIn: parent
            text: "Original preview"
            color: Qt.rgba(1, 1, 1, 0.94)
            font.pixelSize: 12
            font.weight: Font.Medium
            renderType: Text.NativeRendering
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: fileNameTextBox.visible ? fileNameTextBox.top : parent.bottom
        anchors.bottomMargin: 10
        visible: root.fullScreenMode && root.infoOverlayEnabled && root.imageReady
        width: Math.min(parent.width * 0.40, metricsText.implicitWidth + 26)
        height: 28
        radius: height / 2
        color: "#161616"
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.10)

        Text {
            id: metricsText
            anchors.fill: parent
            anchors.leftMargin: 13
            anchors.rightMargin: 13
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            color: Qt.rgba(1, 1, 1, 0.92)
            font.pixelSize: 12
            font.weight: Font.Medium
            renderType: Text.NativeRendering
            elide: Text.ElideRight
            text: Math.round(root.imageWidth) + " × " + Math.round(root.imageHeight)
                + "   " + Math.round(root.currentScale * 100) + "%"
                + (root.limitedToPreview ? "   MEMORY-SAVING PREVIEW" : "")
        }
    }

    Rectangle {
        id: fileNameTextBox
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
        visible: root.fullScreenMode && root.infoOverlayEnabled && root.imageReady && root.currentFileName.length > 0
        width: Math.min(parent.width * 0.52, fileNameText.implicitWidth + 26)
        height: 28
        radius: height / 2
        color: "#161616"
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.10)

        Text {
            id: fileNameText
            anchors.fill: parent
            anchors.leftMargin: 13
            anchors.rightMargin: 13
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            text: root.currentFileName
            color: Qt.rgba(1, 1, 1, 0.92)
            font.pixelSize: 12
            font.weight: Font.Medium
            renderType: Text.NativeRendering
            elide: Text.ElideMiddle
        }
    }
}
