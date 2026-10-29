import QtQuick

Item {
    id: root

    required property string text
    property string subtitle: ""
    property bool checked: false
    property bool checkable: false
    property color accent: "#7AFFFFFF"
    property bool destructive: false
    property bool affirmative: false
    property bool sectionStyle: false
    property int animationDuration: 140

    signal clicked()

    function activate() {
        if (!enabled || !visible) return
        forceActiveFocus()
        clicked()
    }

    width: chipText.implicitWidth + 22
    height: sectionStyle ? Math.max(64, chipLabels.implicitHeight + 20) : 34
    opacity: enabled ? 1.0 : 0.42
    activeFocusOnTab: enabled

    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: subtitle
    Accessible.focusable: enabled
    Accessible.focused: activeFocus
    Accessible.checkable: checkable
    Accessible.checked: checkable && checked
    Accessible.onPressAction: activate()

    Keys.onPressed: function(event) {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                && event.key !== Qt.Key_Enter) {
            return
        }
        if (!event.isAutoRepeat)
            root.activate()
        event.accepted = true
    }

    Behavior on opacity {
        NumberAnimation { duration: root.animationDuration }
    }

    Rectangle {
        anchors.fill: parent
        radius: root.sectionStyle ? 12 : height / 2
        color: root.sectionStyle
            ? (chipMouse.containsMouse
                ? (root.destructive ? "#B32632" : root.affirmative ? "#247A48" : "#26FFFFFF")
                : root.affirmative ? "#143C2A" : "transparent")
            : root.destructive ? "#B32632"
                : root.checked ? root.accent : "#15FFFFFF"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus
            ? "#C8FFFFFF"
            : root.destructive ? "#E87C85"
                : root.affirmative ? "#55BC80"
                : root.checked ? "#40FFFFFF" : "#24FFFFFF"

        Behavior on color {
            ColorAnimation {
                duration: root.animationDuration
            }
        }
    }

    Column {
        id: chipLabels
        anchors.verticalCenter: parent.verticalCenter
        x: root.sectionStyle ? 12 : 11
        width: root.width - 2 * x
        spacing: 2

        Text {
            id: chipText
            width: parent.width
            text: root.text
            color: root.sectionStyle && root.destructive && !chipMouse.containsMouse
                ? "#FFADB5" : root.sectionStyle && root.affirmative && !chipMouse.containsMouse
                    ? "#A1E5B8" : "white"
            horizontalAlignment: root.sectionStyle ? Text.AlignLeft : Text.AlignHCenter
            wrapMode: root.sectionStyle ? Text.Wrap : Text.NoWrap
            font.pixelSize: root.sectionStyle ? 14 : 12
            font.weight: Font.Medium
            renderType: Text.NativeRendering
        }

        Text {
            width: parent.width
            visible: root.sectionStyle && text.length > 0
            text: root.subtitle
            color: root.destructive && chipMouse.containsMouse ? "#E6FFFFFF" : "#B3FFFFFF"
            wrapMode: Text.Wrap
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }

    MouseArea {
        id: chipMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.clicked()
    }
}
