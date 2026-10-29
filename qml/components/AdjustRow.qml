import QtQuick

FocusScope {
    id: root

    required property string label
    required property real value

    property real from: -1.0
    property real to: 1.0
    property real defaultValue: 0.0
    property real keyboardStep: (to - from) / 100.0
    property bool signedDisplay: true
    property real displayMultiplier: 100
    property string suffix: "%"
    property int decimals: 1
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

    function commitValue(nextValue) {
        if (!isFinite(nextValue)) return
        interactionStarted()
        valueRequested(clamp(nextValue, from, to))
        interactionFinished()
    }

    height: 56

    // Qt 6.4/6.8 read the item's conventional "value" property directly.
    // Keep the precise edit value on root, and expose whole displayed percent
    // units on a separate focus surface. Orca otherwise speaks the complete
    // binary floating-point tail of values such as 0.09999999999999999.
    Item {
        id: accessibleSlider
        objectName: "adjustmentAccessibleSlider"
        anchors.fill: parent
        focus: true
        activeFocusOnTab: enabled && visible
        property int value: 0
        readonly property int minimumValue: Math.round(root.from * root.displayMultiplier)
        readonly property int maximumValue: Math.round(root.to * root.displayMultiplier)
        readonly property real stepSize: root.keyboardStep * root.displayMultiplier
        Accessible.role: Accessible.Slider
        Accessible.name: root.label
        Accessible.description: root.suffix === "%" ? "Value in percent" : "Value"
        Accessible.focusable: activeFocusOnTab
        Accessible.focused: activeFocus
        Accessible.onIncreaseAction: root.commitValue(root.value + root.keyboardStep)
        Accessible.onDecreaseAction: root.commitValue(root.value - root.keyboardStep)
        onValueChanged: {
            const displayed = Math.round(root.value * root.displayMultiplier)
            if (value === displayed) return
            root.commitValue(value / root.displayMultiplier)
            value = Math.round(root.value * root.displayMultiplier)
        }
        Binding {
            target: accessibleSlider
            property: "value"
            value: Math.round(root.value * root.displayMultiplier)
        }
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Down)
            commitValue(value - keyboardStep)
        else if (event.key === Qt.Key_Right || event.key === Qt.Key_Up)
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
        anchors.verticalCenter: valueRow.verticalCenter
        width: Math.max(0, valueRow.x - 8)
        elide: Text.ElideRight
        text: root.label
        Accessible.ignored: true
        color: "white"
        font.pixelSize: 12
        font.weight: Font.Medium
        renderType: Text.NativeRendering
    }

    Row {
        id: valueRow
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 5
        height: 26

        EditorNumber {
            objectName: "adjustmentValueInput"
            anchors.verticalCenter: parent.verticalCenter
            width: root.suffix.length > 0 ? 84 : 68
            compact: true
            label: root.label
            value: root.value * root.displayMultiplier
            minimum: root.from * root.displayMultiplier
            maximum: root.to * root.displayMultiplier
            decimals: root.decimals
            signedDisplay: root.signedDisplay
            suffix: root.suffix
            onValueRequested: function(v) { root.commitValue(v / root.displayMultiplier) }
        }

        Rectangle {
            id: resetButton
            objectName: "adjustmentResetButton"
            visible: root.changed
            width: 22
            height: 22
            anchors.verticalCenter: parent.verticalCenter
            radius: 11
            color: resetMouse.containsMouse ? "#24FFFFFF" : "#12FFFFFF"
            border.width: activeFocus ? 2 : 1
            border.color: activeFocus ? "#E8FFFFFF" : "#20FFFFFF"
            activeFocusOnTab: enabled && visible
            Accessible.role: Accessible.Button
            Accessible.name: "Reset " + root.label
            Accessible.focusable: activeFocusOnTab
            Accessible.focused: activeFocus
            Accessible.onPressAction: {
                accessibleSlider.forceActiveFocus()
                root.commitValue(root.defaultValue)
            }
            Keys.onPressed: function(event) {
                if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                        && event.key !== Qt.Key_Enter) return
                if (!event.isAutoRepeat) {
                    accessibleSlider.forceActiveFocus()
                    root.commitValue(root.defaultValue)
                }
                event.accepted = true
            }

            Text {
                anchors.centerIn: parent
                text: "↺"
                Accessible.ignored: true
                color: "#D8FFFFFF"
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }

            MouseArea {
                id: resetMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    accessibleSlider.forceActiveFocus()
                    root.commitValue(root.defaultValue)
                }
            }
        }
    }

    Rectangle {
        id: sliderTrack
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: valueRow.bottom
        anchors.topMargin: 10
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
        height: 26
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
