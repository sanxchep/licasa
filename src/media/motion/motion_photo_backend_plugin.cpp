#include "media/motion/motion_photo_backend_interface.h"
#include "media/motion/motion_photo_playback.h"

#include <QtGlobal>

#include <utility>

namespace Licasa {

class MotionPhotoBackendImpl final : public MotionPhotoBackend {
  public:
    explicit MotionPhotoBackendImpl(MotionPhotoResourcePolicy& policy) : playback_(policy)
    {
        QObject::connect(&playback_, &MotionPhotoPlayback::changed, &playback_,
                         [this] { publish(); });
    }

    void setStateChangedCallback(StateChangedCallback callback) override
    {
        callback_ = std::move(callback);
        publish();
    }

    void setFrameChangedCallback(FrameChangedCallback callback) override
    {
        playback_.setFrameReadyCallback(std::move(callback));
    }

    void play(const PhotoAssetInfo& asset) override { playback_.play(asset); }
    void pause() override { playback_.pause(); }
    void resume() override { playback_.resume(); }
    void seek(qint64 positionMs) override { playback_.seek(positionMs); }
    std::optional<MotionPhotoFrameRaster> captureCurrentFrame() const override
    {
        return playback_.captureCurrentFrame();
    }

  private:
    MotionPhotoBackendState state() const
    {
        return MotionPhotoBackendState{
            playback_.active(),   playback_.playing(),  playback_.position(),
            playback_.duration(), playback_.seekable(), playback_.error(),
        };
    }

    void publish()
    {
        if (callback_) {
            callback_(state());
        }
    }

    MotionPhotoPlayback playback_;
    StateChangedCallback callback_;
};

} // namespace Licasa

extern "C" Q_DECL_EXPORT quint32 licasa_motion_photo_backend_abi()
{
    return Licasa::kMotionPhotoBackendAbi;
}

extern "C" Q_DECL_EXPORT Licasa::MotionPhotoBackend*
licasa_create_motion_photo_backend(Licasa::MotionPhotoResourcePolicy* policy)
{
    if (!policy) {
        return nullptr;
    }
    return new Licasa::MotionPhotoBackendImpl(*policy);
}
