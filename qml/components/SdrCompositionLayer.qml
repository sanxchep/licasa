import QtQuick

// Legacy SDR content is composited internally in its existing sRGB encoding.
// Convert the flattened premultiplied result once, before blending with HDR.
Item {
    id: root
    property real whiteScale: 0
    readonly property bool linearOutput: isFinite(whiteScale) && whiteScale > 0
    layer.enabled: linearOutput
    layer.format: ShaderEffectSource.RGBA16F
    layer.effect: conversionEffect
    readonly property Component conversionEffect: Component {
        ShaderEffect {
            property var source
            property real whiteScale: root.whiteScale
            fragmentShader: "qrc:/Licasa/assets/shaders/sdr-composition.frag.qsb"
        }
    }
}
