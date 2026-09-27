#include "media/motion/motion_photo_frame_surface.h"

#include "media/motion/motion_photo_session.h"

#include <QColorSpace>
#include <QImage>
#include <QMutexLocker>
#include <QPainter>

namespace Licasa {

MotionPhotoFrameSurface::MotionPhotoFrameSurface(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    setMipmap(false);
    setOpaquePainting(false);
}

MotionPhotoFrameSurface::~MotionPhotoFrameSurface()
{
    QObject::disconnect(frameConnection_);
    QObject::disconnect(destroyedConnection_);
}

void MotionPhotoFrameSurface::setSession(QObject* object)
{
    if (session_.data() == object) {
        return;
    }

    QObject::disconnect(frameConnection_);
    QObject::disconnect(destroyedConnection_);
    session_ = object;

    if (auto* motionSession = qobject_cast<MotionPhotoSession*>(object)) {
        frameConnection_ = QObject::connect(motionSession, &MotionPhotoSession::frameChanged, this,
                                            &MotionPhotoFrameSurface::syncFrame);
        destroyedConnection_ = QObject::connect(motionSession, &QObject::destroyed, this, [this] {
            session_.clear();
            clearFrame();
            emit sessionChanged();
        });
    }

    syncFrame();
    emit sessionChanged();
}

bool MotionPhotoFrameSurface::hasFrame() const
{
    QMutexLocker locker(&frameMutex_);
    return frame_.has_value() && frame_->isValid();
}

QSize MotionPhotoFrameSurface::frameSize() const
{
    QMutexLocker locker(&frameMutex_);
    return frame_ && frame_->isValid() ? frame_->size : QSize{};
}

qint64 MotionPhotoFrameSurface::frameTimestampUs() const
{
    QMutexLocker locker(&frameMutex_);
    return frame_ && frame_->isValid() ? frame_->timestampUs : -1;
}

void MotionPhotoFrameSurface::setSmooth(bool smooth)
{
    if (smooth_ == smooth) {
        return;
    }
    smooth_ = smooth;
    update();
    emit smoothChanged();
}

void MotionPhotoFrameSurface::paint(QPainter* painter)
{
    std::optional<MotionPhotoFrameRaster> frame;
    {
        QMutexLocker locker(&frameMutex_);
        frame = frame_;
    }

    if (!frame || !frame->isValid()) {
        return;
    }

    QImage image(reinterpret_cast<const uchar*>(frame->rgba8888.constData()), frame->size.width(),
                 frame->size.height(), frame->bytesPerLine, QImage::Format_RGBA8888);
    if (image.isNull()) {
        return;
    }

    if (!frame->iccProfile.isEmpty()) {
        const QColorSpace colorSpace = QColorSpace::fromIccProfile(frame->iccProfile);
        if (colorSpace.isValid()) {
            image.setColorSpace(colorSpace);
        }
    }

    painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth_);

    const QRectF bounds = boundingRect();
    QSizeF fitted = image.size();
    fitted.scale(bounds.size(), Qt::KeepAspectRatio);
    const QRectF target(bounds.x() + (bounds.width() - fitted.width()) / 2.0,
                        bounds.y() + (bounds.height() - fitted.height()) / 2.0, fitted.width(),
                        fitted.height());
    painter->drawImage(target, image);
}

void MotionPhotoFrameSurface::syncFrame()
{
    auto* motionSession = qobject_cast<MotionPhotoSession*>(session_.data());
    const std::optional<MotionPhotoFrameRaster> next =
        motionSession ? motionSession->presentationFrame() : std::nullopt;

    if (!next || !next->isValid()) {
        clearFrame();
        return;
    }

    {
        QMutexLocker locker(&frameMutex_);
        frame_ = *next;
    }

    // Keep the painted backing texture at video-native resolution even though
    // the QQuickItem itself follows the still-photo geometry and zoom scale.
    if (textureSize() != next->size) {
        setTextureSize(next->size);
    }
    update();
    emit frameChanged();
}

void MotionPhotoFrameSurface::clearFrame()
{
    bool changed = false;
    {
        QMutexLocker locker(&frameMutex_);
        changed = frame_.has_value();
        frame_.reset();
    }

    setTextureSize(QSize());
    update();
    if (changed) {
        emit frameChanged();
    }
}

} // namespace Licasa
