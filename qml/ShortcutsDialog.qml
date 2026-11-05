pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import "components"
import "ShortcutCatalog.js" as ShortcutCatalog

FocusScope {
    id: root
    objectName: "keyboardShortcutsDialog"
    required property Item backdropSource
    property bool smoothMotionEnabled: true
    property bool opened: false
    property bool closing: false
    readonly property bool blurAvailable: GraphicsInfo.api !== GraphicsInfo.Software
        && GraphicsInfo.api !== GraphicsInfo.Unknown
    readonly property int motionDuration: smoothMotionEnabled ? 140 : 0
    signal dismissed()

    anchors.fill: parent
    focus: true
    opacity: opened ? 1 : 0

    function closeHelp() {
        if (closing) return
        closing = true
        opened = false
        dismissTimer.restart()
    }

    function scrollBy(amount) {
        list.contentY = Math.max(0, Math.min(Math.max(0, list.contentHeight - list.height),
            list.contentY + amount))
    }

    // Accept the override as well as the key: Window shortcuts are resolved
    // before ordinary key handling, including during the closing transition.
    Keys.onShortcutOverride: function(event) { event.accepted = true }
    Keys.onPressed: function(event) {
        event.accepted = true
        if (event.key === Qt.Key_Escape) {
            if (!event.isAutoRepeat) closeHelp()
        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            if (closeButton.activeFocus) listFocus.forceActiveFocus(Qt.TabFocusReason)
            else closeButton.forceActiveFocus(Qt.BacktabFocusReason)
        } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_Space) {
            scrollBy(list.height * 0.8)
        } else if (event.key === Qt.Key_PageUp) {
            scrollBy(-list.height * 0.8)
        } else if (event.key === Qt.Key_Down) {
            scrollBy(48)
        } else if (event.key === Qt.Key_Up) {
            scrollBy(-48)
        } else if (event.key === Qt.Key_Home) {
            list.contentY = 0
        } else if (event.key === Qt.Key_End) {
            list.contentY = Math.max(0, list.contentHeight - list.height)
        }
    }

    Behavior on opacity { NumberAnimation { duration: root.motionDuration } }
    Timer { id: dismissTimer; interval: Math.max(1, root.motionDuration); onTriggered: root.dismissed() }

    ShaderEffectSource {
        id: capture
        objectName: "shortcutsBackdropCapture"
        visible: false
        sourceItem: root.blurAvailable ? root.backdropSource : null
        live: false
        textureSize.width: Math.max(1, Math.round(root.width * Math.min(1, 640 / Math.max(1, root.width), 384 / Math.max(1, root.height))))
        textureSize.height: Math.max(1, Math.round(root.height * Math.min(1, 640 / Math.max(1, root.width), 384 / Math.max(1, root.height))))
        onTextureSizeChanged: if (root.blurAvailable) scheduleUpdate()
    }
    ShaderEffect {
        anchors.fill: parent
        visible: root.blurAvailable
        property var source: capture
        property vector2d texelStep: Qt.vector2d(1 / capture.textureSize.width, 1 / capture.textureSize.height)
        fragmentShader: "qrc:/Licasa/assets/shaders/shortcuts-blur.frag.qsb"
    }
    Rectangle { anchors.fill: parent; color: root.blurAvailable ? "#A6000000" : "#C4000000" }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }

    Rectangle {
        id: card
        objectName: "shortcutsCard"
        width: Math.max(1, Math.min(720, root.width - (root.width < 480 ? 24 : 64)))
        height: Math.max(1, Math.min(680, root.height - (root.height < 480 ? 24 : 64)))
        anchors.centerIn: parent
        radius: 22
        color: "#202020"
        border.width: 1
        border.color: "#40FFFFFF"
        clip: true
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Keyboard shortcuts")
        Accessible.description: qsTr("Browse shortcuts by category. Escape closes this dialog and returns to your previous view.")

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: function(wheel) { root.scrollBy(-(wheel.pixelDelta.y || wheel.angleDelta.y / 120 * 72)); wheel.accepted = true }
        }
        Item {
            id: header
            objectName: "shortcutsHeader"
            x: 20
            width: parent.width - 40
            height: Math.max(68, heading.implicitHeight + 32)
            Text {
                id: heading
                anchors.left: parent.left
                anchors.right: closeButton.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Keyboard shortcuts")
                maximumLineCount: 2
                elide: Text.ElideRight
                wrapMode: Text.Wrap
                color: "white"
                font.pixelSize: 21
                font.weight: Font.DemiBold
                Accessible.ignored: true
            }
            RoundIconButton {
                id: closeButton
                objectName: "closeKeyboardShortcutsButton"
                width: 36
                height: 36
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                iconName: "close"
                accessibleName: qsTr("Close keyboard shortcuts")
                animationDuration: root.motionDuration
                onClicked: root.closeHelp()
            }
        }
        Rectangle { x: 20; y: header.height; width: parent.width - 40; height: 1; color: "#24FFFFFF" }
        FocusScope {
            id: listFocus
            objectName: "shortcutsListFocus"
            x: 12
            y: header.height + 9
            width: parent.width - 24
            height: Math.max(1, parent.height - y - 12)
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Shortcut categories")
            Accessible.description: qsTr("Use arrow keys or Page Up and Page Down to scroll.")
            Accessible.focusable: true
            Accessible.focused: activeFocus
            Rectangle {
                anchors.fill: parent
                radius: 10
                color: "transparent"
                border.width: listFocus.activeFocus ? 1 : 0
                border.color: "#B8FFFFFF"
            }
            Flickable {
                id: list
                objectName: "shortcutsList"
                anchors.fill: parent
                anchors.margins: 8
                anchors.rightMargin: 18
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: groups.height
                Column {
                    id: groups
                    width: list.width
                    spacing: 24
                    Repeater {
                        model: ShortcutCatalog.groups()
                        delegate: Column {
                            id: category
                            required property var modelData
                            width: groups.width
                            spacing: 8
                            Text {
                                width: parent.width
                                text: category.modelData.title
                                color: "#E8FFFFFF"
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                                Accessible.role: Accessible.Heading
                                Accessible.name: text + (category.modelData.note ? ". " + category.modelData.note : "")
                            }
                            Text {
                                width: parent.width
                                visible: text.length > 0
                                text: category.modelData.note || ""
                                color: "#BBBBBB"
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                Accessible.ignored: true
                            }
                            Repeater {
                                model: category.modelData.rows
                                delegate: Rectangle {
                                    id: entry
                                    required property var modelData
                                    width: category.width
                                    height: rowContents.height + 20
                                    radius: 10
                                    color: "#292929"
                                    Accessible.role: Accessible.StaticText
                                    Accessible.name: modelData.action + ". " + modelData.keys.map(ShortcutCatalog.spokenKey).join(", ")
                                        + (modelData.note ? ". " + modelData.note : "")
                                    Column {
                                        id: rowContents
                                        x: 12
                                        y: 10
                                        width: parent.width - 24
                                        spacing: 6
                                        Accessible.ignored: true
                                        Text {
                                            width: parent.width
                                            text: entry.modelData.action
                                            color: "white"
                                            font.pixelSize: 13
                                            wrapMode: Text.Wrap
                                            Accessible.ignored: true
                                        }
                                        Flow {
                                            width: parent.width
                                            spacing: 5
                                            Repeater {
                                                model: entry.modelData.keys
                                                delegate: Rectangle {
                                                    id: keycap
                                                    objectName: "shortcutKeycap"
                                                    required property string modelData
                                                    readonly property string displayedKey: ShortcutCatalog.displayKey(modelData, formattedKey.nativeText)
                                                    width: Math.min(rowContents.width, keyLabel.implicitWidth + 16)
                                                    height: keyLabel.implicitHeight + 8
                                                    color: "#383838"
                                                    radius: 5
                                                    border.width: 1
                                                    border.color: "#28FFFFFF"
                                                    Accessible.ignored: true
                                                    Shortcut { id: formattedKey; enabled: false; sequence: keycap.modelData }
                                                    Text {
                                                        id: keyLabel
                                                        x: 8
                                                        y: 4
                                                        width: parent.width - 16
                                                        text: keycap.displayedKey
                                                        color: "#EBEBEB"
                                                        font.pixelSize: 12
                                                        wrapMode: Text.Wrap
                                                        Accessible.ignored: true
                                                    }
                                                }
                                            }
                                        }
                                        Text {
                                            width: parent.width
                                            visible: text.length > 0
                                            text: entry.modelData.note
                                            color: "#BBBBBB"
                                            font.pixelSize: 12
                                            wrapMode: Text.Wrap
                                            Accessible.ignored: true
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            InteractiveScrollBar {
                anchors.right: parent.right
                anchors.top: list.top
                anchors.bottom: list.bottom
                flickable: list
            }
        }
    }
    Component.onCompleted: {
        opened = true
        closeButton.forceActiveFocus(Qt.PopupFocusReason)
        if (blurAvailable) capture.scheduleUpdate()
    }
}
