#pragma once

#include "media/motion/motion_photo_backend_interface.h"

#include <QObject>

#include <memory>
#include <optional>

class QLibrary;

namespace Licasa {

// Lightweight per-window presentation bridge for validated Motion Photos.
// This target contains Qt Core only. The multimedia implementation lives in a
// private module that is mapped on the first playback request and never exposes
// PhotoAssetInfo or checked media ranges through the Qt meta-object API.
class MotionPhotoSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)

  public:
    explicit MotionPhotoSession(MotionPhotoResourcePolicy* resourcePolicy = nullptr,
                                QObject* parent = nullptr);
    ~MotionPhotoSession() override;

    // C++-only authority handoff. These methods are deliberately not slots or
    // invokables, so QML cannot manufacture or retain PhotoAssetInfo metadata.
    void setAsset(const PhotoAssetInfo& asset);
    void clearAsset();
    // Native export bridge only; deliberately absent from the Qt meta-object API.
    std::optional<MotionPhotoFrameRaster> captureCurrentFrame() const;
    // Native presentation bridge only. This getter is intentionally not a Qt
    // property, slot or invokable, so QML cannot retrieve decoded frame bytes.
    std::optional<MotionPhotoFrameRaster> presentationFrame() const;

    bool available() const noexcept { return available_; }
    bool active() const noexcept { return active_; }
    bool playing() const noexcept { return playing_; }
    qint64 positionMs() const noexcept { return positionMs_; }
    qint64 durationMs() const noexcept { return durationMs_; }
    bool seekable() const noexcept { return seekable_; }
    QString errorString() const { return errorString_; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void stop();

  signals:
    void availableChanged();
    void activeChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void seekableChanged();
    void errorChanged();
    void frameChanged();

  private:
    bool ensureBackend();
    void releaseBackend();
    void applyBackendState(const MotionPhotoBackendState& state);
    void resetPlaybackState();
    void setAvailable(bool value);
    void setErrorString(const QString& value);
    void acceptPresentationFrame(MotionPhotoFrameRaster frame);
    void clearPresentationFrame();

    MotionPhotoResourcePolicy* resourcePolicy_ = nullptr;
    std::optional<PhotoAssetInfo> asset_;
    // Library must outlive the object whose vtable/destructor live inside it.
    std::unique_ptr<QLibrary> backendLibrary_;
    std::unique_ptr<MotionPhotoBackend> backend_;
    bool available_ = false;
    bool active_ = false;
    bool playing_ = false;
    qint64 positionMs_ = 0;
    qint64 durationMs_ = 0;
    bool seekable_ = false;
    QString errorString_;
    std::optional<MotionPhotoFrameRaster> presentationFrame_;
};

} // namespace Licasa
