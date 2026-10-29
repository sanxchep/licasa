pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control
    property string accessibleLabel: ""
    height: 42
    textRole: "text"
    valueRole: "value"
    font.pixelSize: 13
    Accessible.name: accessibleLabel
    Keys.onShortcutOverride: function(event) {
        if (event.modifiers === Qt.NoModifier && (event.key !== Qt.Key_Escape || popup.visible))
            event.accepted = true
    }
    padding: 12
    rightPadding: 32
    background: Rectangle {
        radius: 10
        color: control.hovered ? "#343434" : "#282828"
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? "#B8FFFFFF" : "#32FFFFFF"
    }
    contentItem: Text {
        text: control.displayText
        color: "white"
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Text {
        x: control.width - width - 12
        y: (control.height - height) / 2
        text: "⌄"
        color: "#C8FFFFFF"
        font.pixelSize: 18
    }
    delegate: ItemDelegate {
        id: entry
        required property int index
        required property var modelData
        width: control.width - 8
        height: 40
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: entry.modelData.text
            color: "white"
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 8
            color: entry.highlighted ? "#38FFFFFF" : "transparent"
        }
    }
    popup: Popup {
        y: control.height + 6
        width: control.width
        padding: 4
        margins: 8
        implicitHeight: Math.min(280, contentItem.implicitHeight + 8)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle {
            radius: 12
            color: "#282828"
            border.width: 1
            border.color: "#50FFFFFF"
        }
    }
}
