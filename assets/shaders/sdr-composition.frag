#version 440
// Compiled with Qt 6.4 qsb --glsl '100 es,120,150' --hlsl 50 --msl 12.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float whiteScale;
};
layout(binding = 1) uniform sampler2D source;
void main()
{
    vec4 sampleColor = texture(source, qt_TexCoord0);
    // Transfer functions operate on straight RGB, never premultiplied RGB.
    vec3 encoded = sampleColor.a > 0.0
        ? clamp(sampleColor.rgb / sampleColor.a, 0.0, 1.0) : vec3(0.0);
    vec3 low = encoded / 12.92;
    vec3 high = pow((encoded + 0.055) / 1.055, vec3(2.4));
    vec3 linearColor = mix(high, low, lessThanEqual(encoded, vec3(0.04045)));
    fragColor = vec4(linearColor * whiteScale * sampleColor.a, sampleColor.a) * qt_Opacity;
}
