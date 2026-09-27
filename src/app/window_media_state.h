#pragma once

#include "media/photo_asset_probe.h"
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <optional>

class QQuickWindow;

namespace Licasa {
class MotionPhotoSession;
class MotionPhotoExportService;
class AnimationExportService;
class PreferredCoverFrameCoordinator;
class ImageAnimation;

// Per-window native media state and signal bindings. QML receives intent and
// presentation properties; source identity and temporal values stay native.
struct WindowMediaState {
    std::optional<PhotoAssetInfo> motionAsset;
    QPointer<MotionPhotoSession> motionSession;
    QPointer<MotionPhotoExportService> motionService;
    QPointer<AnimationExportService> animationService;
    QPointer<PreferredCoverFrameCoordinator> coverCoordinator;
    QList<QMetaObject::Connection> coverConnections;
    void disconnectCoverSignals();
    void bindAnimationCover(QQuickWindow* window, ImageAnimation* animation);
    void bindMotionCover();
};

} // namespace Licasa
