import QtQuick

Item {
    id: root

    required property string text
    property bool checked: false
    property color accent: "#7AFFFFFF"
    property int animationDuration: 140

    signal clicked()

    width: chipText.implicitWidth + 22
    height: 34
    opacity: enabled ? 1.0 : 0.42
    activeFocusOnTab: enabled

    Accessible.role: Accessible.Button
    Accessible.name: text
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

    Behavior on opacity {
        NumberAnimation { duration: root.animationDuration }
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.checked ? root.accent : "#15FFFFFF"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus
            ? "#C8FFFFFF"
            : root.checked ? "#40FFFFFF" : "#24FFFFFF"

        Behavior on color {
            ColorAnimation {
                duration: root.animationDuration
            }
        }
    }

    Text {
        id: chipText
        anchors.centerIn: parent
        text: root.text
        color: "white"
        font.pixelSize: 12
        font.weight: Font.Medium
        renderType: Text.NativeRendering
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.clicked()
    }
}
