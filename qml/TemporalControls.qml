/*!
    TemporalControls.qml
    --------------------
    Lightweight shared timeline controls for temporal media.

    This component owns no decoder and no media session. It consumes normalized
    presentation state and emits user intent so format-specific controllers can
    remain outside QML.
*/

import QtQuick

Item {
    id: root

    property bool hasTimeline: false
    property bool playing: false
    property bool canScrub: false
    property bool canStep: canScrub
    property bool saveFrameVisible: false
    property bool canSaveFrame: false
    property bool exportMotionVisible: false
    property bool canExportMotion: false
    property bool coverFrameVisible: false
    property bool canSetCoverFrame: false
    property bool coverFramePreferred: false
    property bool currentIsPreferredCover: false
    property bool interactionEnabled: true
    property real position: 0.0
    property int currentFrame: -1
    property int frameCount: 0
    property bool timeBased: false
    property real positionMs: 0.0
    property real durationMs: 0.0
    property string timeLabel: timeBased && durationMs > 0
        ? (formatTimeMs(positionMs) + " / " + formatTimeMs(durationMs))
        : frameCount > 0 && currentFrame >= 0
            ? (Math.min(currentFrame + 1, frameCount) + " / " + frameCount)
            : ""

    signal togglePlaybackRequested()
    signal seekRequested(real normalizedPosition)
    signal stepRequested(int delta)
    signal saveFrameRequested()
    signal exportMotionRequested()
    signal coverFrameRequested()

    readonly property real clampedPosition: Math.max(0.0, Math.min(1.0, position))

    function formatTimeMs(value) {
        const safeMs = Number.isFinite(value) ? Math.max(0, Math.floor(value)) : 0
        const totalSeconds = Math.floor(safeMs / 1000)
        const seconds = totalSeconds % 60
        const totalMinutes = Math.floor(totalSeconds / 60)
        const minutes = totalMinutes % 60
        const hours = Math.floor(totalMinutes / 60)
        const paddedSeconds = seconds < 10 ? "0" + seconds : String(seconds)
        if (hours <= 0)
            return totalMinutes + ":" + paddedSeconds
        const paddedMinutes = minutes < 10 ? "0" + minutes : String(minutes)
        return hours + ":" + paddedMinutes + ":" + paddedSeconds
    }

    visible: hasTimeline
    enabled: interactionEnabled
    implicitWidth: 540
    implicitHeight: 58

    Rectangle {
        id: panel
        anchors.fill: parent
        radius: 18
        color: "#E6161616"
        border.width: 1
        border.color: "#28FFFFFF"
        antialiasing: true

        Row {
            id: transportRow
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Rectangle {
                id: previousButton
                objectName: "temporalPreviousButton"
                width: 34
                height: 34
                radius: 17
                color: previousMouse.pressed ? "#44FFFFFF"
                    : previousMouse.containsMouse ? "#2EFFFFFF" : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: "‹"
                    color: "#F0FFFFFF"
                    font.pixelSize: 25
                    font.weight: Font.Medium
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    id: previousMouse
                    anchors.fill: parent
                    enabled: root.enabled && root.canStep
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.stepRequested(-1)
                }
            }

            Rectangle {
                id: playButton
                objectName: "temporalPlayButton"
                width: 38
                height: 38
                radius: 19
                color: playMouse.pressed ? "#58FFFFFF"
                    : playMouse.containsMouse ? "#3AFFFFFF" : "#24FFFFFF"
                border.width: 1
                border.color: "#28FFFFFF"

                Text {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: root.playing ? 0 : 1
                    text: root.playing ? "Ⅱ" : "▶"
                    color: "#F4FFFFFF"
                    font.pixelSize: root.playing ? 15 : 14
                    font.weight: Font.DemiBold
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    id: playMouse
                    anchors.fill: parent
                    enabled: root.enabled
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.togglePlaybackRequested()
                }
            }

            Rectangle {
                id: nextButton
                objectName: "temporalNextButton"
                width: 34
                height: 34
                radius: 17
                color: nextMouse.pressed ? "#44FFFFFF"
                    : nextMouse.containsMouse ? "#2EFFFFFF" : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: "›"
                    color: "#F0FFFFFF"
                    font.pixelSize: 25
                    font.weight: Font.Medium
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    id: nextMouse
                    anchors.fill: parent
                    enabled: root.enabled && root.canStep
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.stepRequested(1)
                }
            }
        }

        Item {
            id: scrubArea
            anchors.left: transportRow.right
            anchors.right: timeText.left
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            height: 34

            Rectangle {
                id: scrubTrack
                objectName: "temporalScrubber"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 4
                radius: 2
                color: "#38FFFFFF"

                Rectangle {
                    width: parent.width * root.clampedPosition
                    height: parent.height
                    radius: parent.radius
                    color: "#E8FFFFFF"
                }

                Rectangle {
                    width: 12
                    height: 12
                    radius: 6
                    x: Math.round((parent.width - width) * root.clampedPosition)
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.canScrub ? "#F4FFFFFF" : "#78FFFFFF"
                }
            }

            MouseArea {
                id: scrubMouse
                objectName: "temporalScrubberMouse"
                anchors.fill: parent
                enabled: root.enabled && root.canScrub
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor

                function requestAt(mouseX) {
                    const localX = Math.max(0, Math.min(width, mouseX))
                    root.seekRequested(width > 0 ? localX / width : 0.0)
                }

                onPressed: function(mouse) { requestAt(mouse.x) }
                onPositionChanged: function(mouse) {
                    if (pressed)
                        requestAt(mouse.x)
                }
            }
        }

        Rectangle {
            id: exportMotionButton
            objectName: "temporalExportMotionButton"
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 58
            height: 30
            radius: 15
            visible: root.exportMotionVisible
            color: exportMotionMouse.pressed ? "#58FFFFFF"
                : exportMotionMouse.containsMouse ? "#3AFFFFFF" : "#24FFFFFF"
            border.width: 1
            border.color: "#28FFFFFF"

            Text {
                anchors.centerIn: parent
                text: "Export"
                color: root.canExportMotion && root.enabled ? "#F4FFFFFF" : "#78FFFFFF"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                renderType: Text.NativeRendering
            }

            MouseArea {
                id: exportMotionMouse
                anchors.fill: parent
                enabled: root.enabled && root.canExportMotion
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.exportMotionRequested()
            }
        }

        Rectangle {
            id: saveFrameButton
            objectName: "temporalSaveFrameButton"
            anchors.right: exportMotionButton.visible ? exportMotionButton.left : parent.right
            anchors.rightMargin: exportMotionButton.visible ? 6 : 10
            anchors.verticalCenter: parent.verticalCenter
            width: 48
            height: 30
            radius: 15
            visible: root.saveFrameVisible
            color: saveFrameMouse.pressed ? "#58FFFFFF"
                : saveFrameMouse.containsMouse ? "#3AFFFFFF" : "#24FFFFFF"
            border.width: 1
            border.color: "#28FFFFFF"

            Text {
                anchors.centerIn: parent
                text: "Save"
                color: root.canSaveFrame && root.enabled ? "#F4FFFFFF" : "#78FFFFFF"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                renderType: Text.NativeRendering
            }

            MouseArea {
                id: saveFrameMouse
                anchors.fill: parent
                enabled: root.enabled && root.canSaveFrame
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.saveFrameRequested()
            }
        }

        Rectangle {
            id: coverFrameButton
            objectName: "temporalCoverFrameButton"
            anchors.right: saveFrameButton.visible ? saveFrameButton.left
                : exportMotionButton.visible ? exportMotionButton.left : parent.right
            anchors.rightMargin: (saveFrameButton.visible || exportMotionButton.visible) ? 6 : 10
            anchors.verticalCenter: parent.verticalCenter
            width: 58
            height: 30
            radius: 15
            visible: root.coverFrameVisible
            color: coverFrameMouse.pressed ? "#58FFFFFF"
                : coverFrameMouse.containsMouse ? "#3AFFFFFF"
                : root.coverFramePreferred ? "#34FFFFFF" : "#24FFFFFF"
            border.width: 1
            border.color: root.coverFramePreferred ? "#70FFFFFF" : "#28FFFFFF"

            Text {
                anchors.centerIn: parent
                text: root.currentIsPreferredCover ? "Cover ✓"
                    : root.coverFramePreferred ? "Cover •" : "Cover"
                color: root.canSetCoverFrame && root.enabled ? "#F4FFFFFF" : "#78FFFFFF"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                renderType: Text.NativeRendering
            }

            MouseArea {
                id: coverFrameMouse
                anchors.fill: parent
                enabled: root.enabled && root.canSetCoverFrame
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.coverFrameRequested()
            }
        }

        Text {
            id: timeText
            objectName: "temporalTimeLabel"
            anchors.right: coverFrameButton.visible ? coverFrameButton.left
                : saveFrameButton.visible ? saveFrameButton.left
                : exportMotionButton.visible ? exportMotionButton.left : parent.right
            anchors.rightMargin: (coverFrameButton.visible || saveFrameButton.visible
                || exportMotionButton.visible) ? 8 : 14
            anchors.verticalCenter: parent.verticalCenter
            width: 62
            horizontalAlignment: Text.AlignRight
            text: root.timeLabel
            color: "#CFFFFFFF"
            font.pixelSize: 11
            font.weight: Font.Medium
            renderType: Text.NativeRendering
            elide: Text.ElideLeft
        }
    }
}
