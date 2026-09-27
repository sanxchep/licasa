.pragma library

function clamp(value, minimum, maximum) {
    return Number.isFinite(value)
        ? Math.max(minimum, Math.min(maximum, value))
        : minimum
}

function fitScale(imageWidth, imageHeight, maxWidth, maxHeight, minimumScale, maximumScale) {
    if (!Number.isFinite(imageWidth) || !Number.isFinite(imageHeight)
            || !Number.isFinite(maxWidth) || !Number.isFinite(maxHeight)
            || imageWidth <= 0 || imageHeight <= 0)
        return 1.0

    const horizontalScale = Math.max(1.0, maxWidth) / imageWidth
    const verticalScale = Math.max(1.0, maxHeight) / imageHeight
    return clamp(Math.min(horizontalScale, verticalScale), minimumScale, maximumScale)
}

function wheelFactor(sensitivity, rawDelta, maximumDelta) {
    if (!Number.isFinite(sensitivity) || sensitivity <= 0
            || !Number.isFinite(rawDelta))
        return 1.0

    const maximum = Number.isFinite(maximumDelta) ? Math.abs(maximumDelta) : 0
    const delta = clamp(rawDelta, -maximum, maximum)
    return Math.pow(sensitivity, delta)
}

function scaledExtent(imageExtent, scale) {
    if (!Number.isFinite(imageExtent) || !Number.isFinite(scale))
        return 1
    return Math.max(1, Math.ceil(Math.max(0, imageExtent) * Math.max(0, scale)))
}

function constrainRectToBounds(x, y, width, height,
                               boundsX, boundsY, boundsWidth, boundsHeight) {
    const rectWidth = Math.max(0, width)
    const rectHeight = Math.max(0, height)
    const availableWidth = Math.max(0, boundsWidth)
    const availableHeight = Math.max(0, boundsHeight)
    const constrainedX = rectWidth <= availableWidth
        ? clamp(x, boundsX, boundsX + availableWidth - rectWidth)
        : boundsX
    const constrainedY = rectHeight <= availableHeight
        ? clamp(y, boundsY, boundsY + availableHeight - rectHeight)
        : boundsY

    return {
        "x": constrainedX,
        "y": constrainedY,
        "width": rectWidth,
        "height": rectHeight
    }
}

function freeImagePosition(position, scaledExtentValue, viewportExtent, requestedGrip) {
    const extent = Math.max(0, scaledExtentValue)
    const viewport = Math.max(0, viewportExtent)
    const visibleGrip = Math.min(Math.max(0, requestedGrip), extent)
    return clamp(position, visibleGrip - extent, viewport - visibleGrip)
}

function visibleIntersectionExtent(itemPosition, itemExtent,
                                   viewportPosition, viewportExtent) {
    const itemStart = itemPosition
    const itemEnd = itemPosition + Math.max(0, itemExtent)
    const viewportStart = viewportPosition
    const viewportEnd = viewportPosition + Math.max(0, viewportExtent)
    return Math.max(0, Math.min(itemEnd, viewportEnd) - Math.max(itemStart, viewportStart))
}

function needsViewportRecovery(imageX, imageY, imageWidth, imageHeight,
                               viewportX, viewportY, viewportWidth, viewportHeight,
                               requestedGrip) {
    if (!Number.isFinite(imageX) || !Number.isFinite(imageY)
            || !Number.isFinite(imageWidth) || !Number.isFinite(imageHeight)
            || imageWidth <= 0 || imageHeight <= 0
            || viewportWidth <= 0 || viewportHeight <= 0) {
        return true
    }

    const requiredWidth = Math.min(Math.max(1, requestedGrip), imageWidth, viewportWidth)
    const requiredHeight = Math.min(Math.max(1, requestedGrip), imageHeight, viewportHeight)
    const visibleWidth = visibleIntersectionExtent(
        imageX, imageWidth, viewportX, viewportWidth)
    const visibleHeight = visibleIntersectionExtent(
        imageY, imageHeight, viewportY, viewportHeight)

    return visibleWidth < requiredWidth || visibleHeight < requiredHeight
}

function boundedRect(x, y, width, height, boundsWidth, boundsHeight) {
    const left = clamp(Math.floor(x), 0, Math.max(0, Math.floor(boundsWidth)))
    const top = clamp(Math.floor(y), 0, Math.max(0, Math.floor(boundsHeight)))
    const right = clamp(Math.ceil(x + width), left, Math.max(left, Math.ceil(boundsWidth)))
    const bottom = clamp(Math.ceil(y + height), top, Math.max(top, Math.ceil(boundsHeight)))

    return {
        "x": left,
        "y": top,
        "width": Math.max(0, right - left),
        "height": Math.max(0, bottom - top)
    }
}
