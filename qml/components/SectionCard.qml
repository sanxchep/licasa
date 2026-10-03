import QtQuick

Rectangle {
    id: root

    required property string title
    property string subtitle: ""
    property real contentTopSpacing: 14
    default property alias content: bodyColumn.data

    radius: 22
    color: "#1F1F1F"
    border.width: 1
    border.color: "#1EFFFFFF"
    antialiasing: true
    implicitHeight: 16
        + sectionTitle.implicitHeight
        + (sectionSubtitle.visible ? 3 + sectionSubtitle.implicitHeight : 0)
        + contentTopSpacing
        + bodyColumn.implicitHeight
        + 16

    Text {
        id: sectionTitle
        x: 16
        y: 16
        width: Math.max(1, root.width - 32)
        height: implicitHeight
        text: root.title
        color: "white"
        font.pixelSize: 14
        font.weight: Font.DemiBold
        wrapMode: Text.Wrap
        renderType: Text.NativeRendering
    }

    Text {
        id: sectionSubtitle
        x: 16
        y: sectionTitle.y + sectionTitle.implicitHeight + 3
        width: Math.max(1, root.width - 32)
        height: implicitHeight
        text: root.subtitle
        visible: text.length > 0
        color: "#93FFFFFF"
        font.pixelSize: 11
        wrapMode: Text.Wrap
        renderType: Text.NativeRendering
    }

    Column {
        id: bodyColumn
        x: 16
        y: sectionSubtitle.visible
            ? sectionSubtitle.y + sectionSubtitle.implicitHeight + root.contentTopSpacing
            : sectionTitle.y + sectionTitle.implicitHeight + root.contentTopSpacing
        width: parent.width - 32
        spacing: 12
    }
}
