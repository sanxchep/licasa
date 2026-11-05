.pragma library

var bindings = {
    help: "F1", open: "Ctrl+O", save: "Ctrl+S", saveCopy: "Ctrl+Shift+S",
    preferences: "Ctrl+,", quit: "Ctrl+Shift+Q", fullscreen: "F11", escape: "Esc",
    fit: "F", actualSize: "1", zoomIn: "+", zoomInAlias: "=", zoomOut: "-",
    editor: "E", settings: "M", notifications: "N", crop: "C", applyCrop: "Return",
    undo: "Ctrl+Z", redo: "Ctrl+Shift+Z", reset: "Ctrl+R", compare: "Y",
    adjustTool: "Ctrl+1", cropTool: "Ctrl+2", looksTool: "Ctrl+3", exportTool: "Ctrl+4",
    previous: "Left", next: "Right", zoomUp: "Up", zoomDown: "Down",
    panLeft: "Ctrl+Left", panRight: "Ctrl+Right", panUp: "Ctrl+Up", panDown: "Ctrl+Down",
    cropLeft: "Alt+Left", cropRight: "Alt+Right", cropUp: "Alt+Up", cropDown: "Alt+Down",
    cropResizeLeft: "Alt+Shift+Left", cropResizeRight: "Alt+Shift+Right",
    cropResizeUp: "Alt+Shift+Up", cropResizeDown: "Alt+Shift+Down",
    pageUp: "PgUp", pageDown: "PgDown"
}

function sequence(id) {
    if (!bindings[id]) throw new Error("Unknown shortcut: " + id)
    return bindings[id]
}

// Keep executable sequences unchanged; use standard arrow symbols in keycaps.
function displayKey(sequence, nativeText) {
    var arrows = {Left: "←", Right: "→", Up: "↑", Down: "↓"}
    var arrow = arrows[sequence.split("+").pop()]
    return arrow ? nativeText.slice(0, nativeText.lastIndexOf("+") + 1) + arrow : nativeText
}

// Screen readers receive words rather than having to interpret arrow glyphs.
function spokenKey(sequence) {
    var names = {
        Left: qsTranslate("KeyboardShortcuts", "Left arrow"),
        Right: qsTranslate("KeyboardShortcuts", "Right arrow"),
        Up: qsTranslate("KeyboardShortcuts", "Up arrow"),
        Down: qsTranslate("KeyboardShortcuts", "Down arrow")
    }
    var parts = sequence.split("+")
    var name = names[parts[parts.length - 1]]
    if (name) parts[parts.length - 1] = name
    return parts.join("+")
}

function groups() {
    return [
        { title: qsTranslate("KeyboardShortcuts", "Open, save and application"), rows: [
            { action: qsTranslate("KeyboardShortcuts", "Show keyboard shortcuts"), keys: [sequence("help")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Open image"), keys: [sequence("open")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Save changes"), keys: [sequence("save")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Save as a copy"), keys: [sequence("saveCopy")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Fully quit Licasa"), keys: [sequence("quit")], note: "" }
        ] },
        { title: qsTranslate("KeyboardShortcuts", "Viewing and navigation"), note: qsTranslate("KeyboardShortcuts", "Arrow keys: fullscreen only."), rows: [
            { action: qsTranslate("KeyboardShortcuts", "Toggle fullscreen"), keys: [sequence("fullscreen")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Fit image to window"), keys: [sequence("fit")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Actual size"), keys: [sequence("actualSize")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Zoom in"), keys: [sequence("zoomIn"), sequence("zoomInAlias"), sequence("zoomUp")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Zoom out"), keys: [sequence("zoomOut"), sequence("zoomDown")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Previous image"), keys: [sequence("previous")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Next image"), keys: [sequence("next")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Pan image"), keys: [sequence("panLeft"), sequence("panRight"), sequence("panUp"), sequence("panDown")], note: "" }
        ] },
        { title: qsTranslate("KeyboardShortcuts", "Settings and editing"), note: qsTranslate("KeyboardShortcuts", "Editing shortcuts: fullscreen only."), rows: [
            { action: qsTranslate("KeyboardShortcuts", "Open / close Settings"), keys: [sequence("preferences"), sequence("settings")], note: qsTranslate("KeyboardShortcuts", "M: fullscreen only.") },
            { action: qsTranslate("KeyboardShortcuts", "Open / close editor"), keys: [sequence("editor")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Adjust / Crop / Looks / Export"), keys: [sequence("adjustTool"), sequence("cropTool"), sequence("looksTool"), sequence("exportTool")], note: qsTranslate("KeyboardShortcuts", "Editor open.") },
            { action: qsTranslate("KeyboardShortcuts", "Undo / redo"), keys: [sequence("undo"), sequence("redo")], note: qsTranslate("KeyboardShortcuts", "Editor open.") },
            { action: qsTranslate("KeyboardShortcuts", "Reset adjustments"), keys: [sequence("reset")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Compare with original"), keys: [sequence("compare")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Notification history"), keys: [sequence("notifications")], note: qsTranslate("KeyboardShortcuts", "Fullscreen only.") }
        ] },
        { title: qsTranslate("KeyboardShortcuts", "Cropping"), rows: [
            { action: qsTranslate("KeyboardShortcuts", "Start crop"), keys: [sequence("crop")], note: qsTranslate("KeyboardShortcuts", "Fullscreen editor.") },
            { action: qsTranslate("KeyboardShortcuts", "Apply / cancel crop"), keys: [sequence("applyCrop"), sequence("escape")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Move crop frame"), keys: [sequence("cropLeft"), sequence("cropRight"), sequence("cropUp"), sequence("cropDown")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Resize crop frame"), keys: [sequence("cropResizeLeft"), sequence("cropResizeRight"), sequence("cropResizeUp"), sequence("cropResizeDown")], note: "" }
        ] },
        { title: qsTranslate("KeyboardShortcuts", "Playback and controls"), rows: [
            { action: qsTranslate("KeyboardShortcuts", "Activate a focused button"), keys: ["Space", "Return"], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Adjust a focused timeline"), keys: ["Left", "Right", "Home", "End"], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Adjust a focused slider"), keys: ["Left", "Right", "Up", "Down", "Home"], note: qsTranslate("KeyboardShortcuts", "Home resets an adjustment slider.") }
        ] },
        { title: qsTranslate("KeyboardShortcuts", "Panels, dialogs and text fields"), rows: [
            { action: qsTranslate("KeyboardShortcuts", "Move keyboard focus"), keys: ["Tab", "Shift+Tab"], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Scroll a panel"), keys: [sequence("pageUp"), sequence("pageDown")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Close or go back"), keys: [sequence("escape")], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Apply / cancel numeric entry"), keys: ["Return", "Esc"], note: "" },
            { action: qsTranslate("KeyboardShortcuts", "Select all / copy / paste / cut"), keys: ["Ctrl+A", "Ctrl+C", "Ctrl+V", "Ctrl+X"], note: "" }
        ] }
    ]
}
