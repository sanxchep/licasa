#include "export/motion_photo_export.h"

#include "io/atomic_file_copy.h"

#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QThreadPool>

#include <optional>
#include <utility>

namespace Licasa {
namespace {

struct MotionCopySource {
    QString path;
    CheckedByteRange range;
    QString suffix;
    std::optional<ExternalFileIdentity> expectedIdentity;
};

MotionPhotoExportResult failure(const QString& message)
{
    return MotionPhotoExportResult{{}, message};
}

QString suffixForMimeType(const QString& mimeType)
{
    if (mimeType == QStringLiteral("video/mp4")) {
        return QStringLiteral("mp4");
    }
    if (mimeType == QStringLiteral("video/quicktime")) {
        return QStringLiteral("mov");
    }
    return {};
}

std::optional<MotionCopySource> copySourceForAsset(const PhotoAssetInfo& asset, QString* error)
{
    if (asset.kind != PhotoAssetKind::MotionPhoto || !asset.motion ||
        !asset.sourceUrl.isLocalFile()) {
        if (error) {
            *error = QStringLiteral("Export requires a validated local Motion Photo.");
        }
        return std::nullopt;
    }

    const MotionComponent& motion = *asset.motion;
    const bool embedded = motion.hasEmbeddedVideo();
    const bool external = motion.hasExternalVideo();
    if (embedded == external) {
        if (error) {
            *error =
                QStringLiteral("Motion Photo export requires exactly one validated motion source.");
        }
        return std::nullopt;
    }

    const QString suffix = suffixForMimeType(motion.mimeType);
    if (suffix.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The Motion Photo video type is not exportable.");
        }
        return std::nullopt;
    }

    MotionCopySource result;
    result.suffix = suffix;
    if (embedded) {
        result.path = asset.sourceUrl.toLocalFile();
        result.range = motion.embedded;
        return result;
    }

    if (!motion.externalVideoIdentity || motion.externalVideoIdentity->size == 0) {
        if (error) {
            *error = QStringLiteral(
                "The paired Live Photo movie no longer has a validated file identity.");
        }
        return std::nullopt;
    }

    result.path = motion.externalVideoUrl.toLocalFile();
    result.range = CheckedByteRange{0, motion.externalVideoIdentity->size};
    result.expectedIdentity = motion.externalVideoIdentity;
    return result;
}

} // namespace

MotionPhotoExportResult copyMotionPhotoComponent(const PhotoAssetInfo& asset,
                                                 const QUrl& destinationUrl)
{
    QString sourceError;
    const auto source = copySourceForAsset(asset, &sourceError);
    if (!source) {
        return failure(sourceError);
    }

    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        return failure(QStringLiteral("Choose a local destination file."));
    }

    const QString destinationPath = destinationUrl.toLocalFile();
    const QString destinationSuffix = QFileInfo(destinationPath).suffix().toLower();
    if (destinationSuffix != source->suffix) {
        return failure(QStringLiteral("Export this motion component as .%1.").arg(source->suffix));
    }

    QFile sourceFile(source->path);
    if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        return failure(sourceFile.errorString());
    }

    QString identityError;
    const auto sourceIdentity = externalFileIdentity(sourceFile, &identityError);
    if (!sourceIdentity) {
        return failure(identityError);
    }

    if (source->expectedIdentity && *sourceIdentity != *source->expectedIdentity) {
        return failure(QStringLiteral("The paired Live Photo movie changed after validation. "
                                      "Reopen the photo and try again."));
    }

    if (!checkedByteRange(sourceIdentity->size, source->range.offset, source->range.length)) {
        return failure(
            QStringLiteral("The validated motion range no longer fits the source file."));
    }

    const QString primaryPath = asset.sourceUrl.toLocalFile();
    if (asset.motion->hasExternalVideo()) {
        QFile primaryFile(primaryPath);
        if (!primaryFile.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            return failure(QStringLiteral("The Motion Photo still source is no longer readable."));
        }
        QString primaryIdentityError;
        const auto primaryIdentity = externalFileIdentity(primaryFile, &primaryIdentityError);
        if (!primaryIdentity) {
            return failure(primaryIdentityError);
        }
        if (destinationAliasesSource(destinationPath, primaryPath, *primaryIdentity)) {
            return failure(
                QStringLiteral("Choose a destination that is not an alias of the still source."));
        }
    }

    const QString copyError =
        copyFileRangeAtomically(sourceFile, *sourceIdentity, source->range, destinationPath);
    if (!copyError.isEmpty()) {
        return failure(copyError);
    }

    return MotionPhotoExportResult{QUrl::fromLocalFile(destinationPath), {}};
}

MotionPhotoExportService::MotionPhotoExportService(QObject* parent) : QObject(parent) {}

MotionPhotoExportService::~MotionPhotoExportService()
{
    if (!pool_) {
        return;
    }
    pool_->clear();
    pool_->waitForDone();
}

QThreadPool& MotionPhotoExportService::workerPool()
{
    if (!pool_) {
        pool_ = std::make_unique<QThreadPool>();
        pool_->setMaxThreadCount(1);
        pool_->setExpiryTimeout(1000);
    }
    return *pool_;
}

void MotionPhotoExportService::setAsset(const PhotoAssetInfo& asset)
{
    QString ignoredError;
    const auto source = copySourceForAsset(asset, &ignoredError);
    const bool nextAvailable = source.has_value();
    const QUrl nextSourceUrl = nextAvailable ? asset.sourceUrl : QUrl{};
    const QString nextSuffix = nextAvailable ? source->suffix : QString{};

    if (sourceUrl_ != nextSourceUrl) {
        sourceUrl_ = nextSourceUrl;
    }
    if (available_ != nextAvailable) {
        available_ = nextAvailable;
        emit availableChanged();
    }
    if (suggestedSuffix_ != nextSuffix) {
        suggestedSuffix_ = nextSuffix;
        emit suggestedSuffixChanged();
    }
}

void MotionPhotoExportService::clearAsset()
{
    const bool hadAvailability = available_;
    const bool hadSuffix = !suggestedSuffix_.isEmpty();
    sourceUrl_ = QUrl{};
    available_ = false;
    suggestedSuffix_.clear();
    if (hadAvailability) {
        emit availableChanged();
    }
    if (hadSuffix) {
        emit suggestedSuffixChanged();
    }
}

bool MotionPhotoExportService::exportCopy(const QUrl& destinationUrl)
{
    if (busy_) {
        emit exportFailed(sourceUrl_, QStringLiteral("Another motion export is still running."));
        return false;
    }
    if (!available_ || !sourceUrl_.isValid() || !sourceUrl_.isLocalFile()) {
        emit exportFailed(sourceUrl_,
                          QStringLiteral("No exportable motion component is available."));
        return false;
    }
    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        emit exportFailed(sourceUrl_, QStringLiteral("Choose a local destination file."));
        return false;
    }

    const QUrl sourceUrl = sourceUrl_;
    busy_ = true;
    emit busyChanged();

    workerPool().start([this, sourceUrl, destinationUrl]() {
        // Never trust a range cached while the image was first opened. Re-probe
        // at the user export boundary, then stream only that freshly validated
        // motion authority. This remains metadata-only and does not map the
        // Multimedia backend or invoke a transcoder.
        const PhotoAssetInfo current = probePhotoAsset(sourceUrl);
        QString validationError;
        MotionPhotoExportResult result;
        if (!copySourceForAsset(current, &validationError)) {
            result = failure(validationError.isEmpty()
                                 ? QStringLiteral("The motion component is no longer available.")
                                 : validationError);
        } else {
            result = copyMotionPhotoComponent(current, destinationUrl);
        }

        QMetaObject::invokeMethod(
            this, [this, sourceUrl, result]() { finishExport(sourceUrl, result); },
            Qt::QueuedConnection);
    });
    return true;
}

void MotionPhotoExportService::finishExport(const QUrl& sourceUrl,
                                            const MotionPhotoExportResult& result)
{
    busy_ = false;
    emit busyChanged();
    if (!result.errorMessage.isEmpty()) {
        emit exportFailed(sourceUrl, result.errorMessage);
        return;
    }
    emit exportCompleted(sourceUrl, result.destinationUrl);
}

} // namespace Licasa
