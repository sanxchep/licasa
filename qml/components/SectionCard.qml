import QtQuick

Rectangle {
    id: root

    property string title: ""
    property string subtitle: ""
    property real contentTopSpacing: 14
    default property alias content: bodyColumn.data

    radius: 22
    color: "#1F1F1F"
    border.width: 1
    border.color: "#1EFFFFFF"
    antialiasing: true
    implicitHeight: bodyColumn.y + bodyColumn.implicitHeight + 16

    Text {
        id: sectionTitle
        x: 16
        y: 16
        width: Math.max(1, root.width - 32)
        height: implicitHeight
        text: root.title
        visible: text.length > 0
        color: "white"
        font.pixelSize: 14
        font.weight: Font.DemiBold
        wrapMode: Text.Wrap
        renderType: Text.NativeRendering
    }

    Text {
        id: sectionSubtitle
        x: 16
        y: root.title.length > 0 ? sectionTitle.y + sectionTitle.implicitHeight + 3 : 16
        width: Math.max(1, root.width - 32)
        height: implicitHeight
        text: root.subtitle
        visible: text.length > 0
        color: "#B3FFFFFF"
        font.pixelSize: 12
        wrapMode: Text.Wrap
        renderType: Text.NativeRendering
    }

    Column {
        id: bodyColumn
        x: 16
        y: root.subtitle.length > 0
            ? sectionSubtitle.y + sectionSubtitle.implicitHeight + root.contentTopSpacing
            : root.title.length > 0 ? sectionTitle.y + sectionTitle.implicitHeight + root.contentTopSpacing : 16
        width: parent.width - 32
        spacing: 12
    }
}
