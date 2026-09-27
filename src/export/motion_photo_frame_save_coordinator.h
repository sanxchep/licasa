#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantMap>

namespace Licasa {

class ImageSaveService;
class MotionPhotoSession;

// Per-window bridge for the user-facing "Save frame as..." action.
// QML can request a save and observe only a capability boolean; decoded frame
// pixels remain native and are transferred directly from MotionPhotoSession to
// ImageSaveService.
class MotionPhotoFrameSaveCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canSaveFrame READ canSaveFrame NOTIFY canSaveFrameChanged)

  public:
    MotionPhotoFrameSaveCoordinator(MotionPhotoSession& session, ImageSaveService& saveService,
                                    QObject* parent = nullptr);

    bool canSaveFrame() const noexcept;

    Q_INVOKABLE bool saveCurrentFrame(const QUrl& sourceUrl, const QUrl& destinationUrl,
                                      const QVariantMap& exportValues = {});

  signals:
    void canSaveFrameChanged();

  private:
    MotionPhotoSession& session_;
    ImageSaveService& saveService_;
};

} // namespace Licasa
