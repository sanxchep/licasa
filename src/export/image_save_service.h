#pragma once

#include "media/motion/motion_photo_frame.h"
#include <QObject>
#include <QThreadPool>
#include <QUrl>
#include <QVariantMap>
#include <optional>

namespace Licasa {

class ImageResourcePolicy;

class ImageSaveService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

  public:
    explicit ImageSaveService(ImageResourcePolicy& resourcePolicy, QObject* parent = nullptr);
    ~ImageSaveService() override;

    bool busy() const;

    Q_INVOKABLE bool save(const QUrl& sourceUrl, const QUrl& destinationUrl,
                          const QVariantMap& editValues, const QVariantMap& exportValues = {});
    Q_INVOKABLE void estimateSize(const QUrl& sourceUrl, int requestId,
                                  const QVariantMap& editValues,
                                  const QVariantMap& exportValues = {});
    Q_INVOKABLE void cancelSizeEstimate();

    // C++-only current-frame copy path. The raster never enters QML, and its
    // completion signal is distinct from edited-image save so the open Motion
    // Photo is not replaced with the exported still.
    bool saveFrameCopy(const QUrl& sourceUrl, const QUrl& destinationUrl,
                       MotionPhotoFrameRaster frame, const QVariantMap& exportValues = {});

  signals:
    void busyChanged();
    void saveCompleted(const QUrl& sourceUrl, const QUrl& destinationUrl);
    void saveFailed(const QUrl& sourceUrl, const QString& message);
    void sizeEstimateReady(const QUrl& sourceUrl, int requestId, qint64 byteCount,
                           const QString& errorMessage);
    void frameSaveCompleted(const QUrl& sourceUrl, const QUrl& destinationUrl);
    void frameSaveFailed(const QUrl& sourceUrl, const QString& message);

  private:
    void finishSave(const QUrl& sourceUrl, const QUrl& destinationUrl, const QString& errorMessage);
    void finishFrameSave(const QUrl& sourceUrl, const QUrl& destinationUrl,
                         const QString& errorMessage);
    struct EstimateRequest {
        QUrl sourceUrl;
        int requestId = 0;
        QVariantMap editValues;
        QVariantMap exportValues;
    };
    void startSizeEstimate(EstimateRequest request);
    void startQueuedEstimateIfIdle();
    void finishSizeEstimate(const EstimateRequest& request, qint64 byteCount,
                            const QString& errorMessage);

    QThreadPool pool_;
    ImageResourcePolicy& resourcePolicy_;
    bool busy_ = false;
    bool estimating_ = false;
    std::optional<EstimateRequest> pendingEstimate_;
};

} // namespace Licasa
