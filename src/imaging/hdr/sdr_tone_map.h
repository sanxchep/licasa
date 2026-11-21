#pragma once

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>

namespace Licasa::HdrSdr {
struct Rgb {
    float red;
    float green;
    float blue;
};

struct RgbMatrix {
    Rgb red;
    Rgb green;
    Rgb blue;
};

inline const RgbMatrix* toLinearSrgb(unsigned primaries)
{
    // D65 RGB-to-RGB transforms derived from BT.709, BT.2020 and Display P3
    // chromaticities. All three spaces share the same white point.
    static constexpr RgbMatrix bt709{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static constexpr RgbMatrix bt2020{{1.660491f, -0.587641f, -0.072850f},
                                      {-0.124550f, 1.132900f, -0.008350f},
                                      {-0.018151f, -0.100579f, 1.118730f}};
    static constexpr RgbMatrix displayP3{{1.224940f, -0.224940f, 0},
                                         {-0.042057f, 1.042057f, 0},
                                         {-0.019638f, -0.078636f, 1.098274f}};
    switch (primaries) {
    case 1:
        return &bt709;
    case 9:
        return &bt2020;
    case 12:
        return &displayP3;
    default:
        return nullptr;
    }
}

inline float dot(Rgb a, Rgb b) { return a.red * b.red + a.green * b.green + a.blue * b.blue; }

inline float pqDisplayNits(float signal)
{
    // ITU-R BT.2100 PQ EOTF. The endpoint represents 10,000 cd/m².
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const float power = std::pow(signal, 1.0f / m2);
    return 10000.0f * std::pow(std::max(0.0f, power - c1) / (c2 - c3 * power), 1.0f / m1);
}

inline float hlgSceneLight(float signal)
{
    // ITU-R BT.2100 inverse HLG OETF, before the display's system gamma.
    constexpr float a = 0.17883277f;
    constexpr float b = 0.28466892f;
    constexpr float c = 0.55991073f;
    return signal <= 0.5f ? signal * signal / 3.0f : (std::exp((signal - c) / a) + b) / 12.0f;
}

template <float (*Transfer)(float)> inline const auto& hdrTransferTable()
{
    static const auto table = [] {
        std::array<float, 65536> values{};
        for (size_t index = 0; index < values.size(); ++index) {
            values[index] = Transfer(float(index) / 65535.0f);
        }
        return values;
    }();
    return table;
}

inline float sdrLuminance(float nits)
{
    // Map diffuse white (203 cd/m²) to 70% linear SDR, with a smooth
    // highlight shoulder that leaves room above diffuse white.
    constexpr float diffuseWhite = 203.0f;
    constexpr float knee = 0.70f;
    constexpr float gain = knee / diffuseWhite;
    constexpr float shoulder = 1.0f - knee;
    if (nits <= diffuseWhite) {
        return nits * gain;
    }
    return 1.0f - shoulder / (1.0f + (nits - diffuseWhite) * gain / shoulder);
}

inline uchar encodeSrgb(float linear)
{
    // Encode once at the end of the transform. This table is finer than the
    // output raster and avoids three transfer-function powers per pixel.
    static const auto table = [] {
        std::array<uchar, 16385> values{};
        for (size_t index = 0; index < values.size(); ++index) {
            const float value = float(index) / 16384.0f;
            const float encoded = value <= 0.0031308f
                                      ? 12.92f * value
                                      : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
            values[index] = uchar(std::lround(encoded * 255.0f));
        }
        return values;
    }();
    return table[size_t(std::clamp(linear, 0.0f, 1.0f) * 16384.0f + 0.5f)];
}

inline void toneMapPixel(const quint16* source, uchar* destination, const RgbMatrix& matrix,
                  const std::array<float, 65536>& transfer, bool hlg)
{
    const Rgb input{transfer[source[0]], transfer[source[1]], transfer[source[2]]};
    const Rgb rgb{dot(matrix.red, input), dot(matrix.green, input), dot(matrix.blue, input)};
    const float luma = std::max(0.0f, dot({0.212639f, 0.715169f, 0.072192f}, rgb));
    // HLG is scene-referred. Use the BT.2100 1,000 cd/m² reference display
    // with gamma 1.2 and zero black. PQ has already supplied display nits.
    const float mapped = sdrLuminance(hlg ? 1000.0f * std::pow(luma, 1.2f) : luma);
    const float gain = luma > 0.0f ? mapped / luma : 0.0f;
    const Rgb chroma{rgb.red * gain - mapped, rgb.green * gain - mapped, rgb.blue * gain - mapped};
    const float low = std::min({chroma.red, chroma.green, chroma.blue});
    const float high = std::max({chroma.red, chroma.green, chroma.blue});
    // Desaturate toward equal luminance only as far as the sRGB gamut needs.
    float saturation = 1.0f;
    if (low < -mapped) {
        saturation = -mapped / low;
    }
    if (high > 1.0f - mapped) {
        saturation = std::min(saturation, (1.0f - mapped) / high);
    }
    destination[0] = encodeSrgb(mapped + chroma.red * saturation);
    destination[1] = encodeSrgb(mapped + chroma.green * saturation);
    destination[2] = encodeSrgb(mapped + chroma.blue * saturation);
    destination[3] = uchar((quint32(source[3]) + 128) / 257);
}

} // namespace Licasa::HdrSdr
