import QtQuick

Item {
    id: root

    required property string title
    property string subtitle: ""
    property real value: 0.0
    property real from: 0.0
    property real to: 1.0
    property real step: 0.05
    property real displayMultiplier: 100.0
    property int displayDecimals: 0
    property string displaySuffix: "%"
    property int buttonMotionDuration: 140

    signal valueChangedByUser(real value)

    function clampedValue(v) {
        return Math.max(from, Math.min(to, v))
    }

    function valueForTrackX(trackX, trackWidth) {
        const ratio = Math.max(0.0, Math.min(1.0, trackX / Math.max(1, trackWidth)))
        return from + ratio * Math.max(0.000001, to - from)
    }

    function normalizedValue() {
        return (clampedValue(value) - from) / Math.max(0.000001, to - from)
    }

    function formattedValue() {
        return (clampedValue(value) * displayMultiplier).toFixed(displayDecimals)
            + displaySuffix
    }

    readonly property real textBottom: valueSubtitle.visible
        ? valueSubtitle.y + valueSubtitle.height
        : valueTitle.y + valueTitle.height

    height: Math.max(72, textBottom + 12 + controls.height)

    Text {
        id: valueTitle
        anchors.left: parent.left
        anchors.right: valueLabel.left
        anchors.rightMargin: 12
        anchors.top: parent.top
        text: root.title
        color: "white"
        font.pixelSize: 14
        font.weight: Font.Medium
        elide: Text.ElideRight
    }

    Text {
        id: valueSubtitle
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: valueTitle.bottom
        anchors.topMargin: 2
        text: root.subtitle
        color: "#99FFFFFF"
        font.pixelSize: 11
        wrapMode: Text.Wrap
        visible: text.length > 0
    }

    Text {
        id: valueLabel
        anchors.right: parent.right
        anchors.top: parent.top
        text: root.formattedValue()
        color: "#F2FFFFFF"
        font.pixelSize: 13
        font.weight: Font.Medium
    }

    Item {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 34

        Rectangle {
            id: minusButton
            width: 42
            height: 34
            radius: 17
            anchors.left: parent.left
            color: "#26FFFFFF"
            border.width: 1
            border.color: "#24FFFFFF"

            Text {
                anchors.centerIn: parent
                text: "−"
                color: "white"
                font.pixelSize: 20
                font.weight: Font.Medium
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    const nextValue = root.clampedValue(root.value - root.step)
                    root.valueChangedByUser(nextValue)
                }
            }
        }

        Rectangle {
            id: track
            anchors.left: minusButton.right
            anchors.right: plusButton.left
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            height: 8
            radius: 4
            color: "#20FFFFFF"
            border.width: 1
            border.color: "#18FFFFFF"

            Rectangle {
                width: Math.max(0, parent.width * root.normalizedValue())
                height: parent.height
                radius: parent.radius
                color: "#A0FFFFFF"
            }

            Rectangle {
                width: 14
                height: 14
                radius: width / 2
                x: root.normalizedValue() * Math.max(0, parent.width - width)
                anchors.verticalCenter: parent.verticalCenter
                color: "white"
                border.width: 1
                border.color: "#26000000"
                antialiasing: true
            }
        }

        Rectangle {
            id: plusButton
            width: 42
            height: 34
            radius: 17
            anchors.right: parent.right
            color: "#26FFFFFF"
            border.width: 1
            border.color: "#24FFFFFF"

            Text {
                anchors.centerIn: parent
                text: "+"
                color: "white"
                font.pixelSize: 18
                font.weight: Font.Medium
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    const nextValue = root.clampedValue(root.value + root.step)
                    root.valueChangedByUser(nextValue)
                }
            }
        }

        MouseArea {
            anchors.left: track.left
            anchors.right: track.right
            anchors.verticalCenter: track.verticalCenter
            height: controls.height
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor

            function updateValue(mouseX) {
                root.valueChangedByUser(root.valueForTrackX(mouseX, width))
            }

            onPressed: function(mouse) {
                updateValue(mouse.x)
            }

            onPositionChanged: function(mouse) {
                if (pressed)
                    updateValue(mouse.x)
            }
        }
    }
}
