#pragma once

#include "io/external_file_identity.h"
#include "media/android_motion_photo.h"

#include <QObject>
#include <QThreadPool>
#include <QUrl>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Licasa {

enum class PhotoAssetKind {
    Still,
    MotionPhoto,
};

struct MotionComponent {
    CheckedByteRange embedded;
    QString mimeType;
    std::optional<qint64> presentationTimestampUs;
    QUrl externalVideoUrl = {};
    std::optional<ExternalFileIdentity> externalVideoIdentity = std::nullopt;

    bool hasEmbeddedVideo() const noexcept { return embedded.length > 0; }
    bool hasExternalVideo() const noexcept { return externalVideoUrl.isLocalFile(); }
};

struct PhotoAssetInfo {
    PhotoAssetKind kind = PhotoAssetKind::Still;
    QUrl sourceUrl;
    std::optional<MotionComponent> motion;
    QString metadataError;
};

// Cheap, bounded phone-media probe. JPEG keeps its qualified APP1 scanner;
// HEIC/AVIF obtain XMP through app-private metadata-only QImageIOHandler
// Description fields. Apple JPEG/HEIC stills may also pair with a validated
// same-basename MOV by content identifier. No probe performs a raster decode or
// exposes raw ranges, identifiers, or filesystem paths to QML.
PhotoAssetInfo probePhotoAsset(const QUrl& url, const std::atomic_bool* cancelled = nullptr);
QVariantMap photoAssetInfoToVariantMap(const PhotoAssetInfo& info);

class PhotoAssetProbe final : public QObject {
    Q_OBJECT

  public:
    explicit PhotoAssetProbe(QObject* parent = nullptr);
    ~PhotoAssetProbe() override;

    using ResultHandler = std::function<void(const QUrl&, const PhotoAssetInfo&)>;
    using InvalidationHandler = std::function<void()>;

    // Native-only handoff used by the per-window MotionPhotoSession. These are
    // ordinary C++ methods rather than Qt slots/invokables so raw asset
    // authority never enters the QML meta-object surface.
    void setResultHandler(ResultHandler handler);
    void setInvalidationHandler(InvalidationHandler handler);

    Q_INVOKABLE void request(const QUrl& url);
    Q_INVOKABLE void cancel();

  signals:
    void infoReady(const QUrl& url, const QVariantMap& info);

  private:
    QThreadPool pool_;
    std::atomic<std::uint64_t> generation_{0};
    std::shared_ptr<std::atomic_bool> cancellation_;
    ResultHandler resultHandler_;
    InvalidationHandler invalidationHandler_;
};

} // namespace Licasa
