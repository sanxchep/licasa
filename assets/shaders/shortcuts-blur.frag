#version 440
// Compiled with Qt 6.4 qsb --glsl '100 es,120,150' --hlsl 50 --msl 12.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 texelStep;
};
layout(binding = 1) uniform sampler2D source;
void main()
{
    vec4 sum = vec4(0.0);
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            float weight = float((3 - abs(x)) * (3 - abs(y)));
            sum += texture(source, qt_TexCoord0 + vec2(x, y) * texelStep * 1.8) * weight;
        }
    }
    fragColor = sum * (qt_Opacity / 81.0);
}
