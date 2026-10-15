import QtQuick

Item {
    id: root

    required property string title
    property string subtitle: ""
    property bool checked: false
    property int buttonMotionDuration: 140
    property int switchMotionDuration: 160

    signal toggled(bool value)

    height: Math.max(64, textColumn.implicitHeight + 20)
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
        objectName: "toggleRowFocusFrame"
        anchors.fill: parent
        radius: 12
        color: rowMouse.containsMouse && root.enabled ? "#12FFFFFF" : "transparent"
        border.width: root.activeFocus ? 2 : 0
        border.color: "#C8FFFFFF"
    }

    Column {
        id: textColumn
        x: 12
        width: Math.max(1, root.width - toggleBox.width - 34)
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Text {
            id: rowTitle
            objectName: "toggleRowTitle"
            width: parent.width
            text: root.title
            color: "white"
            font.pixelSize: 14
            font.weight: Font.Medium
            wrapMode: Text.Wrap
        }

        Text {
            id: rowSubtitle
            objectName: "toggleRowSubtitle"
            width: parent.width
            text: root.subtitle
            color: "#B3FFFFFF"
            font.pixelSize: 12
            wrapMode: Text.Wrap
            visible: text.length > 0
        }
    }

    Item {
        id: toggleBox
        objectName: "toggleRowSwitch"
        width: 52
        height: 32
        anchors.right: parent.right
        anchors.rightMargin: 10
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
        id: rowMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.toggled(!root.checked)
    }
}
