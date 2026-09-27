#pragma once

#include "media/motion/motion_photo_frame.h"
#include "media/motion/motion_photo_playback_policy.h"
#include "media/photo_asset_probe.h"

#include <QString>

#include <functional>
#include <optional>

namespace Licasa {

inline constexpr quint32 kMotionPhotoBackendAbi = 4;

struct MotionPhotoBackendState {
    bool active = false;
    bool playing = false;
    qint64 positionMs = 0;
    qint64 durationMs = 0;
    bool seekable = false;
    QString errorString;
};

class MotionPhotoBackend {
  public:
    using StateChangedCallback = std::function<void(const MotionPhotoBackendState&)>;
    using FrameChangedCallback = std::function<void(MotionPhotoFrameRaster)>;

    virtual ~MotionPhotoBackend() = default;
    virtual void setStateChangedCallback(StateChangedCallback callback) = 0;
    virtual void setFrameChangedCallback(FrameChangedCallback callback) = 0;
    virtual void play(const PhotoAssetInfo& asset) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void seek(qint64 positionMs) = 0;
    virtual std::optional<MotionPhotoFrameRaster> captureCurrentFrame() const = 0;
};

using MotionPhotoBackendAbiFn = quint32 (*)();
using CreateMotionPhotoBackendFn = MotionPhotoBackend* (*)(MotionPhotoResourcePolicy* policy);

} // namespace Licasa
