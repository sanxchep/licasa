#include "export/image_save_operations.h"
#include "export/image_export_contract.h"
#include "export/image_export_encoder.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_processing.h"
#include "io/external_file_identity.h"
#include "media/motion/motion_photo_frame.h"
#include <QColorSpace>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace Licasa::ImageSaveOperations {
namespace {
QByteArray normalizedWriterFormat(const QString& suffix)
{
    const QByteArray requested = suffix.trimmed().toLower().toLatin1();
    if (requested.isEmpty()) {
        return {};
    }

    const QByteArray canonical = ImageExport::canonicalFormat(requested);
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    for (QByteArray format : supported) {
        format = format.toLower();
        if (format == requested || format == canonical ||
            (canonical == QByteArrayLiteral("jpeg") && format == QByteArrayLiteral("jpg"))) {
            return canonical;
        }
    }
    return {};
}

struct ImageDestination {
    QString path;
    QByteArray format;
    QString errorMessage;
};

// Resolve once for both edited-image and captured-frame exports. Only the
// fallback differs: preserve a writable source format, or default to PNG.
ImageDestination resolveImageDestination(QString destinationPath, const ImageExportOptions& options,
                                         const QString& fallbackSuffix = {})
{
    if (!options.validationError.isEmpty()) {
        return {{}, {}, options.validationError};
    }
    QByteArray requestedFormat = normalizedWriterFormat(QString::fromLatin1(options.format));
    if (!options.format.isEmpty() && requestedFormat.isEmpty()) {
        return {{}, {}, QStringLiteral("This export format is unavailable in this installation.")};
    }

    if (!requestedFormat.isEmpty()) {
        const QString extension = requestedFormat == QByteArrayLiteral("jpeg")
                                      ? QStringLiteral("jpg")
                                      : QString::fromLatin1(requestedFormat);
        const QString currentSuffix = QFileInfo(destinationPath).suffix();
        if (currentSuffix.isEmpty()) {
            destinationPath += QStringLiteral(".") + extension;
        } else if (normalizedWriterFormat(currentSuffix) != requestedFormat) {
            if (options.copyOnly) {
                return {{},
                        {},
                        QStringLiteral(
                            "Use .%1 for this export format, then confirm the destination again.")
                            .arg(extension)};
            }
            destinationPath.chop(currentSuffix.size());
            destinationPath += extension;
        }
    } else if (QFileInfo(destinationPath).suffix().isEmpty()) {
        QByteArray fallbackFormat = normalizedWriterFormat(fallbackSuffix);
        if (fallbackFormat.isEmpty()) {
            fallbackFormat = QByteArrayLiteral("png");
        }
        destinationPath += QStringLiteral(".") + QString::fromLatin1(fallbackFormat);
    }

    const QFileInfo destinationInfo(destinationPath);
    if (!destinationInfo.dir().exists()) {
        return {{}, {}, QStringLiteral("The destination folder does not exist.")};
    }

    const QByteArray writerFormat = requestedFormat.isEmpty()
                                        ? normalizedWriterFormat(destinationInfo.suffix())
                                        : requestedFormat;
    if (writerFormat.isEmpty()) {
        return {{}, {}, QStringLiteral("Choose an available export format.")};
    }

    return {destinationInfo.absoluteFilePath(), writerFormat, {}};
}

// Own the raster during scaling and writing. Both callers retain the same
// processing-policy lock until QSaveFile has committed or cancelled the write.
ImageSaveResult writeExportImage(QImage image, const ImageDestination& destination,
                                 const ImageExportOptions& options, int maximumMegapixels,
                                 const std::function<QString()>& validateSource = {},
                                 const std::function<void()>& beforeCommit = {})
{
    const quint64 maximumPixels = ImageProcessing::pixelsForMegapixels(maximumMegapixels);
    const qreal scale = std::isfinite(options.scale) ? std::clamp(options.scale, 0.01, 2.0) : 1.0;
    if ((options.width > 0) != (options.height > 0) || options.width < 0 || options.height < 0) {
        return {{}, QStringLiteral("Choose valid output dimensions.")};
    }
    const double w = options.width > 0 ? options.width : image.width() * static_cast<double>(scale);
    const double h =
        options.height > 0 ? options.height : image.height() * static_cast<double>(scale);
    if (!std::isfinite(w) || !std::isfinite(h) || w > std::numeric_limits<int>::max() ||
        h > std::numeric_limits<int>::max()) {
        return {{}, QStringLiteral("That export resolution is too large.")};
    }
    const QSize size(std::max(1, int(std::lround(w))), std::max(1, int(std::lround(h))));
    if (!ImageDecodeContract::allows(size, maximumPixels)) {
        return {{}, QStringLiteral("That export exceeds the configured working memory limit.")};
    }
    if (!ImageExport::isIcon(destination.format) && options.width > 0 &&
        (w < image.width() * .01 - 1 || h < image.height() * .01 - 1 ||
         w > image.width() * 2.0 + 1 || h > image.height() * 2.0 + 1)) {
        return {
            {},
            QStringLiteral("Output dimensions must be between 1% and 200% of the edited image.")};
    }
    if (size != image.size() && !ImageExport::isIcon(destination.format)) {
        image = image.scaled(size, Qt::IgnoreAspectRatio,
                             options.nearest ? Qt::FastTransformation : Qt::SmoothTransformation);
        if (image.isNull()) {
            return {{}, QStringLiteral("The scaled image could not be allocated.")};
        }
    }

    QSaveFile output(destination.path);
    if (!output.open(QIODevice::WriteOnly)) {
        return {{}, output.errorString()};
    }

    const QString error =
        ImageExport::encode(&output, std::move(image), destination.format, options);
    if (!error.isEmpty()) {
        output.cancelWriting();
        return {{}, error};
    }
    if (beforeCommit) {
        beforeCommit();
    }
    if (validateSource) {
        const QString sourceError = validateSource();
        if (!sourceError.isEmpty()) {
            output.cancelWriting();
            return {{}, sourceError};
        }
    }
    if (!output.commit()) {
        return {{}, output.errorString()};
    }

    return {QUrl::fromLocalFile(destination.path), {}};
}

} // namespace

ImageSaveResult writeEditedImage(const QString& sourcePath, QString destinationPath,
                                 const ImageEditParameters& parameters,
                                 const ImageExportOptions& options, int maximumMegapixels,
                                 std::optional<ExternalFileIdentity> expectedSource,
                                 const std::function<void()>& beforeCommit)
{
    const quint64 maximumPixels = ImageProcessing::pixelsForMegapixels(maximumMegapixels);
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return {{}, QStringLiteral("The original image no longer exists.")};
    }

    const ImageDestination destination =
        resolveImageDestination(std::move(destinationPath), options, sourceInfo.suffix());
    if (!destination.errorMessage.isEmpty()) {
        return {{}, destination.errorMessage};
    }

    QFile source(sourceInfo.absoluteFilePath());
    if (!source.open(QIODevice::ReadOnly)) {
        return {{}, source.errorString()};
    }
    QString identityError;
    const auto identity = externalFileIdentity(source, &identityError);
    if (!identity) {
        return {{}, identityError};
    }
    if (expectedSource && *identity != *expectedSource) {
        return {{}, QStringLiteral("The original image changed before export. Try again.")};
    }
    bool aliasesSource =
        QFileInfo(destination.path).canonicalFilePath() == sourceInfo.canonicalFilePath();
#if defined(Q_OS_UNIX)
    struct stat destinationStatus {};
    if (::stat(QFile::encodeName(destination.path).constData(), &destinationStatus) == 0) {
        aliasesSource = aliasesSource || (quint64(destinationStatus.st_dev) == identity->device &&
                                          quint64(destinationStatus.st_ino) == identity->inode);
    }
#endif
    if (options.copyOnly && aliasesSource) {
        return {{}, QStringLiteral("Choose a copy destination different from the original image.")};
    }
    QImageReader reader(&source);
    // Saving is an explicit full-pixel operation. RAW viewing's embedded
    // preview is never silently substituted for the requested export.
    ImageDecodeContract::configure(reader, maximumPixels, nullptr, true);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        return {{}, ImageProcessing::readerErrorOrFallback(reader)};
    }
    const QSize sourceSize = reader.size();
    if (!sourceSize.isValid()) {
        return {{},
                QStringLiteral(
                    "The decoder cannot report dimensions before full-resolution processing.")};
    }
    if (!ImageDecodeContract::allows(sourceSize, maximumPixels)) {
        return {{}, ImageProcessing::imageLimitError(maximumMegapixels)};
    }

    QImage image = reader.read();
    if (image.isNull()) {
        return {{}, ImageProcessing::readerErrorOrFallback(reader)};
    }
    if (!ImageDecodeContract::allows(image.size(), maximumPixels)) {
        return {{}, ImageProcessing::imageLimitError(maximumMegapixels)};
    }
    if (!applyImageEdits(image, parameters)) {
        return {{}, QStringLiteral("The image could not be processed safely.")};
    }

    return writeExportImage(
        std::move(image), destination, options, maximumMegapixels,
        [&]() {
            const auto after = externalFileIdentity(source, &identityError);
            QFile current(sourcePath);
            if (!current.open(QIODevice::ReadOnly)) {
                return QStringLiteral("The source changed during export.");
            }
            const auto currentIdentity = externalFileIdentity(current, &identityError);
            return after && currentIdentity && *after == *identity && *currentIdentity == *identity
                       ? QString()
                       : QStringLiteral("The source changed during export. Try again.");
        },
        beforeCommit);
}

ImageSaveResult writeFrameImage(const MotionPhotoFrameRaster& frame, QString destinationPath,
                                const ImageExportOptions& options, int maximumMegapixels)
{
    const quint64 maximumPixels = ImageProcessing::pixelsForMegapixels(maximumMegapixels);
    if (!frame.isValid() || !ImageDecodeContract::allows(frame.size, maximumPixels)) {
        return {{}, ImageProcessing::imageLimitError(maximumMegapixels)};
    }

    const ImageDestination destination =
        resolveImageDestination(std::move(destinationPath), options);
    if (!destination.errorMessage.isEmpty()) {
        return {{}, destination.errorMessage};
    }

    QImage image(reinterpret_cast<const uchar*>(frame.rgba8888.constData()), frame.size.width(),
                 frame.size.height(), frame.bytesPerLine, QImage::Format_RGBA8888);
    if (image.isNull()) {
        return {{}, QStringLiteral("The selected video frame could not be reconstructed.")};
    }
    if (!frame.iccProfile.isEmpty()) {
        const QColorSpace colorSpace = QColorSpace::fromIccProfile(frame.iccProfile);
        if (colorSpace.isValid()) {
            image.setColorSpace(colorSpace);
        }
    }

    return writeExportImage(std::move(image), destination, options, maximumMegapixels);
}

SizeEstimateResult measureEditedImageSize(const QString& sourcePath,
                                          const ImageEditParameters& parameters,
                                          const ImageExportOptions& options, int maximumMegapixels)
{
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return {-1, QStringLiteral("A temporary export file could not be created.")};
    }

    // Use the production writer and the dialog's suggested extension so the
    // byte count reflects the same edits, format, quality and scaling as Save As.
    const QString sourceSuffix = QFileInfo(sourcePath).suffix().toLower();
    const QString suggestionSuffix =
        !options.format.isEmpty() ? (options.format == "jpeg" ? QStringLiteral("jpg")
                                                              : QString::fromLatin1(options.format))
        : normalizedWriterFormat(sourceSuffix).isEmpty() ? QStringLiteral("png")
                                                         : sourceSuffix;
    const ImageSaveResult result = writeEditedImage(
        sourcePath, directory.filePath(QStringLiteral("estimate.") + suggestionSuffix), parameters,
        options, maximumMegapixels);
    if (!result.errorMessage.isEmpty()) {
        return {-1, result.errorMessage};
    }
    const qint64 byteCount = QFileInfo(result.destinationUrl.toLocalFile()).size();
    if (byteCount <= 0) {
        return {-1, QStringLiteral("The export size could not be measured.")};
    }
    return {byteCount, {}};
}

} // namespace Licasa::ImageSaveOperations
