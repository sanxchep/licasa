import QtQuick

Item {
    id: root

    required property string iconName
    property string accessibleName: ""
    property bool checked: false
    property int animationDuration: 140
    property color baseColor: "#1A1A1A"
    property color hoverColor: "#2A2A2A"
    property color pressedColor: "#383838"

    readonly property url iconSource: {
        switch (iconName) {
        case "settings": return Qt.resolvedUrl("../../assets/icons/settings.svg")
        case "edit": return Qt.resolvedUrl("../../assets/icons/edit.svg")
        case "exit-fullscreen": return Qt.resolvedUrl("../../assets/icons/exit-fullscreen.svg")
        case "close": return Qt.resolvedUrl("../../assets/icons/close.svg")
        case "notifications": return Qt.resolvedUrl("../../assets/icons/notifications.svg")
        default: return ""
        }
    }

    signal clicked()

    width: 48
    height: 48
    activeFocusOnTab: enabled

    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    Accessible.focusable: enabled
    Accessible.focused: activeFocus
    Accessible.onPressAction: clicked()

    Keys.onPressed: function(event) {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                && event.key !== Qt.Key_Enter) {
            return
        }
        if (!event.isAutoRepeat)
            root.clicked()
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: iconMouse.pressed
            ? root.pressedColor
            : iconMouse.containsMouse || root.checked
                ? root.hoverColor
                : root.baseColor
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? "#D8FFFFFF" : "#38FFFFFF"
        antialiasing: true

        Behavior on color {
            ColorAnimation { duration: root.animationDuration }
        }
    }

    Image {
        readonly property int rasterExtent: Math.max(
            24,
            Math.round(Math.min(root.width, root.height))
        )

        anchors.centerIn: parent
        width: Math.round(Math.min(root.width, root.height) * 0.48)
        height: width
        source: root.iconSource
        sourceSize.width: rasterExtent
        sourceSize.height: rasterExtent
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        asynchronous: false
        cache: true
    }

    MouseArea {
        id: iconMouse

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.clicked()
    }
}
