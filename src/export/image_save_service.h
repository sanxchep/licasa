#pragma once

#include "media/motion/motion_photo_frame.h"
#include <QList>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>
#include <QVariantMap>

namespace Licasa {

class ImageResourcePolicy;

class ImageSaveService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

  public:
    explicit ImageSaveService(ImageResourcePolicy& resourcePolicy, QObject* parent = nullptr);
    ~ImageSaveService() override;

    bool busy() const;
    Q_INVOKABLE QString sourceVersion(const QUrl& sourceUrl) const;

    // Results identify the initiating window independently of its source URL.
    // Requests observe the owner weakly so closing a window never keeps it alive.
    Q_INVOKABLE bool save(const QUrl& sourceUrl, const QUrl& destinationUrl,
                          const QVariantMap& editValues, const QVariantMap& exportValues = {},
                          QObject* requestOwner = nullptr,
                          const QString& expectedSourceVersion = {});
    Q_INVOKABLE void estimateSize(const QUrl& sourceUrl, int requestId,
                                  const QVariantMap& editValues,
                                  const QVariantMap& exportValues = {},
                                  QObject* requestOwner = nullptr);
    Q_INVOKABLE void cancelSizeEstimate(QObject* requestOwner = nullptr);

    // C++-only current-frame copy path. The raster never enters QML, and its
    // completion signal is distinct from edited-image save so the open Motion
    // Photo is not replaced with the exported still.
    bool saveFrameCopy(const QUrl& sourceUrl, const QUrl& destinationUrl,
                       MotionPhotoFrameRaster frame, const QVariantMap& exportValues = {},
                       QObject* requestOwner = nullptr);

  signals:
    void busyChanged();
    void saveCompleted(const QUrl& sourceUrl, const QUrl& destinationUrl, QObject* requestOwner);
    void saveFailed(const QUrl& sourceUrl, const QString& message, QObject* requestOwner);
    void sizeEstimateReady(const QUrl& sourceUrl, int requestId, qint64 byteCount,
                           const QString& errorMessage, QObject* requestOwner);
    void frameSaveCompleted(const QUrl& sourceUrl, const QUrl& destinationUrl,
                            QObject* requestOwner);
    void frameSaveFailed(const QUrl& sourceUrl, const QString& message, QObject* requestOwner);

  private:
    void finishSave(const QUrl& sourceUrl, const QUrl& destinationUrl, const QString& errorMessage,
                    const QPointer<QObject>& requestOwner);
    void finishFrameSave(const QUrl& sourceUrl, const QUrl& destinationUrl,
                         const QString& errorMessage, const QPointer<QObject>& requestOwner);
    struct EstimateRequest {
        QUrl sourceUrl;
        int requestId = 0;
        QVariantMap editValues;
        QVariantMap exportValues;
        QPointer<QObject> owner;
        bool owned = false;
    };
    void startSizeEstimate(EstimateRequest request);
    void startQueuedEstimateIfIdle();
    void finishSizeEstimate(const EstimateRequest& request, qint64 byteCount,
                            const QString& errorMessage);

    QThreadPool pool_;
    ImageResourcePolicy& resourcePolicy_;
    bool busy_ = false;
    bool estimating_ = false;
    QList<EstimateRequest> pendingEstimates_;
};

} // namespace Licasa
