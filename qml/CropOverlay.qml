pragma ComponentBehavior: Bound

/*
    CropOverlay.qml
    ----------------
    A modal crop canvas with free creation, movable selection, eight handles,
    optional aspect locking, composition guides, and an adjustable crop shield.
*/

import QtQuick

Item {
    id: root

    required property bool active
    required property real imageX
    required property real imageY
    required property real imageWidth
    required property real imageHeight
    required property real cropX
    required property real cropY
    required property real cropWidth
    required property real cropHeight

    property real aspectRatio: 0.0
    property int outputWidth: 0
    property int outputHeight: 0
    property string estimatedFileSize: ""
    property real shieldOpacity: 0.70
    property color accentColor: "#D8FFFFFF"

    signal cropRequested(real x, real y, real width, real height)

    readonly property real minimumCropWidth: Math.min(
        0.25, Math.max(0.02, 36 / Math.max(1, imageWidth)))
    readonly property real minimumCropHeight: Math.min(
        0.25, Math.max(0.02, 36 / Math.max(1, imageHeight)))
    readonly property real selectionX: imageX + cropX * imageWidth
    readonly property real selectionY: imageY + cropY * imageHeight
    readonly property real selectionWidth: cropWidth * imageWidth
    readonly property real selectionHeight: cropHeight * imageHeight

    visible: active && imageWidth > 0 && imageHeight > 0
    enabled: visible

    function clamp(value, minimum, maximum) {
        return Math.max(minimum, Math.min(maximum, value))
    }

    function normalizedPoint(point) {
        return Qt.point(
            clamp((point.x - imageX) / Math.max(1, imageWidth), 0.0, 1.0),
            clamp((point.y - imageY) / Math.max(1, imageHeight), 0.0, 1.0)
        )
    }

    function constrainedRect(anchorX, anchorY, pointerX, pointerY) {
        const boundedAnchorX = clamp(anchorX, 0.0, 1.0)
        const boundedAnchorY = clamp(anchorY, 0.0, 1.0)
        const boundedPointerX = clamp(pointerX, 0.0, 1.0)
        const boundedPointerY = clamp(pointerY, 0.0, 1.0)
        const growsRight = boundedPointerX >= boundedAnchorX
        const growsDown = boundedPointerY >= boundedAnchorY
        const maximumWidth = growsRight ? 1.0 - boundedAnchorX : boundedAnchorX
        const maximumHeight = growsDown ? 1.0 - boundedAnchorY : boundedAnchorY

        let width = Math.min(maximumWidth,
            Math.max(minimumCropWidth, Math.abs(boundedPointerX - boundedAnchorX)))
        let height = Math.min(maximumHeight,
            Math.max(minimumCropHeight, Math.abs(boundedPointerY - boundedAnchorY)))

        if (aspectRatio > 0.0) {
            const normalizedRatio = aspectRatio * imageHeight / Math.max(1, imageWidth)
            if (normalizedRatio > 0.0) {
                // Grow enough to follow both pointer axes, then constrain back
                // inside the available image quadrant.
                if (width / Math.max(0.00001, height) > normalizedRatio)
                    height = width / normalizedRatio
                else
                    width = height * normalizedRatio

                if (width > maximumWidth) {
                    width = maximumWidth
                    height = width / normalizedRatio
                }
                if (height > maximumHeight) {
                    height = maximumHeight
                    width = height * normalizedRatio
                }
            }
        }

        width = clamp(width, Math.min(minimumCropWidth, maximumWidth), maximumWidth)
        height = clamp(height, Math.min(minimumCropHeight, maximumHeight), maximumHeight)
        return Qt.rect(
            growsRight ? boundedAnchorX : boundedAnchorX - width,
            growsDown ? boundedAnchorY : boundedAnchorY - height,
            width,
            height
        )
    }

    function requestMovedCrop(deltaX, deltaY,
                              startX, startY, startWidth, startHeight) {
        const normalizedDx = deltaX / Math.max(1, imageWidth)
        const normalizedDy = deltaY / Math.max(1, imageHeight)
        cropRequested(
            clamp(startX + normalizedDx, 0.0, 1.0 - startWidth),
            clamp(startY + normalizedDy, 0.0, 1.0 - startHeight),
            startWidth,
            startHeight
        )
    }

    function keyboardAdjust(horizontal, vertical, resize) {
        if (!root.visible)
            return
        const step = 12
        if (resize) {
            root.requestResizedCrop(4,
                Qt.point(root.selectionX + root.selectionWidth + horizontal * step,
                         root.selectionY + root.selectionHeight + vertical * step),
                root.cropX, root.cropY, root.cropWidth, root.cropHeight)
        } else {
            root.requestMovedCrop(horizontal * step, vertical * step,
                root.cropX, root.cropY, root.cropWidth, root.cropHeight)
        }
    }

    function requestResizedCrop(handleIndex, point,
                                startX, startY, startWidth, startHeight) {
        const normalized = normalizedPoint(point)
        const startRight = startX + startWidth
        const startBottom = startY + startHeight

        if (handleIndex % 2 === 0 && aspectRatio > 0.0) {
            let anchorX = startX
            let anchorY = startY
            if (handleIndex === 0) {
                anchorX = startRight
                anchorY = startBottom
            } else if (handleIndex === 2) {
                anchorX = startX
                anchorY = startBottom
            } else if (handleIndex === 4) {
                anchorX = startX
                anchorY = startY
            } else {
                anchorX = startRight
                anchorY = startY
            }
            const locked = constrainedRect(anchorX, anchorY, normalized.x, normalized.y)
            cropRequested(locked.x, locked.y, locked.width, locked.height)
            return
        }

        let nextLeft = startX
        let nextTop = startY
        let nextRight = startRight
        let nextBottom = startBottom

        if (handleIndex === 0 || handleIndex === 6 || handleIndex === 7)
            nextLeft = clamp(normalized.x, 0.0, startRight - minimumCropWidth)
        if (handleIndex === 2 || handleIndex === 3 || handleIndex === 4)
            nextRight = clamp(normalized.x, startX + minimumCropWidth, 1.0)
        if (handleIndex === 0 || handleIndex === 1 || handleIndex === 2)
            nextTop = clamp(normalized.y, 0.0, startBottom - minimumCropHeight)
        if (handleIndex === 4 || handleIndex === 5 || handleIndex === 6)
            nextBottom = clamp(normalized.y, startY + minimumCropHeight, 1.0)

        cropRequested(nextLeft,
                      nextTop,
                      nextRight - nextLeft,
                      nextBottom - nextTop)
    }

    MouseArea {
        id: createArea
        x: root.imageX
        y: root.imageY
        width: root.imageWidth
        height: root.imageHeight
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.CrossCursor

        property point pressPoint: Qt.point(0, 0)
        property bool moved: false

        onPressed: function(mouse) {
            pressPoint = mapToItem(root, mouse.x, mouse.y)
            moved = false
        }

        onPositionChanged: function(mouse) {
            if (!pressed)
                return

            const point = mapToItem(root, mouse.x, mouse.y)
            if (!moved && Math.hypot(point.x - pressPoint.x, point.y - pressPoint.y) < 5)
                return
            moved = true

            const anchor = root.normalizedPoint(pressPoint)
            const pointer = root.normalizedPoint(point)
            const next = root.constrainedRect(anchor.x, anchor.y, pointer.x, pointer.y)
            root.cropRequested(next.x, next.y, next.width, next.height)
        }
    }

    // A crop shield is four simple rectangles rather than a shader or mask.
    // It leaves the kept pixels untouched and remains effectively free while
    // the crop frame is dragged.
    Item {
        id: cropShield
        anchors.fill: parent
        visible: root.shieldOpacity > 0.0
        opacity: root.clamp(root.shieldOpacity, 0.0, 1.0)
        z: 1

        Rectangle {
            objectName: "cropShieldTop"
            x: root.imageX
            y: root.imageY
            width: root.imageWidth
            height: Math.max(0, root.selectionY - root.imageY)
            color: "black"
        }

        Rectangle {
            objectName: "cropShieldBottom"
            x: root.imageX
            y: root.selectionY + root.selectionHeight
            width: root.imageWidth
            height: Math.max(0,
                root.imageY + root.imageHeight - y)
            color: "black"
        }

        Rectangle {
            objectName: "cropShieldLeft"
            x: root.imageX
            y: root.selectionY
            width: Math.max(0, root.selectionX - root.imageX)
            height: root.selectionHeight
            color: "black"
        }

        Rectangle {
            objectName: "cropShieldRight"
            x: root.selectionX + root.selectionWidth
            y: root.selectionY
            width: Math.max(0,
                root.imageX + root.imageWidth - x)
            height: root.selectionHeight
            color: "black"
        }
    }

    Rectangle {
        id: selection
        x: root.selectionX
        y: root.selectionY
        width: Math.max(1, root.selectionWidth)
        height: Math.max(1, root.selectionHeight)
        color: "transparent"
        border.width: 2
        border.color: "#FAFFFFFF"
        antialiasing: true
        z: 2

        Rectangle {
            anchors.fill: parent
            anchors.margins: 3
            color: "transparent"
            border.width: 1
            border.color: root.accentColor
            opacity: 0.72
        }

        Item {
            anchors.fill: parent
            visible: parent.width >= 100 && parent.height >= 100

            Repeater {
                model: 2

                Rectangle {
                    required property int index
                    x: selection.width * (index + 1) / 3
                    width: 1
                    height: selection.height
                    color: "#82FFFFFF"
                }
            }

            Repeater {
                model: 2

                Rectangle {
                    required property int index
                    y: selection.height * (index + 1) / 3
                    width: selection.width
                    height: 1
                    color: "#82FFFFFF"
                }
            }
        }

        Rectangle {
            visible: root.outputWidth > 0 && parent.width > 150 && parent.height > 80
            x: 10
            y: 10
            width: Math.min(parent.width - 20, sizeText.implicitWidth + 18)
            height: 28
            radius: 9
            color: "#C40A0C0F"
            border.width: 1
            border.color: "#30FFFFFF"

            Text {
                id: sizeText
                anchors.fill: parent
                anchors.leftMargin: 9
                anchors.rightMargin: 9
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                text: root.outputWidth + " × " + root.outputHeight
                    + (root.estimatedFileSize.length > 0
                        ? "  (" + root.estimatedFileSize + ")"
                        : "")
                color: "white"
                font.pixelSize: 11
                font.weight: Font.Medium
                renderType: Text.NativeRendering
                elide: Text.ElideRight
            }
        }

        MouseArea {
            id: moveArea
            anchors.fill: parent
            anchors.margins: 16
            cursorShape: Qt.SizeAllCursor
            z: 3

            property point pressPoint: Qt.point(0, 0)
            property real startX: 0.0
            property real startY: 0.0
            property real startWidth: 1.0
            property real startHeight: 1.0

            onPressed: function(mouse) {
                pressPoint = mapToItem(root, mouse.x, mouse.y)
                startX = root.cropX
                startY = root.cropY
                startWidth = root.cropWidth
                startHeight = root.cropHeight
            }

            onPositionChanged: function(mouse) {
                if (!pressed)
                    return
                const point = mapToItem(root, mouse.x, mouse.y)
                root.requestMovedCrop(
                    point.x - pressPoint.x,
                    point.y - pressPoint.y,
                    startX,
                    startY,
                    startWidth,
                    startHeight
                )
            }
        }
    }

    // Make the complete edge draggable, not just the visible midpoint grip.
    // These generous invisible hit bands sit above the move surface and below
    // the corner grips, so precision never depends on landing on a tiny mark.
    Repeater {
        model: [1, 3, 5, 7]

        Item {
            id: edgeZone
            required property int modelData

            readonly property bool horizontalEdge: modelData === 1 || modelData === 5
            readonly property bool leadingEdge: modelData === 1 || modelData === 7

            visible: root.aspectRatio <= 0.0
            x: horizontalEdge
                ? root.selectionX + 10
                : (leadingEdge ? root.selectionX - 12
                               : root.selectionX + root.selectionWidth - 12)
            y: horizontalEdge
                ? (leadingEdge ? root.selectionY - 12
                               : root.selectionY + root.selectionHeight - 12)
                : root.selectionY + 10
            width: horizontalEdge ? Math.max(1, root.selectionWidth - 20) : 24
            height: horizontalEdge ? 24 : Math.max(1, root.selectionHeight - 20)
            z: 4

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                cursorShape: edgeZone.horizontalEdge ? Qt.SizeVerCursor : Qt.SizeHorCursor

                property real startX: 0.0
                property real startY: 0.0
                property real startWidth: 1.0
                property real startHeight: 1.0

                onPressed: {
                    startX = root.cropX
                    startY = root.cropY
                    startWidth = root.cropWidth
                    startHeight = root.cropHeight
                }

                onPositionChanged: function(mouse) {
                    if (!pressed)
                        return
                    root.requestResizedCrop(
                        edgeZone.modelData,
                        mapToItem(root, mouse.x, mouse.y),
                        startX,
                        startY,
                        startWidth,
                        startHeight
                    )
                }
            }
        }
    }

    Repeater {
        model: 8

        Rectangle {
            id: handle
            required property int index

            readonly property bool cornerHandle: index % 2 === 0
            readonly property bool leftHandle: index === 0 || index === 6 || index === 7
            readonly property bool rightHandle: index === 2 || index === 3 || index === 4
            readonly property bool topHandle: index === 0 || index === 1 || index === 2
            readonly property bool bottomHandle: index === 4 || index === 5 || index === 6
            readonly property bool horizontalEdge: index === 1 || index === 5
            readonly property bool verticalEdge: index === 3 || index === 7

            visible: root.aspectRatio <= 0.0 || cornerHandle
            width: cornerHandle ? 20 : horizontalEdge ? 34 : 12
            height: cornerHandle ? 20 : verticalEdge ? 34 : 12
            radius: cornerHandle ? 6 : 5
            x: root.selectionX
                + (leftHandle ? 0 : rightHandle ? root.selectionWidth : root.selectionWidth / 2)
                - width / 2
            y: root.selectionY
                + (topHandle ? 0 : bottomHandle ? root.selectionHeight : root.selectionHeight / 2)
                - height / 2
            color: handleMouse.containsMouse || handleMouse.pressed
                ? root.accentColor : "white"
            border.width: 2
            border.color: "#70000000"
            antialiasing: true
            z: 5

            MouseArea {
                id: handleMouse
                anchors.fill: parent
                anchors.margins: -10
                hoverEnabled: true
                cursorShape: handle.index === 0 || handle.index === 4
                    ? Qt.SizeFDiagCursor
                    : handle.index === 2 || handle.index === 6
                        ? Qt.SizeBDiagCursor
                        : handle.index === 1 || handle.index === 5
                            ? Qt.SizeVerCursor
                            : Qt.SizeHorCursor

                property real startX: 0.0
                property real startY: 0.0
                property real startWidth: 1.0
                property real startHeight: 1.0

                onPressed: function(mouse) {
                    startX = root.cropX
                    startY = root.cropY
                    startWidth = root.cropWidth
                    startHeight = root.cropHeight
                }

                onPositionChanged: function(mouse) {
                    if (!pressed)
                        return
                    const point = mapToItem(root, mouse.x, mouse.y)
                    root.requestResizedCrop(
                        handle.index,
                        point,
                        startX,
                        startY,
                        startWidth,
                        startHeight
                    )
                }
            }
        }
    }
}
