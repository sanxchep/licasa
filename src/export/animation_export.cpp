#include "export/animation_export.h"

#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"
#include "io/atomic_file_copy.h"

#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QImageReader>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThreadPool>

#include <algorithm>

namespace Licasa {
namespace {

AnimationExportResult failure(const QString& message) { return AnimationExportResult{{}, message}; }

bool validateAnimatedSource(ImageResourcePolicy& resourcePolicy, QFile& source, QString* error)
{
    if (!source.seek(0)) {
        if (error) {
            *error = QCoreApplication::translate("AnimationExport", "Unable to seek the animation source.");
        }
        return false;
    }

    QMutexLocker gate(&resourcePolicy.processingMutex());
    const int megapixels = resourcePolicy.prepareForProcessing();
    const quint64 pixelBudget = quint64(std::max(1, megapixels)) * 1000000ULL;

    QImageReader reader(&source);
    ImageDecodeContract::configure(reader, pixelBudget);
    reader.setAutoTransform(true);

    if (!reader.canRead()) {
        if (error) {
            *error = QCoreApplication::translate("AnimationExport", "The source is not a readable image.");
        }
        return false;
    }
    // Some streaming/incremental plugins report an unknown count (0) while
    // supportsAnimation() is authoritative. Only a known singleton is static.
    if (!reader.supportsAnimation() || reader.imageCount() == 1) {
        if (error) {
            *error = QCoreApplication::translate("AnimationExport", "The source is not an animated image.");
        }
        return false;
    }
    return true;
}

} // namespace

AnimationExportResult copyAnimationFile(ImageResourcePolicy& resourcePolicy, const QUrl& sourceUrl,
                                        const QUrl& destinationUrl)
{
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        return failure(QCoreApplication::translate("AnimationExport", "The animation source is not a local file."));
    }
    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        return failure(QCoreApplication::translate("AnimationExport", "Choose a local destination file."));
    }

    const QString sourcePath = sourceUrl.toLocalFile();
    const QString destinationPath = destinationUrl.toLocalFile();
    const QString sourceSuffix = QFileInfo(sourcePath).suffix().toLower();
    const QString destinationSuffix = QFileInfo(destinationPath).suffix().toLower();
    if (sourceSuffix.isEmpty()) {
        return failure(QCoreApplication::translate("AnimationExport", "The animation source has no exportable file extension."));
    }
    if (destinationSuffix != sourceSuffix) {
        return failure(QCoreApplication::translate(
            "AnimationExport", "Export this animation copy with the %1 extension.")
                           .arg(QStringLiteral(".") + sourceSuffix));
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        return failure(source.errorString());
    }

    QString identityError;
    const auto identityBefore = externalFileIdentity(source, &identityError);
    if (!identityBefore) {
        return failure(identityError);
    }

    QString validationError;
    if (!validateAnimatedSource(resourcePolicy, source, &validationError)) {
        return failure(validationError);
    }

    const auto identityAfterValidation = externalFileIdentity(source, &identityError);
    if (!identityAfterValidation || *identityAfterValidation != *identityBefore) {
        return failure(QCoreApplication::translate("AnimationExport", "The animation source changed during export validation."));
    }
    const QString copyError = copyFileRangeAtomically(
        source, *identityBefore, CheckedByteRange{0, identityBefore->size}, destinationPath);
    if (!copyError.isEmpty()) {
        return failure(copyError);
    }

    return AnimationExportResult{QUrl::fromLocalFile(destinationPath), {}};
}

AnimationExportService::AnimationExportService(ImageResourcePolicy& resourcePolicy, QObject* parent)
    : QObject(parent), resourcePolicy_(resourcePolicy)
{}

AnimationExportService::~AnimationExportService()
{
    if (!pool_) {
        return;
    }
    pool_->clear();
    pool_->waitForDone();
}

QThreadPool& AnimationExportService::workerPool()
{
    if (!pool_) {
        pool_ = std::make_unique<QThreadPool>();
        pool_->setMaxThreadCount(1);
        pool_->setExpiryTimeout(1000);
    }
    return *pool_;
}

bool AnimationExportService::exportCopy(const QUrl& sourceUrl, const QUrl& destinationUrl)
{
    if (busy_) {
        emit exportFailed(sourceUrl, QCoreApplication::translate("AnimationExport", "Another animation export is still running."));
        return false;
    }
    if (!sourceUrl.isValid() || !sourceUrl.isLocalFile()) {
        emit exportFailed(sourceUrl, QCoreApplication::translate("AnimationExport", "No local animation source is available."));
        return false;
    }
    if (!destinationUrl.isValid() || !destinationUrl.isLocalFile()) {
        emit exportFailed(sourceUrl, QCoreApplication::translate("AnimationExport", "Choose a local destination file."));
        return false;
    }

    busy_ = true;
    emit busyChanged();

    workerPool().start([this, sourceUrl, destinationUrl]() {
        const AnimationExportResult result =
            copyAnimationFile(resourcePolicy_, sourceUrl, destinationUrl);
        QMetaObject::invokeMethod(
            this, [this, sourceUrl, result]() { finishExport(sourceUrl, result); },
            Qt::QueuedConnection);
    });
    return true;
}

void AnimationExportService::finishExport(const QUrl& sourceUrl,
                                          const AnimationExportResult& result)
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
