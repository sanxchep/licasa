#include "export/image_save_service.h"
#include "export/image_save_operations.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_resource_policy.h"
#include <QCryptographicHash>
#include <QMutexLocker>
#include <utility>

namespace Licasa {
namespace {
QString versionToken(const ExternalFileIdentity& identity)
{
    QByteArray value;
    for (const auto& part :
         {QString::number(identity.device), QString::number(identity.inode),
          QString::number(identity.linkCount), QString::number(identity.size),
          QString::number(identity.modifiedSeconds), QString::number(identity.modifiedNanoseconds),
          QString::number(identity.changedSeconds), QString::number(identity.changedNanoseconds)}) {
        value += part.toLatin1() + ':';
    }
    return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}
} // namespace

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

QString ImageSaveService::sourceVersion(const QUrl& sourceUrl) const
{
    if (!sourceUrl.isLocalFile()) {
        return {};
    }
    QFile source(sourceUrl.toLocalFile());
    if (!source.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto identity = externalFileIdentity(source);
    return identity ? versionToken(*identity) : QString();
}

bool ImageSaveService::save(const QUrl& sourceUrl, const QUrl& destinationUrl,
                            const QVariantMap& editValues, const QVariantMap& exportValues,
                            QObject* requestOwner, const QString& expectedSourceVersion)
{
    if (busy_) {
        emit saveFailed(sourceUrl, QStringLiteral("Another image is still being saved."),
                        requestOwner);
        return false;
    }
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        emit saveFailed(sourceUrl, QStringLiteral("The original image is not a local file."),
                        requestOwner);
        return false;
    }
    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        emit saveFailed(sourceUrl, QStringLiteral("Choose a local destination file."),
                        requestOwner);
        return false;
    }

    const ImageEditParameters parameters = editParametersFromMap(editValues);
    const ImageExportOptions options = exportOptionsFromMap(exportValues);
    const QPointer<QObject> ownerGuard(requestOwner);
    QFile acceptedSource(sourceUrl.toLocalFile());
    QString sourceError;
    if (!acceptedSource.open(QIODevice::ReadOnly)) {
        emit saveFailed(sourceUrl, acceptedSource.errorString(), requestOwner);
        return false;
    }
    const auto sourceIdentity = externalFileIdentity(acceptedSource, &sourceError);
    if (!sourceIdentity) {
        emit saveFailed(sourceUrl, sourceError, requestOwner);
        return false;
    }
    if (!expectedSourceVersion.isEmpty() &&
        versionToken(*sourceIdentity) != expectedSourceVersion) {
        emit saveFailed(sourceUrl,
                        QStringLiteral("The original image changed. Reopen it before saving."),
                        requestOwner);
        return false;
    }

    busy_ = true;
    emit busyChanged();

    const QString sourcePath = sourceUrl.toLocalFile();
    const QString destinationPath = destinationUrl.toLocalFile();
    pool_.start([this, sourceUrl, sourcePath, destinationPath, parameters, options, ownerGuard,
                 sourceIdentity]() {
        ImageSaveOperations::ImageSaveResult result;
        {
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            result = ImageSaveOperations::writeEditedImage(
                sourcePath, destinationPath, parameters, options,
                resourcePolicy_.prepareForProcessing(), sourceIdentity);
        }
        QMetaObject::invokeMethod(
            this,
            [this, sourceUrl, result, ownerGuard]() {
                finishSave(sourceUrl, result.destinationUrl, result.errorMessage, ownerGuard);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void ImageSaveService::estimateSize(const QUrl& sourceUrl, int requestId,
                                    const QVariantMap& editValues, const QVariantMap& exportValues,
                                    QObject* requestOwner)
{
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        emit sizeEstimateReady(sourceUrl, requestId, -1,
                               QStringLiteral("The original image is not a local file."),
                               requestOwner);
        return;
    }
    EstimateRequest request{sourceUrl,    requestId,    editValues,
                            exportValues, requestOwner, requestOwner != nullptr};
    if (estimating_ || busy_) {
        for (auto& pending : pendingEstimates_) {
            if (pending.owner == request.owner) {
                pending = std::move(request);
                return;
            }
        }
        if (pendingEstimates_.size() >= 32) {
            pendingEstimates_.removeFirst();
        }
        pendingEstimates_.append(std::move(request));
        return;
    }
    startSizeEstimate(std::move(request));
}

void ImageSaveService::cancelSizeEstimate(QObject* requestOwner)
{
    if (!requestOwner) {
        pendingEstimates_.clear();
    } else {
        pendingEstimates_.removeIf([requestOwner](const EstimateRequest& request) {
            return request.owner == requestOwner;
        });
    }
}

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
    if (!request.owned || request.owner) {
        emit sizeEstimateReady(request.sourceUrl, request.requestId, byteCount, errorMessage,
                               request.owner);
    }
}

void ImageSaveService::startQueuedEstimateIfIdle()
{
    if (busy_ || estimating_) {
        return;
    }
    while (!pendingEstimates_.isEmpty()) {
        EstimateRequest next = pendingEstimates_.takeFirst();
        if (next.owned && !next.owner) {
            continue;
        }
        startSizeEstimate(std::move(next));
        return;
    }
}

bool ImageSaveService::saveFrameCopy(const QUrl& sourceUrl, const QUrl& destinationUrl,
                                     MotionPhotoFrameRaster frame, const QVariantMap& exportValues,
                                     QObject* requestOwner)
{
    if (busy_) {
        emit frameSaveFailed(sourceUrl, QStringLiteral("Another image is still being saved."),
                             requestOwner);
        return false;
    }
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        emit frameSaveFailed(sourceUrl, QStringLiteral("The Motion Photo is not a local file."),
                             requestOwner);
        return false;
    }
    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        emit frameSaveFailed(sourceUrl, QStringLiteral("Choose a local destination file."),
                             requestOwner);
        return false;
    }
    if (!frame.isValid()) {
        emit frameSaveFailed(
            sourceUrl, QStringLiteral("No decoded Motion Photo frame is available."), requestOwner);
        return false;
    }

    const ImageExportOptions options = exportOptionsFromMap(exportValues);
    const QString destinationPath = destinationUrl.toLocalFile();
    const QPointer<QObject> ownerGuard(requestOwner);

    busy_ = true;
    emit busyChanged();

    pool_.start([this, sourceUrl, destinationPath, frame = std::move(frame), options,
                 ownerGuard]() mutable {
        ImageSaveOperations::ImageSaveResult result;
        {
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            result = ImageSaveOperations::writeFrameImage(frame, destinationPath, options,
                                                          resourcePolicy_.prepareForProcessing());
        }
        QMetaObject::invokeMethod(
            this,
            [this, sourceUrl, result, ownerGuard]() {
                finishFrameSave(sourceUrl, result.destinationUrl, result.errorMessage, ownerGuard);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void ImageSaveService::finishSave(const QUrl& sourceUrl, const QUrl& destinationUrl,
                                  const QString& errorMessage,
                                  const QPointer<QObject>& requestOwner)
{
    busy_ = false;
    emit busyChanged();
    startQueuedEstimateIfIdle();

    if (!errorMessage.isEmpty()) {
        emit saveFailed(sourceUrl, errorMessage, requestOwner.data());
        return;
    }
    emit saveCompleted(sourceUrl, destinationUrl, requestOwner.data());
}

void ImageSaveService::finishFrameSave(const QUrl& sourceUrl, const QUrl& destinationUrl,
                                       const QString& errorMessage,
                                       const QPointer<QObject>& requestOwner)
{
    busy_ = false;
    emit busyChanged();
    startQueuedEstimateIfIdle();

    if (!errorMessage.isEmpty()) {
        emit frameSaveFailed(sourceUrl, errorMessage, requestOwner.data());
        return;
    }
    emit frameSaveCompleted(sourceUrl, destinationUrl, requestOwner.data());
}

} // namespace Licasa
