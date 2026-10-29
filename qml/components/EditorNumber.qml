import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: root

    property real value: 0
    property real minimum: 0
    property real maximum: 100000
    property int decimals: 0
    property string suffix: ""
    property string label: ""
    property bool signedDisplay: false
    property bool compact: false
    property bool dirty: false
    signal valueRequested(real value)
    signal historyRequested(bool redo)

    function formattedValue() {
        const rounded = Number(value.toFixed(decimals))
        return (signedDisplay && rounded > 0 ? "+" : "") + String(rounded)
    }

    function restoreValue() {
        dirty = false
        text = Qt.binding(function() { return root.formattedValue() })
    }

    function finishEditing(apply) {
        if (!dirty) return
        const draft = text.trim()
        // Parse the whole entry: blanks, exponents and partial numbers must
        // never turn into an unintended zero or a partially parsed value.
        const numeric = /^[+-]?(\d+(\.\d*)?|\.\d+)$/.test(draft) ? Number(draft) : NaN
        dirty = false
        if (apply && isFinite(numeric)) {
            const bounded = Math.max(minimum, Math.min(maximum, numeric))
            const next = Number(bounded.toFixed(decimals))
            if (next !== value) valueRequested(next)
        }
        restoreValue()
    }

    height: compact ? 26 : 40
    text: formattedValue()
    color: "white"
    font.pixelSize: compact ? 12 : 13
    selectByMouse: true
    horizontalAlignment: Text.AlignHCenter
    leftPadding: 6
    rightPadding: unitLabel.visible ? unitLabel.width + 8 : 6
    topPadding: 2
    bottomPadding: 2
    inputMethodHints: Qt.ImhFormattedNumbersOnly | Qt.ImhNoPredictiveText
    Accessible.name: label
    Accessible.description: minimum + " to " + maximum + suffix

    onTextEdited: dirty = true
    onEditingFinished: finishEditing(true)
    onActiveFocusChanged: { if (!activeFocus) finishEditing(true) }
    // An undo, reset, source switch or range change retires the old draft.
    onValueChanged: restoreValue()
    onMinimumChanged: restoreValue()
    onMaximumChanged: restoreValue()
    onVisibleChanged: { if (!visible) restoreValue() }
    onEnabledChanged: { if (!enabled) restoreValue() }
    Keys.onShortcutOverride: function(event) {
        const control = (event.modifiers & Qt.ControlModifier) !== 0
        const textKeys = [Qt.Key_A, Qt.Key_C, Qt.Key_V, Qt.Key_X,
            Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down,
            Qt.Key_Home, Qt.Key_End, Qt.Key_Backspace, Qt.Key_Delete]
        const textUndo = event.key === Qt.Key_Z || event.key === Qt.Key_Y
        event.accepted = control ? textKeys.indexOf(event.key) >= 0 || textUndo
            : event.key !== Qt.Key_F11 && event.key !== Qt.Key_PageUp && event.key !== Qt.Key_PageDown
    }
    Keys.onPressed: function(event) {
        if ((event.modifiers & Qt.ControlModifier) !== 0 && !dirty
                && (event.key === Qt.Key_Z || event.key === Qt.Key_Y)) {
            historyRequested(event.key === Qt.Key_Y || (event.modifiers & Qt.ShiftModifier) !== 0)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            finishEditing(false)
            event.accepted = true
        } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            event.accepted = true
        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            finishEditing(true)
            event.accepted = false
        }
    }

    Text {
        id: unitLabel
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: root.suffix
        visible: text.length > 0
        color: "#A8FFFFFF"
        font: root.font
        Accessible.ignored: true
    }
    background: Rectangle {
        radius: root.compact ? 6 : 9
        color: "#282828"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? "#B8FFFFFF" : "#32FFFFFF"
    }
}
