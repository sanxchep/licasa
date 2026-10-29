pragma ComponentBehavior: Bound
import QtQuick
import "components"

Column {
    id: root
    required property string exportFormat
    required property real exportScale
    required property int exportQuality
    required property int outputWidth
    required property int outputHeight
    required property string saveStatus
    required property int buttonMotionDuration
    property var exportFormats: []
    property int unscaledWidth: Math.max(1, Math.round(outputWidth / exportScale))
    property int unscaledHeight: Math.max(1, Math.round(outputHeight / exportScale))
    property string sourceFormat: "png"
    readonly property string effectiveFormat: exportFormat === "original" ? sourceFormat : exportFormat
    property var exportSettings: ({})
    property string estimatedSize: ""
    property bool saveBusy: false
    property bool hasImage: true
    property bool advancedOpen: false
    readonly property bool icon: root.contains(["ico", "cur", "icns"], effectiveFormat)
    readonly property bool mono: root.contains(["pbm", "wbmp", "xbm"], effectiveFormat)
    readonly property bool alpha: root.contains(["png", "webp", "tiff", "ico", "cur", "icns", "xpm"], effectiveFormat)
    readonly property int iconPixels: setting("iconSize", 64)
    readonly property string formatLabel: {
        for (const item of exportFormats) if (item.key === effectiveFormat) return item.label
        return effectiveFormat.toUpperCase()
    }
    signal exportFormatRequested(string format)
    signal exportScaleRequested(real value)
    signal exportQualityRequested(int value)
    signal settingRequested(string name, var value)
    signal exportRequested()
    spacing: 12

    function contains(values, value) {
        for (const candidate of values) if (candidate === value) return true
        return false
    }
    function formatChoices() {
        let choices = [{ "text": "Same as source · " + sourceFormat.toUpperCase(), "value": "original" }]
        for (const item of exportFormats) choices.push({ "text": item.label + "  ·  ." + item.suffix, "value": item.key })
        return choices
    }
    function iconChoices() {
        const sizes = effectiveFormat === "icns" ? [32,64,128,256,512,1024] : [16,24,32,48,64,128,256]
        let result = []
        for (const size of sizes) result.push({ "text": size + " × " + size + " pixels", "value": size })
        return result
    }
    function setting(name, fallback) { return exportSettings[name] !== undefined ? exportSettings[name] : fallback }
    function indexFor(model, value) {
        for (let i = 0; i < model.length; ++i) if (model[i].value === value) return i
        return 0
    }
    function request(name, value) {
        if (name === "quality") exportQualityRequested(value)
        else settingRequested(name, value)
    }
    function descriptionFor(key) {
        switch (key) {
        case "lossless": return "Keep image quality unchanged."
        case "quality": return "Higher quality usually means larger files."
        case "progressive": return "Show a preview while loading."
        case "optimized": return "Reduce file size without changing quality."
        case "paletteColors": return "Choose how many colors to keep."
        case "threshold": return mono ? "Choose which pixels become black." : "Choose which pixels become transparent."
        case "dither": return "Use dots for smoother shading."
        case "invert": return "Swap black and white pixels."
        case "hotspotX": return "Set the click point horizontally."
        case "hotspotY": return "Set the click point vertically."
        case "keepAlpha": return "Keep transparent areas clear."
        case "matte": return "Fill transparent areas with this color."
        default: return ""
        }
    }
    function choice(key, title, labels, values, fallback) {
        return { "key": key, "title": title, "type": "choice", "fallback": fallback,
            "choices": labels.map(function(label, i) { return { "text": label, "value": values[i] } }) }
    }
    function toggle(key, title, fallback) { return { "key": key, "title": title, "type": "toggle", "fallback": fallback } }
    function number(key, title, fallback, maximum) { return { "key": key, "title": title, "type": "number", "fallback": fallback, "maximum": maximum } }
    readonly property var rows: {
        const f = effectiveFormat
        let result = []
        if (f === "png") result.push(choice("compression", "Compression", ["Fast (Same quality, larger)", "Balanced (Same quality, smaller)", "Smallest (Same quality, slower)"], [0, 50, 100], 50))
        if (f === "tiff") result.push(choice("compression", "Compression", ["LZW (Same quality, smaller)", "Uncompressed (Same quality, larger)"], [50, 0], 50))
        if (f === "webp") result.push(toggle("lossless", "Lossless", false))
        if (f === "jpeg" || (f === "webp" && !setting("lossless", false)))
            result.push({ "key": "quality", "title": "Quality", "type": "slider", "fallback": 95, "minimum": 1, "maximum": f === "webp" ? 99 : 100 })
        if (f === "jpeg") result = result.concat([toggle("progressive", "Progressive JPEG", false), toggle("optimized", "Optimize encoding", true)])
        if (["png", "jpeg", "tiff", "bmp"].indexOf(f) >= 0) {
            const indexed = f === "png" || f === "bmp"
            result.push(choice("colorMode", "Color mode", indexed ? ["Color", "Grayscale", "Indexed palette (Fewer colors)"] : ["Color", "Grayscale"], indexed ? [0, 1, 2] : [0, 1], 0))
        }
        const palette = f === "xpm" || (setting("colorMode", 0) === 2 && ["png", "bmp"].indexOf(f) >= 0)
        if (palette) result.push(choice("paletteColors", "Palette", ["8 palette colors", "64 palette colors", "216 palette colors"], [8, 64, 216], 216))
        if (mono || f === "xpm") result.push({ "key": "threshold", "title": mono ? "Black / white threshold" : "Transparency threshold", "type": "slider", "fallback": 128, "minimum": 0, "maximum": 255 })
        if (mono || palette) result.push(toggle("dither", "Dithering", true))
        if (mono) result.push(toggle("invert", "Invert black and white", false))
        if (["pbm", "pgm", "ppm"].indexOf(f) >= 0) result.push(choice("ascii", "Encoding", ["Binary encoding (Smaller file)", "Plain text (Readable numbers)"], [false, true], false))
        if (f === "cur") result = result.concat([number("hotspotX", "Cursor hotspot X", 0, iconPixels - 1), number("hotspotY", "Cursor hotspot Y", 0, iconPixels - 1)])
        if (alpha) result.push(toggle("keepAlpha", "Keep transparency", true))
        if (!alpha || !setting("keepAlpha", true)) result.push(choice("matte", "Background", ["White background", "Black background"], ["#ffffff", "#000000"], "#ffffff"))
        return result
    }
    SectionCard {
        width: parent.width
        title: "File format"
        contentTopSpacing: 8
        ExportChoice {
            objectName: "editorExportFormat"
            width: parent.width
            accessibleLabel: "Export file format"
            model: root.formatChoices()
            currentIndex: root.indexFor(model, root.exportFormat)
            onActivated: root.exportFormatRequested(currentValue)
        }
    }
    SectionCard {
        width: parent.width
        title: root.icon ? "Icon canvas" : "Resolution"
        ExportChoice {
            objectName: "editorExportIconSize"
            visible: root.icon
            width: parent.width
            accessibleLabel: "Icon dimensions"
            model: root.iconChoices()
            currentIndex: root.indexFor(model, root.iconPixels)
            onActivated: root.settingRequested("iconSize", currentValue)
        }
        ExportChoice {
            objectName: "editorExportIconCrop"
            visible: root.icon
            width: parent.width
            accessibleLabel: "Icon framing"
            model: [{ "text": "Fit (Keep whole image)", "value": false }, { "text": "Fill (Crop edges)", "value": true }]
            currentIndex: root.indexFor(model, root.setting("iconCrop", false))
            onActivated: root.settingRequested("iconCrop", currentValue)
        }
        AdjustRow {
            objectName: "editorExportScaleControl"
            visible: !root.icon
            width: parent.width
            label: "Output scale"
            value: root.exportScale
            from: 0.01
            to: 2.0
            defaultValue: 1.0
            keyboardStep: 0.01
            signedDisplay: false
            onValueRequested: function(v) { root.exportScaleRequested(v) }
        }
        Row {
            visible: !root.icon
            width: parent.width
            spacing: 8
            ExportNumber {
                objectName: "editorExportWidth"
                width: (parent.width - 26) / 2
                value: root.outputWidth
                minimum: Math.max(1, Math.round(root.unscaledWidth * .01))
                maximum: root.unscaledWidth * 2
                label: "Output width in pixels; aspect ratio locked"
                onValueRequested: function(v) { if (root.unscaledWidth > 0) root.exportScaleRequested(Math.max(.01, Math.min(2, v / root.unscaledWidth))) }
            }
            Text { text: "×"; color: "#B8FFFFFF"; height: 40; verticalAlignment: Text.AlignVCenter }
            ExportNumber {
                objectName: "editorExportHeight"
                width: (parent.width - 26) / 2
                value: root.outputHeight
                minimum: Math.max(1, Math.round(root.unscaledHeight * .01))
                maximum: root.unscaledHeight * 2
                label: "Output height in pixels; aspect ratio locked"
                onValueRequested: function(v) { if (root.unscaledHeight > 0) root.exportScaleRequested(Math.max(.01, Math.min(2, v / root.unscaledHeight))) }
            }
        }
        Text {
            width: parent.width
            text: root.icon ? root.iconPixels + " × " + root.iconPixels + " pixels"
                : root.outputWidth + " × " + root.outputHeight + " pixels · aspect ratio locked"
            color: "#B8FFFFFF"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
    }
    SectionCard {
        width: parent.width
        title: root.formatLabel + " settings"
        visible: true
        Repeater {
            model: root.rows
            Column {
                id: row
                required property var modelData
                width: parent ? parent.width : 0
                spacing: 6
                Text {
                    visible: row.modelData.type === "choice" || row.modelData.type === "number"
                    text: row.modelData.title
                    color: "#B8FFFFFF"
                    font.pixelSize: 12
                }
                Text {
                    visible: (row.modelData.type === "choice" || row.modelData.type === "number")
                        && text.length > 0
                    width: parent.width
                    text: root.descriptionFor(row.modelData.key)
                    color: "#B3FFFFFF"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                ExportChoice {
                    objectName: "editorExport_" + row.modelData.key
                    visible: row.modelData.type === "choice"
                    width: parent.width
                    accessibleLabel: row.modelData.title
                    model: row.modelData.choices || []
                    currentIndex: root.indexFor(model, root.setting(row.modelData.key, row.modelData.fallback))
                    onActivated: root.request(row.modelData.key, currentValue)
                }
                ToggleRow {
                    objectName: "editorExport_" + row.modelData.key + "Toggle"
                    visible: row.modelData.type === "toggle"
                    width: parent.width
                    title: row.modelData.title
                    subtitle: root.descriptionFor(row.modelData.key)
                    buttonMotionDuration: root.buttonMotionDuration
                    switchMotionDuration: root.buttonMotionDuration > 0 ? 160 : 0
                    checked: root.setting(row.modelData.key, row.modelData.fallback) === true
                    onToggled: function(v) { root.request(row.modelData.key, v) }
                }
                AdjustRow {
                    objectName: "editorExport_" + row.modelData.key + "Slider"
                    visible: row.modelData.type === "slider"
                    width: parent.width
                    label: row.modelData.title
                    value: row.modelData.type !== "slider" ? 0 : row.modelData.key === "quality"
                        ? Math.min(root.exportQuality, row.modelData.maximum) : root.setting(row.modelData.key, row.modelData.fallback)
                    from: row.modelData.minimum !== undefined ? row.modelData.minimum : 0
                    to: row.modelData.maximum !== undefined ? row.modelData.maximum : 100
                    defaultValue: row.modelData.type === "slider" ? row.modelData.fallback : 0
                    displayMultiplier: 1
                    suffix: row.modelData.key === "quality" ? "%" : ""
                    decimals: 0
                    keyboardStep: 1
                    signedDisplay: false
                    onValueRequested: function(v) { root.request(row.modelData.key, Math.round(v)) }
                }
                Text {
                    visible: row.modelData.type === "slider" && text.length > 0
                    width: parent.width
                    text: root.descriptionFor(row.modelData.key)
                    color: "#B3FFFFFF"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                ExportNumber {
                    objectName: "editorExport_" + row.modelData.key + "Number"
                    visible: row.modelData.type === "number"
                    width: parent.width
                    label: row.modelData.title
                    value: row.modelData.type === "number" ? root.setting(row.modelData.key, row.modelData.fallback) : 0
                    maximum: row.modelData.maximum !== undefined ? row.modelData.maximum : 100000
                    onValueRequested: function(v) { root.request(row.modelData.key, v) }
                }
            }
        }
    }
    ActionChip {
        objectName: "editorExportAdvanced"
        width: parent.width
        text: root.advancedOpen ? "Advanced  ⌃" : "Advanced  ⌄"
        onClicked: root.advancedOpen = !root.advancedOpen
    }
    SectionCard {
        visible: root.advancedOpen
        width: parent.width
        title: "Advanced"
        ToggleRow {
            objectName: "editorExportNearest"
            width: parent.width
            title: "Pixel-art scaling"
            subtitle: "Keep pixel edges sharp."
            buttonMotionDuration: root.buttonMotionDuration
            switchMotionDuration: root.buttonMotionDuration > 0 ? 160 : 0
            checked: root.setting("nearest", false)
            onToggled: function(v) { root.settingRequested("nearest", v) }
        }
        ToggleRow {
            objectName: "editorExportSrgb"
            visible: root.contains(["png", "jpeg", "tiff"], root.effectiveFormat)
            width: parent.width
            title: "Convert to sRGB"
            subtitle: "Use standard colors for screens."
            buttonMotionDuration: root.buttonMotionDuration
            switchMotionDuration: root.buttonMotionDuration > 0 ? 160 : 0
            checked: root.setting("srgb", false)
            onToggled: function(v) { root.settingRequested("srgb", v) }
        }
        Text {
            visible: root.contains(["png", "jpeg", "tiff", "bmp"], root.effectiveFormat)
            text: "Density (DPI) · 0 keeps source"
            color: "#B8FFFFFF"
            font.pixelSize: 12
        }
        Text {
            visible: root.contains(["png", "jpeg", "tiff", "bmp"], root.effectiveFormat)
            width: parent.width
            text: "Change print density, not pixel size."
            color: "#B3FFFFFF"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        ExportNumber {
            objectName: "editorExportDpi"
            visible: root.contains(["png", "jpeg", "tiff", "bmp"], root.effectiveFormat)
            width: parent.width
            label: "Output DPI; zero preserves source density"
            value: root.setting("dpi", 0)
            maximum: 2400
            onValueRequested: function(v) { root.settingRequested("dpi", v) }
        }
    }
    ActionChip {
        objectName: "editorExportButton"
        width: parent.width
        sectionStyle: true
        affirmative: true
        text: root.saveBusy ? "Exporting…" : "Export " + root.formatLabel
            + (root.estimatedSize.length > 0 ? " · " + root.estimatedSize : "")
        subtitle: root.icon ? root.iconPixels + " × " + root.iconPixels + " pixels" : root.outputWidth + " × " + root.outputHeight + " pixels"
        enabled: root.hasImage && !root.saveBusy
        animationDuration: root.buttonMotionDuration
        onClicked: { if (enabled) root.exportRequested() }
    }
    Text {
        visible: root.saveStatus.length > 0
        width: parent.width
        text: root.saveStatus
        color: "#C8FFFFFF"
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }
}
