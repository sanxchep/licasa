.pragma library

// Absolute Linux paths are literal strings; only URLs carry percent encoding.
function isLocal(value) {
    if (value === undefined || value === null || typeof value.toString !== "function")
        return false
    const text = value.toString()
    return text.startsWith("/") || /^file:\/\//i.test(text)
}

function localPath(value) {
    if (!isLocal(value))
        return ""
    const text = value.toString()
    if (text.startsWith("/"))
        return text

    let encoded = text.substring(7).split(/[?#]/, 1)[0]
    if (encoded.length === 0)
        return ""
    // Match QUrl::toLocalFile for an explicit file-URL authority.
    if (!encoded.startsWith("/"))
        encoded = "//" + encoded
    try {
        return decodeURIComponent(encoded)
    } catch (error) {
        return ""
    }
}

function fromLocalPath(path) {
    if (typeof path !== "string" || !path.startsWith("/"))
        return ""
    return "file://" + path.split("/").map(encodeURIComponent).join("/")
}

function fileName(value) {
    const path = localPath(value)
    return path.substring(path.lastIndexOf("/") + 1)
}

function parts(value) {
    const path = localPath(value)
    const separator = path.lastIndexOf("/")
    const name = path.substring(separator + 1)
    const dot = name.lastIndexOf(".")
    return {
        "folder": path.substring(0, separator + 1),
        "stem": dot > 0 ? name.substring(0, dot) : name,
        "suffix": dot > 0 ? name.substring(dot + 1).toLowerCase() : ""
    }
}

function suggestedCopy(value, label, suffix) {
    const source = parts(value)
    if (source.stem.length === 0 || suffix.length === 0)
        return ""
    return fromLocalPath(source.folder + source.stem + "-" + label + "." + suffix)
}

function suggestedEditedCopy(value, format) {
    const source = parts(value)
    let suffix = ["jpg", "jpeg", "png", "webp"].indexOf(source.suffix) >= 0
        ? source.suffix : "png"
    if (format === "jpeg")
        suffix = "jpg"
    else if (format === "png" || format === "webp")
        suffix = format
    return suggestedCopy(value, "edited", suffix)
}
