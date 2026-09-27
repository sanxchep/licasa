import QtQuick

Item {
    id: root

    required property string text
    property bool highlighted: false
    property bool emphasizedText: false
    property int animationDuration: 140

    signal clicked()

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

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.highlighted ? "#42FFFFFF" : "#18FFFFFF"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus
            ? "#C8FFFFFF"
            : root.highlighted ? "#4CFFFFFF" : "#20FFFFFF"

        Behavior on color {
            ColorAnimation {
                duration: root.animationDuration
            }
        }
    }

    Text {
        anchors.centerIn: parent
        text: root.text
        color: "white"
        font.pixelSize: root.emphasizedText ? 22 : 12
        font.weight: root.emphasizedText ? Font.Medium : Font.DemiBold
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
