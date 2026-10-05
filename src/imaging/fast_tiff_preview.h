#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <atomic>

namespace Licasa {

// Return a bounded preview for uncompressed, stripped, 16-bit grayscale TIFFs.
// Other TIFF layouts stay on the ordinary Qt decoder path.
QImage readFastTiffPreview(const QString& path, const QSize& requestedSize,
                           quint64 maximumOutputPixels, qsizetype maximumOutputBytes,
                           std::atomic_bool* cancelled);

} // namespace Licasa
