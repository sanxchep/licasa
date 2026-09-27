/*!
    OverlayControls.qml
    -------------------
    Top-left fullscreen toggle button and close button overlay.
*/

import QtQuick
import "components"

Item {
    id: root

    required property bool appVisible
    required property bool fullScreenMode
    required property bool hasImage
    required property real windowedCloseButtonX
    required property real windowedCloseButtonY
    property real cornerControlSize: 48
    property real cornerControlMargin: 24

    signal toggleFullScreenRequested()
    signal closeRequested()

    RoundIconButton {
        id: fullScreenButton
        width: root.cornerControlSize
        height: width
        x: root.cornerControlMargin
        y: root.cornerControlMargin
        z: 100
        visible: root.appVisible && root.hasImage && root.fullScreenMode
        iconName: "exit-fullscreen"
        accessibleName: "Exit fullscreen"
        onClicked: root.toggleFullScreenRequested()
    }

    RoundIconButton {
        id: closeButton
        width: root.fullScreenMode ? root.cornerControlSize : 36
        height: width
        x: Math.round(
                root.fullScreenMode
                ? root.width - width - root.cornerControlMargin
                : root.windowedCloseButtonX
        )
        y: Math.round(
                root.fullScreenMode
                ? root.cornerControlMargin
                : root.windowedCloseButtonY
        )
        z: 110
        visible: root.appVisible
        iconName: "close"
        accessibleName: "Close image"
        hoverColor: "#B93838"
        pressedColor: "#D73737"
        onClicked: root.closeRequested()
    }
}
