import QtQuick

Item {
    id: root

    required property string title
    property string subtitle: ""
    property bool checked: false
    property int buttonMotionDuration: 140
    property int switchMotionDuration: 160

    signal toggled(bool value)

    readonly property real textBottom: rowSubtitle.visible
        ? rowSubtitle.y + rowSubtitle.height
        : rowTitle.y + rowTitle.height

    height: Math.max(58, textBottom + 8)
    activeFocusOnTab: enabled && visible

    Accessible.role: Accessible.CheckBox
    Accessible.name: title
    Accessible.description: subtitle
    Accessible.checked: checked
    Accessible.focusable: enabled && visible
    Accessible.focused: activeFocus
    Accessible.onPressAction: root.toggled(!root.checked)

    Keys.onPressed: function(event) {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                && event.key !== Qt.Key_Enter)
            return
        if (!event.isAutoRepeat)
            root.toggled(!root.checked)
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: "transparent"
        border.width: root.activeFocus ? 2 : 0
        border.color: "#C8FFFFFF"
    }

    Text {
        id: rowTitle
        anchors.left: parent.left
        anchors.right: toggleBox.left
        anchors.rightMargin: 12
        anchors.top: parent.top
        text: root.title
        color: "white"
        font.pixelSize: 14
        font.weight: Font.Medium
        wrapMode: Text.Wrap
    }

    Text {
        id: rowSubtitle
        anchors.left: parent.left
        anchors.right: toggleBox.left
        anchors.rightMargin: 12
        anchors.top: rowTitle.bottom
        anchors.topMargin: 2
        text: root.subtitle
        color: "#99FFFFFF"
        font.pixelSize: 11
        wrapMode: Text.Wrap
        visible: text.length > 0
    }

    Item {
        id: toggleBox
        width: 52
        height: 32
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter

        Rectangle {
            id: track
            anchors.fill: parent
            radius: height / 2
            color: root.checked ? "#34C759" : "#5AFFFFFF"
            border.width: 1
            border.color: root.checked ? "#4DFFFFFF" : "#24FFFFFF"

            Behavior on color {
                ColorAnimation {
                    duration: root.buttonMotionDuration
                }
            }
        }

        Rectangle {
            width: 28
            height: 28
            radius: 14
            y: 2
            x: root.checked ? parent.width - width - 2 : 2
            color: "white"
            antialiasing: true

            Behavior on x {
                NumberAnimation {
                    duration: root.switchMotionDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.toggled(!root.checked)
    }
}
