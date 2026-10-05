#pragma once

#include <QIODevice>
#include <QImageReader>
#include <QSize>
#include <QVariant>

#include <atomic>

// A narrow, per-reader bridge to application-owned Qt image plugins. It carries
// ImageResourcePolicy's existing admission snapshot; it does not own another
// application policy. Values are set once, before handing the reader to a
// worker/handler, and are never taken from file metadata.
namespace Licasa::ImageDecodeContract {
inline constexpr char pixelBudgetProperty[] = "_licasaPixelBudget";
inline constexpr char cancellationProperty[] = "_licasaCancellation";
inline constexpr char errorProperty[] = "_licasaCodecError";
inline constexpr char fullDetailProperty[] = "_licasaFullDetail";
inline constexpr char rawFastDevelopmentProperty[] = "_licasaRawFastDevelopment";
inline constexpr char rawInteractiveDevelopmentProperty[] = "_licasaRawInteractiveDevelopment";
inline constexpr char previewFirstProperty[] = "_licasaPreviewFirst";
inline constexpr char sensorSizeProperty[] = "_licasaSensorSize";
inline constexpr char embeddedPreviewSizeProperty[] = "_licasaEmbeddedPreviewSize";
// A RAW plugin may admit an oversized embedded JPEG only through native,
// bounded JPEG scaling; this flag lets the selected-preview worker use it.
inline constexpr char scalableEmbeddedJpegProperty[] = "_licasaScalableEmbeddedJpeg";
inline constexpr char previewPresentedProperty[] = "_licasaPreviewPresented";
// Metadata-only HEIF Exif exposure is opt-in. Ordinary HEIC/HEIF probing keeps
// this false unless a same-basename Apple MOV candidate exists.
inline constexpr char appleLivePhotoExifProbeProperty[] = "_licasaAppleLivePhotoExifProbe";

inline bool allows(const QSize& size, quint64 maximumPixels) noexcept
{
    return size.width() > 0 && size.height() > 0 &&
           quint64(size.width()) * quint64(size.height()) <= maximumPixels;
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
