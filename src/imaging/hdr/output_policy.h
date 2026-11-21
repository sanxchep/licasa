#pragma once
#include <cmath>

namespace Licasa {
// Explicit lab calibration. Vulkan's built-in hdrInfo luminance defaults are
// not evidence of the connected display's peak or preferred SDR paper white.
struct HdrOutputPolicy {
    float headroom = 0;
    float whiteScale = 0;
    bool operator==(const HdrOutputPolicy& other) const {
        return headroom == other.headroom && whiteScale == other.whiteScale;
    }
    bool operator!=(const HdrOutputPolicy& other) const { return !(*this == other); }
};
inline HdrOutputPolicy calibratedHdrOutput(float paperWhiteNits, float peakNits, bool sceneReferred)
{
    if (!std::isfinite(paperWhiteNits) || !std::isfinite(peakNits) ||
        paperWhiteNits < 80 || paperWhiteNits > 500 || peakNits <= paperWhiteNits || peakNits > 10000)
        return {};
    return {std::fmin(peakNits / paperWhiteNits, 8.0f), sceneReferred ? paperWhiteNits / 80.0f : 1.0f};
}
} // namespace Licasa
