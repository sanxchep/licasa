#pragma once
#include "output_policy.h"
#include <QtGlobal>
#include <mutex>
#include <optional>

namespace Licasa {
// Render/GUI shared output epoch. Values are explicit calibration, never inferred
// from Vulkan's default luminance metadata or a monitor name/EDID cache.
class HdrOutputState {
public:
    struct Native {
        bool linear = false, supported = false, sceneReferred = true;
        int limitsType = 0;
        float white = 0, minimum = 0, maximum = 0, potential = 0;
        bool operator==(const Native& n) const {
            return linear == n.linear && supported == n.supported && sceneReferred == n.sceneReferred
                && limitsType == n.limitsType && white == n.white && minimum == n.minimum
                && maximum == n.maximum && potential == n.potential;
        }
    };
    struct Snapshot {
        quint64 revision = 1;
        float white = 0, peak = 0;
        bool calibrated = false;
        bool nativeReview = false;
    };
    Snapshot snapshot() const { std::lock_guard lock(mutex_); return value_; }
    quint64 invalidate() { std::lock_guard lock(mutex_); return invalidateLocked(); }
    bool calibrate(quint64 revision, float white, float peak) {
        if (calibratedHdrOutput(white, peak, true).headroom <= 1) return false;
        std::lock_guard lock(mutex_);
        if (revision != value_.revision || (native_ && !native_->supported)) return false;
        value_ = {value_.revision + 1, white, peak, true};
        return true;
    }
    void requireNativeReview() {
        std::lock_guard lock(mutex_); invalidateLocked(); value_.nativeReview = true;
    }
    void resetNativeReview() { std::lock_guard lock(mutex_); value_.nativeReview = false; }
    void expectSurface() { std::lock_guard lock(mutex_); native_.reset(); }
    // Called only on the render owner. A changed native output invalidates the
    // calibration immediately, before GUI delivery or another photo draw.
    bool observe(const Native& native) {
        std::lock_guard lock(mutex_);
        const bool changed = (native_ && !(*native_ == native))
            || (value_.calibrated && (!native.supported || !native.linear));
        if (changed) { invalidateLocked(); value_.nativeReview = true; }
        native_ = native;
        return changed;
    }
private:
    quint64 invalidateLocked() {
        value_ = {value_.revision + 1, 0, 0, false, value_.nativeReview};
        native_.reset();
        return value_.revision;
    }
    mutable std::mutex mutex_;
    Snapshot value_;
    std::optional<Native> native_;
};
// On an FP16 linear surface the SDR rendition/UI still needs exact sRGB decoding
// at 80-nit white when HDR calibration is unavailable. An SDR surface uses 0.
inline HdrOutputPolicy hdrSurfacePolicy(bool linear, bool sceneReferred,
                                        const HdrOutputState::Snapshot& calibration)
{
    if (!linear) return {};
    if (!calibration.calibrated) return {0, 1};
    return calibratedHdrOutput(calibration.white, calibration.peak, sceneReferred);
}
}
