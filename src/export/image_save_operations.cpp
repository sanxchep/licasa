#include "export/image_save_operations.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_processing.h"
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

    const QByteArray canonical =
        requested == QByteArrayLiteral("jpg") ? QByteArrayLiteral("jpeg") : requested;
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
    QByteArray requestedFormat = normalizedWriterFormat(QString::fromLatin1(options.format));
    if (!options.format.isEmpty() && requestedFormat.isEmpty()) {
        return {{},
                {},
                QStringLiteral("This export format is unavailable. Choose PNG, JPEG, or WebP.")};
    }

    if (!requestedFormat.isEmpty()) {
        const QString extension = requestedFormat == QByteArrayLiteral("jpeg")
                                      ? QStringLiteral("jpg")
                                      : QString::fromLatin1(requestedFormat);
        const QString currentSuffix = QFileInfo(destinationPath).suffix();
        if (currentSuffix.isEmpty()) {
            destinationPath += QStringLiteral(".") + extension;
        } else if (normalizedWriterFormat(currentSuffix) != requestedFormat) {
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
        return {
            {}, {}, QStringLiteral("This file type cannot be saved. Choose PNG, JPEG, or WebP.")};
    }

    return {destinationInfo.absoluteFilePath(), writerFormat, {}};
}

// Own the raster during scaling and writing. Both callers retain the same
// processing-policy lock until QSaveFile has committed or cancelled the write.
ImageSaveResult writeExportImage(QImage image, const ImageDestination& destination,
                                 const ImageExportOptions& options, int maximumMegapixels)
{
    const quint64 maximumPixels = ImageProcessing::pixelsForMegapixels(maximumMegapixels);
    const qreal scale = std::isfinite(options.scale) ? std::clamp(options.scale, 0.1, 2.0) : 1.0;
    if (!qFuzzyCompare(scale, 1.0)) {
        const double scaledWidth = image.width() * static_cast<double>(scale);
        const double scaledHeight = image.height() * static_cast<double>(scale);
        if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) ||
            scaledWidth > std::numeric_limits<int>::max() ||
            scaledHeight > std::numeric_limits<int>::max()) {
            return {{}, QStringLiteral("That export resolution is too large.")};
        }

        const QSize scaledSize(std::max(1, static_cast<int>(std::lround(scaledWidth))),
                               std::max(1, static_cast<int>(std::lround(scaledHeight))));
        if (!ImageDecodeContract::allows(scaledSize, maximumPixels)) {
            return {{},
                    QStringLiteral("That export exceeds the %1 MP full-resolution limit. Increase "
                                   "Maximum image size in Advanced Settings.")
                        .arg(maximumMegapixels)};
        }
        image = image.scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (image.isNull()) {
            return {{}, QStringLiteral("The scaled image could not be allocated.")};
        }
    }

    QSaveFile output(destination.path);
    if (!output.open(QIODevice::WriteOnly)) {
        return {{}, output.errorString()};
    }

    QImageWriter writer(&output, destination.format);
    writer.setOptimizedWrite(true);
    if (destination.format == QByteArrayLiteral("jpeg") ||
        destination.format == QByteArrayLiteral("webp")) {
        writer.setQuality(options.quality);
    }

    if (!writer.write(image)) {
        const QString error = writer.errorString();
        output.cancelWriting();
        return {{}, error};
    }
    if (!output.commit()) {
        return {{}, output.errorString()};
    }

    return {QUrl::fromLocalFile(destination.path), {}};
}

} // namespace

ImageSaveResult writeEditedImage(const QString& sourcePath, QString destinationPath,
                                 const ImageEditParameters& parameters,
                                 const ImageExportOptions& options, int maximumMegapixels)
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

    QImageReader reader(sourceInfo.absoluteFilePath());
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

    return writeExportImage(std::move(image), destination, options, maximumMegapixels);
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
        sourceSuffix == QStringLiteral("jpg") || sourceSuffix == QStringLiteral("jpeg") ||
                sourceSuffix == QStringLiteral("png") || sourceSuffix == QStringLiteral("webp")
            ? sourceSuffix
            : QStringLiteral("png");
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
