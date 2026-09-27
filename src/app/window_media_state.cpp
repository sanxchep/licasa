#include "app/window_media_state.h"

#include "imaging/image_animation.h"
#include "media/motion/motion_photo_session.h"
#include "media/preferred_cover_frame_coordinator.h"

#include <QQuickWindow>
#include <algorithm>

namespace Licasa {

void WindowMediaState::disconnectCoverSignals()
{
    for (const auto& connection : coverConnections) {
        QObject::disconnect(connection);
    }
    coverConnections.clear();
}

void WindowMediaState::bindAnimationCover(QQuickWindow* window, ImageAnimation* animation)
{
    auto* coordinator = coverCoordinator.data();
    const QPointer<ImageAnimation> animationGuard(animation);
    const QPointer<PreferredCoverFrameCoordinator> coordinatorGuard(coordinator);
    const QPointer<QQuickWindow> windowGuard(window);

    coordinator->setApplyHandler([animationGuard](qint64 value) {
        if (!animationGuard || value < 0 || value > 100000) {
            return false;
        }
        if (!animationGuard->active() || animationGuard->source().isEmpty()) {
            return false;
        }
        animationGuard->seekFrame(static_cast<int>(value));
        return true;
    });

    const auto updateAnimationState = [animationGuard, coordinatorGuard]() {
        if (!animationGuard || !coordinatorGuard) {
            return;
        }
        coordinatorGuard->setCurrentState(
            animationGuard->currentFrame(), animationGuard->currentFrame() >= 0,
            animationGuard->active() && !animationGuard->source().isEmpty());
    };

    updateAnimationState();
    QUrl sourceUrl = animation->source();
    if (sourceUrl.isEmpty()) {
        sourceUrl = window->property("residentImageUrl").toUrl();
    }
    coordinator->setSource(sourceUrl, PreferredCoverFrameKind::AnimationFrameIndex);

    coverConnections.append(QObject::connect(animation, &ImageAnimation::frameChanged, coordinator,
                                             updateAnimationState));
    coverConnections.append(QObject::connect(animation, &ImageAnimation::activeChanged, coordinator,
                                             updateAnimationState));
    coverConnections.append(QObject::connect(
        animation, &ImageAnimation::sourceChanged, coordinator,
        [animationGuard, coordinatorGuard, windowGuard, updateAnimationState]() {
            if (!animationGuard || !coordinatorGuard) {
                return;
            }
            updateAnimationState();
            QUrl source = animationGuard->source();
            if (source.isEmpty() && windowGuard) {
                source = windowGuard->property("residentImageUrl").toUrl();
            }
            coordinatorGuard->setSource(source, PreferredCoverFrameKind::AnimationFrameIndex);
        }));
}

void WindowMediaState::bindMotionCover()
{
    auto* coordinator = coverCoordinator.data();
    auto* session = motionSession.data();
    const QPointer<MotionPhotoSession> motionGuard(session);
    const QPointer<PreferredCoverFrameCoordinator> coordinatorGuard(coordinator);

    coordinator->setApplyHandler([motionGuard](qint64 timestampUs) {
        if (!motionGuard || timestampUs < 0 || !motionGuard->active() || !motionGuard->seekable() ||
            motionGuard->durationMs() <= 0) {
            return false;
        }
        const qint64 requestedMs = timestampUs / 1000;
        motionGuard->seek(std::min(requestedMs, motionGuard->durationMs()));
        return true;
    });

    const auto updateMotionState = [motionGuard, coordinatorGuard]() {
        if (!motionGuard || !coordinatorGuard) {
            return;
        }
        coordinatorGuard->setCurrentState(motionGuard->positionMs() * 1000, motionGuard->active(),
                                          motionGuard->active() && motionGuard->seekable() &&
                                              motionGuard->durationMs() > 0);
    };

    updateMotionState();
    coordinator->setSource(motionAsset->sourceUrl, PreferredCoverFrameKind::MotionTimestampUs);

    coverConnections.append(QObject::connect(session, &MotionPhotoSession::positionChanged,
                                             coordinator, updateMotionState));
    coverConnections.append(QObject::connect(session, &MotionPhotoSession::activeChanged,
                                             coordinator, updateMotionState));
    coverConnections.append(QObject::connect(session, &MotionPhotoSession::durationChanged,
                                             coordinator, updateMotionState));
    coverConnections.append(QObject::connect(session, &MotionPhotoSession::seekableChanged,
                                             coordinator, updateMotionState));
}

} // namespace Licasa
