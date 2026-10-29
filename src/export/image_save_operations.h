#pragma once

#include "io/external_file_identity.h"
#include <QString>
#include <QUrl>
#include <functional>

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

// The internal observer allows deterministic transaction-boundary failure tests;
// application callers leave it empty. It is never exposed through QML or settings.
ImageSaveResult writeEditedImage(const QString& sourcePath, QString destinationPath,
                                 const ImageEditParameters& parameters,
                                 const ImageExportOptions& options, int maximumMegapixels,
                                 std::optional<ExternalFileIdentity> expectedSource = {},
                                 const std::function<void()>& beforeCommit = {});
ImageSaveResult writeFrameImage(const MotionPhotoFrameRaster& frame, QString destinationPath,
                                const ImageExportOptions& options, int maximumMegapixels);
SizeEstimateResult measureEditedImageSize(const QString& sourcePath,
                                          const ImageEditParameters& parameters,
                                          const ImageExportOptions& options, int maximumMegapixels);

} // namespace ImageSaveOperations
} // namespace Licasa
