#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <atomic>

namespace Licasa {

// Return an early, lower-detail progressive JPEG pass for the opening preview.
// A null image means the ordinary Qt reader should handle this file instead.
QImage readEarlyProgressiveJpegPreview(const QString& path, const QSize& requestedSize,
                                       quint64 maximumPixels, const std::atomic_bool* cancelled);

} // namespace Licasa
