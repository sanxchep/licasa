#pragma once

#include "media/motion/motion_photo_frame.h"
#include "media/motion/motion_photo_playback_policy.h"
#include "media/photo_asset_probe.h"

#include <QObject>
#include <QSize>
#include <functional>
#include <memory>
#include <optional>

namespace Licasa {
// C++ integration boundary only: no QML registration or raw-range invokables.
// Constructing this controller creates no media objects. The policy must outlive
// it; all methods and destruction belong on its owning (GUI) thread. Call stop
// before navigation, hiding, editing or saving. Stop completion is asynchronous.
class MotionPhotoPlayback final : public QObject {
    Q_OBJECT
  public:
    using FrameReadyCallback = std::function<void(MotionPhotoFrameRaster)>;

    explicit MotionPhotoPlayback(MotionPhotoResourcePolicy& policy, QObject* parent = nullptr);
    ~MotionPhotoPlayback() override;

    void play(const PhotoAssetInfo& asset);
    void pause();
    void resume();
    void seek(qint64 milliseconds);
    void stop();
    void setFrameReadyCallback(FrameReadyCallback callback);

    bool active() const;
    bool playing() const;
    bool seekable() const;
    qint64 position() const;
    qint64 duration() const;
    QString error() const { return error_; }
    // Explicit export operation only. No QVideoFrame/backend handle crosses this boundary.
    std::optional<MotionPhotoFrameRaster> captureCurrentFrame() const;

  signals:
    void changed();
    // Metadata only in this increment: no frame raster is copied or retained.
    void frameDecoded(const QSize& size, qint64 timestampUs);

  private:
    struct Session;
    void clear();
    void fail(const QString& message);
    void start(const PhotoAssetInfo& asset, quint64 generation);
    void connectPlayerSignals(quint64 generation);
    void connectFrameSink(quint64 generation);
    void admitAndPlay();
    MotionPhotoResourcePolicy& policy_;
    std::unique_ptr<Session> session_;
    QString error_;
    quint64 generation_ = 0;
    qint64 pendingSeek_ = 0;
    bool seekQueued_ = false;
    FrameReadyCallback frameReadyCallback_;
};
} // namespace Licasa
