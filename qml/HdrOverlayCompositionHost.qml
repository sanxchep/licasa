import QtQuick
import QtQuick.Controls as Controls
import "components"

// Qt Quick Controls can reparent in-window popups and dimmers to Overlay.
// Convert that independent SDR branch too. Native popup windows keep their own
// SDR surface. Binding restoration leaves Qt's overlay state intact on unload.
SdrCompositionLayer {
    id: root
    visible: false
    layer.enabled: false
    Component.onDestruction: whiteScale = 0
    readonly property Item popupOverlay: Controls.Overlay.overlay
    Binding {
        target: root.popupOverlay
        property: "layer.effect"
        value: root.conversionEffect
        when: !!root.popupOverlay && root.linearOutput
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        target: root.popupOverlay
        property: "layer.format"
        value: ShaderEffectSource.RGBA16F
        when: !!root.popupOverlay && root.linearOutput
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        target: root.popupOverlay
        property: "layer.enabled"
        value: true
        when: !!root.popupOverlay && root.linearOutput
        restoreMode: Binding.RestoreBindingOrValue
    }
}
