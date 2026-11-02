#pragma once

#include <QIODevice>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QVariant>

#include <algorithm>
#include <atomic>

// A narrow, per-reader bridge to application-owned Qt image plugins. It carries
// ImageResourcePolicy's existing admission snapshot; it does not own another
// application policy. Values are set once, before handing the reader to a
// worker/handler, and are never taken from file metadata.
namespace Licasa::ImageDecodeContract {
inline constexpr quint64 maximumDecodedBaseCacheBytes = 256ULL * 1024 * 1024;
// A selected preview may hand an already-decoded raster to its imminent full
// request. Larger bases are consumed once, never retained for subsequent edits.
inline constexpr quint64 maximumFullBaseHandoffBytes = 512ULL * 1024 * 1024;

inline bool boundedRasterFullBase(const QSize& size, quint64 maximumPixels,
                                  quint64 maximumBytes = maximumDecodedBaseCacheBytes) noexcept
{
    if (size.width() <= 0 || size.height() <= 0) {
        return false;
    }
    const quint64 pixels = quint64(size.width()) * quint64(size.height());
    return pixels >= 25000000 && pixels <= maximumPixels &&
           pixels <= std::min(maximumBytes, maximumFullBaseHandoffBytes) / 4;
}

inline bool reusablePngFullBase(const QSize& size, QImage::Format format,
                                quint64 maximumPixels) noexcept
{
    if (format != QImage::Format_ARGB32 && format != QImage::Format_RGB32) {
        return false;
    }
    const quint64 pixels = quint64(size.width()) * quint64(size.height());
    // When the image fits half the pixel budget, paired preview/full reads
    // can overlap and are faster than waiting for a retained preview base.
    return boundedRasterFullBase(size, maximumPixels) && pixels > maximumPixels / 2;
}

inline constexpr char pixelBudgetProperty[] = "_licasaPixelBudget";
// A native-scaling reader may admit a large source while bounding the raster
// each attempted reduced-resolution decode is allowed to allocate.
inline constexpr char scaledDecodePixelBudgetProperty[] = "_licasaScaledDecodePixelBudget";
inline constexpr char boundedJp2ScaleProperty[] = "_licasaBoundedJp2Scale";
inline constexpr char cancellationProperty[] = "_licasaCancellation";
inline constexpr char errorProperty[] = "_licasaCodecError";
inline constexpr char fullDetailProperty[] = "_licasaFullDetail";
inline constexpr char rawFastDevelopmentProperty[] = "_licasaRawFastDevelopment";
inline constexpr char rawInteractiveDevelopmentProperty[] = "_licasaRawInteractiveDevelopment";
inline constexpr char previewFirstProperty[] = "_licasaPreviewFirst";
inline constexpr char sensorSizeProperty[] = "_licasaSensorSize";
inline constexpr char embeddedPreviewSizeProperty[] = "_licasaEmbeddedPreviewSize";
// RAW metadata can rule out a thumbnail even if its dimensions are unknown.
inline constexpr char rawPreviewCandidateProperty[] = "_licasaRawPreviewCandidate";
// HEIF can construct a scaled grid preview from individual bounded tiles.
inline constexpr char boundedGridPreviewProperty[] = "_licasaBoundedGridPreview";
// A RAW plugin may admit an oversized embedded JPEG only through native,
// bounded JPEG scaling; this flag lets the selected-preview worker use it.
inline constexpr char scalableEmbeddedJpegProperty[] = "_licasaScalableEmbeddedJpeg";
// A single-image decoder that must materialize the full raster for a scaled
// preview can offer that immutable base to the provider's bounded edit cache.
inline constexpr char retainFullBaseProperty[] = "_licasaRetainFullBase";
inline constexpr char fullBaseByteLimitProperty[] = "_licasaFullBaseByteLimit";
inline constexpr char decodedFullBaseProperty[] = "_licasaDecodedFullBase";
inline constexpr char previewPresentedProperty[] = "_licasaPreviewPresented";
// Metadata-only HEIF Exif exposure is opt-in. Ordinary HEIC/HEIF probing keeps
// this false unless a same-basename Apple MOV candidate exists.
inline constexpr char appleLivePhotoExifProbeProperty[] = "_licasaAppleLivePhotoExifProbe";

inline bool allows(const QSize& size, quint64 maximumPixels) noexcept
{
    return size.width() > 0 && size.height() > 0 &&
           quint64(size.width()) * quint64(size.height()) <= maximumPixels;
}

inline quint64 fullBaseByteLimit(QIODevice* device)
{
    const QVariant value = device ? device->property(fullBaseByteLimitProperty) : QVariant();
    return value.isValid() ? std::min(value.toULongLong(), maximumFullBaseHandoffBytes)
                           : maximumDecodedBaseCacheBytes;
}

inline void configure(QImageReader& reader, quint64 pixels,
                      const std::atomic_bool* cancelled = nullptr, bool fullDetail = false,
                      bool rawFastDevelopment = false, bool rawInteractiveDevelopment = false)
{
    if (auto* device = reader.device()) {
        device->setProperty(pixelBudgetProperty, QVariant::fromValue(pixels));
        device->setProperty(fullDetailProperty, fullDetail);
        device->setProperty(rawFastDevelopmentProperty, rawFastDevelopment);
        device->setProperty(rawInteractiveDevelopmentProperty, rawInteractiveDevelopment);
        // The response owns this atomic until read() returns and finished is
        // emitted. Plugins must not retain it beyond their reader lifetime.
        device->setProperty(cancellationProperty,
                            QVariant::fromValue(reinterpret_cast<quintptr>(cancelled)));
    }
}

inline quint64 pixelBudget(QIODevice* device)
{
    const auto value = device ? device->property(pixelBudgetProperty) : QVariant();
    if (value.isValid()) {
        return value.toULongLong();
    }
    // Other Qt consumers still get Qt's allocation policy. The backend's own
    // maintained limits remain enabled even when such a consumer requests no
    // Qt allocation cap. Licasa always supplies the exact pixel snapshot above.
    const int cap = QImageReader::allocationLimit();
    return cap > 0 ? quint64(cap) * 1024 * 1024 / 4 : ~quint64(0);
}

inline bool cancelled(QIODevice* device)
{
    const auto pointer = device ? device->property(cancellationProperty).value<quintptr>() : 0;
    const auto* flag = reinterpret_cast<const std::atomic_bool*>(pointer);
    return flag && flag->load(std::memory_order_relaxed);
}
} // namespace Licasa::ImageDecodeContract
