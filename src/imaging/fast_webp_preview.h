#pragma once

#include <QImage>
#include <QSize>

#include <atomic>

namespace Licasa {
// Header-only eligibility check; actual decode still validates the bitstream.
bool fastWebpBaseEligible(const QString& path);
// Decode a bounded preview for simple, static WebP files. For costly large
// files, optionally retain the full raster so promotion avoids a second decode.
// ICC and animated files keep Qt's reader. Callers must preserve Qt's
// orientation handling when an EXIF transform is present.
QImage readFastWebpPreview(const QString& path, const QSize& target, quint64 maximumPixels,
                           qsizetype maximumPreviewBytes, const std::atomic_bool* cancelled,
                           QImage* retainedFullBase = nullptr, qsizetype maximumRetainedBytes = 0);
} // namespace Licasa
