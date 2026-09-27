import QtQuick

Item {
    id: root

    required property Flickable flickable

    property real minimumHandleHeight: 40
    property color trackColor: "#12FFFFFF"
    property color handleColor: "#5AFFFFFF"
    property color activeHandleColor: "#B8FFFFFF"

    readonly property real scrollRange: Math.max(
        0,
        flickable.contentHeight - flickable.height
    )
    readonly property real handleHeight: Math.min(
        Math.max(
            minimumHandleHeight,
            flickable.height * flickable.height
                / Math.max(flickable.height, flickable.contentHeight)
        ),
        Math.max(0, height - 8)
    )
    readonly property real handleTravel: Math.max(0, height - handleHeight - 8)
    readonly property real normalizedPosition: scrollRange > 0
        ? Math.max(0, Math.min(1, flickable.contentY / scrollRange))
        : 0
    readonly property bool dragging: scrollMouse.pressed

    visible: flickable.contentHeight > flickable.height + 0.5
    enabled: visible
    width: 16

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: scrollMouse.containsMouse || scrollMouse.pressed ? 8 : 4
        radius: width / 2
        color: root.trackColor

        Behavior on width {
            NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
        }
    }

    Rectangle {
        id: handle

        anchors.horizontalCenter: parent.horizontalCenter
        y: 4 + root.normalizedPosition * root.handleTravel
        width: scrollMouse.containsMouse || scrollMouse.pressed ? 8 : 4
        height: root.handleHeight
        radius: width / 2
        color: scrollMouse.pressed
            ? root.activeHandleColor
            : root.handleColor
        antialiasing: true

        Behavior on width {
            NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
        }

        Behavior on color {
            ColorAnimation { duration: 100 }
        }
    }

    MouseArea {
        id: scrollMouse

        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor

        property real grabOffset: 0

        function setPosition(pointerY) {
            if (root.scrollRange <= 0 || root.handleTravel <= 0)
                return

            const handleY = Math.max(
                0,
                Math.min(root.handleTravel, pointerY - grabOffset - 4)
            )
            root.flickable.contentY = handleY / root.handleTravel * root.scrollRange
        }

        onPressed: function(mouse) {
            const handleTop = handle.y
            const handleBottom = handleTop + handle.height
            grabOffset = mouse.y >= handleTop && mouse.y <= handleBottom
                ? mouse.y - handleTop
                : handle.height / 2
            setPosition(mouse.y)
        }

        onPositionChanged: function(mouse) {
            if (pressed)
                setPosition(mouse.y)
        }
    }
}
