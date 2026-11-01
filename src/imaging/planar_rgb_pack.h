#pragma once

#include <QImage>

#include <algorithm>
#include <cstdint>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace Licasa {
// OpenJPEG's admitted unsigned 8-bit planes still use signed 32-bit storage.
// Saturate before packing, including inverse-transform overshoot, and keep the
// scalar tail for odd widths and platforms without SSE2.
inline void packRgb8Row(const int32_t* red, const int32_t* green, const int32_t* blue, QRgb* output,
                        int width)
{
    int x = 0;
#if defined(__SSE2__)
    const __m128i zero = _mm_setzero_si128();
    const __m128i alpha = _mm_set1_epi8(char(0xff));
    for (; x <= width - 4; x += 4) {
        const auto bytes = [zero](const int32_t* samples) {
            const __m128i values = _mm_loadu_si128(reinterpret_cast<const __m128i*>(samples));
            return _mm_packus_epi16(_mm_packs_epi32(values, zero), zero);
        };
        const __m128i bg = _mm_unpacklo_epi8(bytes(blue + x), bytes(green + x));
        const __m128i ra = _mm_unpacklo_epi8(bytes(red + x), alpha);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + x), _mm_unpacklo_epi16(bg, ra));
    }
#endif
    for (; x < width; ++x) {
        output[x] = qRgb(std::clamp(red[x], 0, 255), std::clamp(green[x], 0, 255),
                         std::clamp(blue[x], 0, 255));
    }
}
} // namespace Licasa
