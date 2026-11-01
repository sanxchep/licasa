#pragma once

#include "imaging/image_decode_contract.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <atomic>

namespace Licasa {

struct FastPngPreview {
    QImage preview;
    QImage fullBase;
};

// Admit only large, plain RGB/RGBA PNGs whose full 32-bit raster fits the
// selected base limit. Other PNGs stay with QImageReader.
bool fastPngBaseEligible(
    const QString& path, quint64 maximumPixels,
    quint64 maximumFullBaseBytes = ImageDecodeContract::maximumDecodedBaseCacheBytes);

FastPngPreview readFastPngPreview(
    const QString& path, const QSize& previewSize, quint64 maximumPixels,
    qsizetype maximumPreviewBytes, const std::atomic_bool* cancelled,
    quint64 maximumFullBaseBytes = ImageDecodeContract::maximumDecodedBaseCacheBytes);

// Background nearby previews keep only the sampled raster, never the large
// decoded base used for selected-image full-detail promotion.
QImage readFastPngNearbyPreview(const QString& path, const QSize& previewSize,
                                quint64 maximumPixels, qsizetype maximumPreviewBytes,
                                const std::atomic_bool* cancelled);

} // namespace Licasa
