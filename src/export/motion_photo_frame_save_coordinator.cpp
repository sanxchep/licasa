#include "export/motion_photo_frame_save_coordinator.h"

#include "export/image_save_service.h"
#include "media/motion/motion_photo_session.h"

#include <optional>
#include <utility>

namespace Licasa {

MotionPhotoFrameSaveCoordinator::MotionPhotoFrameSaveCoordinator(MotionPhotoSession& session,
                                                                 ImageSaveService& saveService,
                                                                 QObject* parent)
    : QObject(parent), session_(session), saveService_(saveService)
{
    const auto notifyCapability = [this]() { emit canSaveFrameChanged(); };
    connect(&session_, &MotionPhotoSession::availableChanged, this, notifyCapability);
    connect(&session_, &MotionPhotoSession::activeChanged, this, notifyCapability);
    connect(&saveService_, &ImageSaveService::busyChanged, this, notifyCapability);
}

bool MotionPhotoFrameSaveCoordinator::canSaveFrame() const noexcept
{
    return session_.available() && session_.active() && !saveService_.busy();
}

bool MotionPhotoFrameSaveCoordinator::saveCurrentFrame(const QUrl& sourceUrl,
                                                       const QUrl& destinationUrl,
                                                       const QVariantMap& exportValues)
{
    // Avoid copying a video frame when another save already owns the shared
    // service. Calling the service with an empty frame preserves its existing
    // user-visible busy/error signal path.
    if (saveService_.busy()) {
        return saveService_.saveFrameCopy(sourceUrl, destinationUrl, MotionPhotoFrameRaster{},
                                          exportValues);
    }

    std::optional<MotionPhotoFrameRaster> frame = session_.captureCurrentFrame();
    const bool accepted = saveService_.saveFrameCopy(
        sourceUrl, destinationUrl, frame ? std::move(*frame) : MotionPhotoFrameRaster{},
        exportValues);

    if (accepted) {
        // saveFrameCopy owns the raster before returning. Tear down playback
        // immediately so its shared heavy-processing lock is released before
        // the asynchronous image writer proceeds and no decoder buffers overlap
        // the frame-copy export longer than necessary.
        session_.stop();
    }
    return accepted;
}

} // namespace Licasa
