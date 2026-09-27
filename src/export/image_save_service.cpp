#include "export/image_save_service.h"
#include "export/image_save_operations.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_resource_policy.h"
#include <QMutexLocker>
#include <utility>

namespace Licasa {

ImageSaveService::ImageSaveService(ImageResourcePolicy& resourcePolicy, QObject* parent)
    : QObject(parent), resourcePolicy_(resourcePolicy)
{
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(1000);
}

ImageSaveService::~ImageSaveService()
{
    pool_.clear();
    pool_.waitForDone();
}

bool ImageSaveService::busy() const { return busy_; }

bool ImageSaveService::save(const QUrl& sourceUrl, const QUrl& destinationUrl,
                            const QVariantMap& editValues, const QVariantMap& exportValues)
{
    if (busy_) {
        emit saveFailed(sourceUrl, QStringLiteral("Another image is still being saved."));
        return false;
    }
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        emit saveFailed(sourceUrl, QStringLiteral("The original image is not a local file."));
        return false;
    }
    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        emit saveFailed(sourceUrl, QStringLiteral("Choose a local destination file."));
        return false;
    }

    const ImageEditParameters parameters = editParametersFromMap(editValues);
    const ImageExportOptions options = exportOptionsFromMap(exportValues);

    pendingEstimate_.reset();
    busy_ = true;
    emit busyChanged();

    const QString sourcePath = sourceUrl.toLocalFile();
    const QString destinationPath = destinationUrl.toLocalFile();
    pool_.start([this, sourceUrl, sourcePath, destinationPath, parameters, options]() {
        ImageSaveOperations::ImageSaveResult result;
        {
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            result = ImageSaveOperations::writeEditedImage(sourcePath, destinationPath, parameters,
                                                           options,
                                                           resourcePolicy_.prepareForProcessing());
        }
        QMetaObject::invokeMethod(
            this,
            [this, sourceUrl, result]() {
                finishSave(sourceUrl, result.destinationUrl, result.errorMessage);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void ImageSaveService::estimateSize(const QUrl& sourceUrl, int requestId,
                                    const QVariantMap& editValues, const QVariantMap& exportValues)
{
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        emit sizeEstimateReady(sourceUrl, requestId, -1,
                               QStringLiteral("The original image is not a local file."));
        return;
    }
    EstimateRequest request{sourceUrl, requestId, editValues, exportValues};
    if (estimating_ || busy_) {
        pendingEstimate_ = std::move(request);
        return;
    }
    startSizeEstimate(std::move(request));
}

void ImageSaveService::cancelSizeEstimate() { pendingEstimate_.reset(); }

void ImageSaveService::startSizeEstimate(EstimateRequest request)
{
    estimating_ = true;
    pool_.start([this, request = std::move(request)]() {
        ImageSaveOperations::SizeEstimateResult result;
        {
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            result = ImageSaveOperations::measureEditedImageSize(
                request.sourceUrl.toLocalFile(), editParametersFromMap(request.editValues),
                exportOptionsFromMap(request.exportValues), resourcePolicy_.prepareForProcessing());
        }
        QMetaObject::invokeMethod(
            this,
            [this, request = std::move(request), result]() {
                finishSizeEstimate(request, result.byteCount, result.errorMessage);
            },
            Qt::QueuedConnection);
    });
}

void ImageSaveService::finishSizeEstimate(const EstimateRequest& request, qint64 byteCount,
                                          const QString& errorMessage)
{
    estimating_ = false;
    startQueuedEstimateIfIdle();
    emit sizeEstimateReady(request.sourceUrl, request.requestId, byteCount, errorMessage);
}

void ImageSaveService::startQueuedEstimateIfIdle()
{
    if (!pendingEstimate_ || busy_ || estimating_) {
        return;
    }
    EstimateRequest next = std::move(*pendingEstimate_);
    pendingEstimate_.reset();
    startSizeEstimate(std::move(next));
}

bool ImageSaveService::saveFrameCopy(const QUrl& sourceUrl, const QUrl& destinationUrl,
                                     MotionPhotoFrameRaster frame, const QVariantMap& exportValues)
{
    if (busy_) {
        emit frameSaveFailed(sourceUrl, QStringLiteral("Another image is still being saved."));
        return false;
    }
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        emit frameSaveFailed(sourceUrl, QStringLiteral("The Motion Photo is not a local file."));
        return false;
    }
    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        emit frameSaveFailed(sourceUrl, QStringLiteral("Choose a local destination file."));
        return false;
    }
    if (!frame.isValid()) {
        emit frameSaveFailed(sourceUrl,
                             QStringLiteral("No decoded Motion Photo frame is available."));
        return false;
    }

    const ImageExportOptions options = exportOptionsFromMap(exportValues);
    const QString destinationPath = destinationUrl.toLocalFile();

    busy_ = true;
    emit busyChanged();

    pool_.start([this, sourceUrl, destinationPath, frame = std::move(frame), options]() mutable {
        ImageSaveOperations::ImageSaveResult result;
        {
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            result = ImageSaveOperations::writeFrameImage(frame, destinationPath, options,
                                                          resourcePolicy_.prepareForProcessing());
        }
        QMetaObject::invokeMethod(
            this,
            [this, sourceUrl, result]() {
                finishFrameSave(sourceUrl, result.destinationUrl, result.errorMessage);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void ImageSaveService::finishSave(const QUrl& sourceUrl, const QUrl& destinationUrl,
                                  const QString& errorMessage)
{
    busy_ = false;
    emit busyChanged();
    startQueuedEstimateIfIdle();

    if (!errorMessage.isEmpty()) {
        emit saveFailed(sourceUrl, errorMessage);
        return;
    }
    emit saveCompleted(sourceUrl, destinationUrl);
}

void ImageSaveService::finishFrameSave(const QUrl& sourceUrl, const QUrl& destinationUrl,
                                       const QString& errorMessage)
{
    busy_ = false;
    emit busyChanged();
    startQueuedEstimateIfIdle();

    if (!errorMessage.isEmpty()) {
        emit frameSaveFailed(sourceUrl, errorMessage);
        return;
    }
    emit frameSaveCompleted(sourceUrl, destinationUrl);
}

} // namespace Licasa
