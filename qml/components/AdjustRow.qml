import QtQuick

Item {
    id: root

    required property string label
    required property real value

    property real from: -1.0
    property real to: 1.0
    property real defaultValue: 0.0
    property real keyboardStep: (to - from) / 100.0
    property bool signedDisplay: true
    readonly property bool changed: Math.abs(value - defaultValue) > 0.0005

    signal valueRequested(real value)
    signal interactionStarted()
    signal interactionFinished()

    function clamp(v, lo, hi) {
        return Math.max(lo, Math.min(hi, v))
    }

    function ratioFor(v) {
        return clamp((v - root.from) / Math.max(0.00001, root.to - root.from), 0.0, 1.0)
    }

    function valueForTrackX(trackX) {
        const ratio = clamp(trackX / Math.max(1, sliderTrack.width), 0.0, 1.0)
        return root.from + (root.to - root.from) * ratio
    }

    function displayText(v) {
        const pct = Math.round(v * 100)
        if (!root.signedDisplay)
            return pct + "%"
        return (pct > 0 ? "+" : "") + pct + "%"
    }

    function commitValue(nextValue) {
        interactionStarted()
        valueRequested(clamp(nextValue, from, to))
        interactionFinished()
    }

    height: 48
    activeFocusOnTab: true

    Accessible.role: Accessible.Slider
    Accessible.name: label
    Accessible.description: displayText(value)
    Accessible.focusable: true
    Accessible.focused: activeFocus

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Left)
            commitValue(value - keyboardStep)
        else if (event.key === Qt.Key_Right)
            commitValue(value + keyboardStep)
        else if (event.key === Qt.Key_Home)
            commitValue(defaultValue)
        else
            return
        event.accepted = true
    }

    Text {
        id: labelText
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.label
        color: "white"
        font.pixelSize: 12
        font.weight: Font.Medium
        renderType: Text.NativeRendering
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 5

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.displayText(root.value)
            color: root.changed ? "#E8FFFFFF" : "#A8FFFFFF"
            font.pixelSize: 12
            font.weight: root.changed ? Font.Medium : Font.Normal
            renderType: Text.NativeRendering
        }

        Rectangle {
            visible: root.changed
            width: 22
            height: 22
            radius: 11
            color: resetMouse.containsMouse ? "#24FFFFFF" : "#12FFFFFF"
            border.width: 1
            border.color: "#20FFFFFF"

            Text {
                anchors.centerIn: parent
                text: "↺"
                color: "#D8FFFFFF"
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }

            MouseArea {
                id: resetMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.commitValue(root.defaultValue)
            }
        }
    }

    Rectangle {
        id: sliderTrack
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: labelText.bottom
        anchors.topMargin: 12
        height: 8
        radius: height / 2
        color: "#18FFFFFF"
        border.width: 1
        border.color: "#18FFFFFF"

        Rectangle {
            readonly property real defaultX: root.ratioFor(root.defaultValue) * parent.width

            x: root.signedDisplay ? Math.min(defaultX, knob.centerX) : 0
            width: root.signedDisplay
                ? Math.abs(knob.centerX - defaultX)
                : Math.max(0, knob.centerX)
            height: parent.height
            radius: parent.radius
            color: root.activeFocus ? "#F0FFFFFF" : "#B5FFFFFF"
        }
    }

    Rectangle {
        id: knob
        property real centerX: root.ratioFor(root.value) * sliderTrack.width

        width: 16
        height: 16
        radius: width / 2
        x: sliderTrack.x + centerX - width / 2
        y: sliderTrack.y + sliderTrack.height / 2 - height / 2
        color: "white"
        border.width: 1
        border.color: "#26000000"
        antialiasing: true
    }

    MouseArea {
        anchors.left: sliderTrack.left
        anchors.right: sliderTrack.right
        anchors.verticalCenter: sliderTrack.verticalCenter
        height: 32
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        function updateValue(mouseX) {
            root.valueRequested(root.valueForTrackX(mouseX))
        }

        onPressed: function(mouse) {
            root.forceActiveFocus()
            root.interactionStarted()
            updateValue(mouse.x)
        }

        onPositionChanged: function(mouse) {
            if (pressed)
                updateValue(mouse.x)
        }

        onReleased: root.interactionFinished()
        onCanceled: root.interactionFinished()

        onWheel: function(wheel) {
            const direction = wheel.angleDelta.y >= 0 ? 1 : -1
            root.commitValue(root.value + direction * root.keyboardStep)
            wheel.accepted = true
        }
    }
}
