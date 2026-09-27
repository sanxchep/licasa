#pragma once

#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QSize>
#include <QtQml/qqmlregistration.h>

#include <optional>

#include "media/motion/motion_photo_frame.h"

namespace Licasa {

// Native-only presentation surface for Motion Photo video frames. QML receives
// only a normal visual item plus boolean/size state; decoded bytes, QVideoFrame,
// QVideoSink and backend handles never cross the meta-object boundary.
class MotionPhotoFrameSurface : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
    Q_PROPERTY(QSize frameSize READ frameSize NOTIFY frameChanged)
    Q_PROPERTY(qint64 frameTimestampUs READ frameTimestampUs NOTIFY frameChanged)
    Q_PROPERTY(bool smooth READ smooth WRITE setSmooth NOTIFY smoothChanged)

  public:
    explicit MotionPhotoFrameSurface(QQuickItem* parent = nullptr);
    ~MotionPhotoFrameSurface() override;

    QObject* session() const noexcept { return session_.data(); }
    void setSession(QObject* session);

    bool hasFrame() const;
    QSize frameSize() const;
    qint64 frameTimestampUs() const;

    bool smooth() const noexcept { return smooth_; }
    void setSmooth(bool smooth);

    void paint(QPainter* painter) override;

  signals:
    void sessionChanged();
    void frameChanged();
    void smoothChanged();

  private:
    void syncFrame();
    void clearFrame();

    QPointer<QObject> session_;
    QMetaObject::Connection frameConnection_;
    QMetaObject::Connection destroyedConnection_;
    mutable QMutex frameMutex_;
    std::optional<MotionPhotoFrameRaster> frame_;
    bool smooth_ = true;
};

} // namespace Licasa
