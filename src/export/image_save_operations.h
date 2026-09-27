#pragma once

#include <QString>
#include <QUrl>

namespace Licasa {
struct ImageEditParameters;
struct ImageExportOptions;
struct MotionPhotoFrameRaster;

namespace ImageSaveOperations {

struct ImageSaveResult {
    QUrl destinationUrl;
    QString errorMessage;
};

struct SizeEstimateResult {
    qint64 byteCount = -1;
    QString errorMessage;
};

ImageSaveResult writeEditedImage(const QString& sourcePath, QString destinationPath,
                                 const ImageEditParameters& parameters,
                                 const ImageExportOptions& options, int maximumMegapixels);
ImageSaveResult writeFrameImage(const MotionPhotoFrameRaster& frame, QString destinationPath,
                                const ImageExportOptions& options, int maximumMegapixels);
SizeEstimateResult measureEditedImageSize(const QString& sourcePath,
                                          const ImageEditParameters& parameters,
                                          const ImageExportOptions& options, int maximumMegapixels);

} // namespace ImageSaveOperations
} // namespace Licasa
