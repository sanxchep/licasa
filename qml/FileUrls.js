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

function previewFirstFullResolution(value) {
    return ["png", "pnm", "ppm", "pgm", "pbm"].indexOf(parts(value).suffix) >= 0
}

function queryValue(value, name) {
    const text = String(value)
    const question = text.indexOf("?")
    if (question < 0)
        return ""
    const entries = text.substring(question + 1).split("#", 1)[0].split("&")
    for (let index = 0; index < entries.length; ++index) {
        const separator = entries[index].indexOf("=")
        if (separator < 0)
            continue
        if (entries[index].substring(0, separator) === name) {
            try {
                return decodeURIComponent(entries[index].substring(separator + 1))
            } catch (error) {
                return ""
            }
        }
    }
    return ""
}

function presentedParallelPairId(value) {
    if (queryValue(value, "licasa_stage") !== "preview"
            || queryValue(value, "licasa_parallel") !== "preview")
        return ""
    return queryValue(value, "licasa_pair")
}

function suggestedCopy(value, label, suffix) {
    const source = parts(value)
    if (source.stem.length === 0 || suffix.length === 0)
        return ""
    return fromLocalPath(source.folder + source.stem + "-" + label + "." + suffix)
}

function suggestedEditedCopy(value, format, writableFilters) {
    const source = parts(value)
    let suffix = ["jpg", "jpeg", "png", "webp"].indexOf(source.suffix) >= 0
        ? source.suffix : "png"
    if (source.suffix && writableFilters) {
        for (let index = 0; index < writableFilters.length; ++index) {
            if (String(writableFilters[index]).split(/[\s()]+/).indexOf("*." + source.suffix) >= 0) {
                suffix = source.suffix
                break
            }
        }
    }
    if (format === "jpeg")
        suffix = "jpg"
    else if (["png", "webp", "tiff", "bmp", "ico", "cur", "icns", "pbm", "pgm", "ppm", "wbmp", "xbm", "xpm"].indexOf(format) >= 0)
        suffix = format
    return suggestedCopy(value, "edited", suffix)
}
