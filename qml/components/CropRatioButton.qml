import QtQuick

Item {
    id: root
    required property string text
    property real ratio: 0
    property bool checked: false
    property bool custom: false
    property int animationDuration: 140
    signal clicked()

    implicitWidth: 72
    implicitHeight: 68
    activeFocusOnTab: enabled && visible
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: checked ? "Selected aspect ratio" : "Use this aspect ratio"
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.focusable: enabled && visible
    Accessible.focused: activeFocus
    Accessible.onPressAction: activate()

    function activate() {
        if (!enabled || !visible) return
        forceActiveFocus()
        clicked()
    }
    Keys.onPressed: function(event) {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                && event.key !== Qt.Key_Enter) return
        if (!event.isAutoRepeat) activate()
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        radius: 11
        color: root.checked ? "#143C2A" : pointer.containsMouse ? "#343434" : "#282828"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? "#DDFFFFFF" : root.checked ? "#55BC80" : "#32FFFFFF"
        Behavior on color { ColorAnimation { duration: root.animationDuration } }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 11 + (20 - height) / 2
            width: root.ratio > 0 ? Math.max(3, Math.min(30, 20 * root.ratio)) : 28
            height: root.ratio > 0 ? Math.max(3, Math.min(20, 30 / root.ratio)) : 20
            radius: 3
            color: root.checked ? "#24573A" : "#0AFFFFFF"
            border.width: 1
            border.color: root.checked ? "#A1E5B8" : "#A8FFFFFF"
            Text {
                anchors.centerIn: parent
                visible: root.custom
                text: "···"
                color: root.checked ? "#A1E5B8" : "#B8FFFFFF"
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            text: root.text
            color: root.checked ? "#A1E5B8" : "#DDFFFFFF"
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 11
            font.weight: Font.Medium
            renderType: Text.NativeRendering
        }
    }
    MouseArea {
        id: pointer
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.activate()
    }
}
