#include "imaging/fast_tiff_preview.h"

#include <QByteArray>
#include <QColorSpace>
#include <QFile>

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Licasa {
namespace {
constexpr quint64 kMaximumSourcePixels = 200000000;
constexpr tmsize_t kMaximumScanlineBytes = 1024 * 1024;
constexpr tmsize_t kMaximumStripBytes = 8 * 1024 * 1024;

QSize outputSize(const QSize& source, const QSize& requested, quint64 maximumPixels)
{
    if (!requested.isValid() || requested.isEmpty() || maximumPixels == 0) {
        return {};
    }
    QSize target = source.scaled(requested, Qt::KeepAspectRatio);
    if (!target.isValid() || target.isEmpty()) {
        return {};
    }
    const quint64 pixels = quint64(target.width()) * quint64(target.height());
    if (pixels <= maximumPixels) {
        return target;
    }
    const long double factor =
        std::sqrt(static_cast<long double>(maximumPixels) / static_cast<long double>(pixels));
    target = QSize(std::max(1, int(std::floor(target.width() * factor))),
                   std::max(1, int(std::floor(target.height() * factor))));
    while (quint64(target.width()) * quint64(target.height()) > maximumPixels) {
        if (target.width() >= target.height()) {
            target.rwidth() -= 1;
        } else {
            target.rheight() -= 1;
        }
    }
    return target;
}
} // namespace

QImage readFastTiffPreview(const QString& path, const QSize& requestedSize,
                           quint64 maximumOutputPixels, qsizetype maximumOutputBytes,
                           std::atomic_bool* cancelled)
{
    if (maximumOutputBytes <= 0 || maximumOutputPixels == 0 ||
        (cancelled && cancelled->load(std::memory_order_relaxed))) {
        return {};
    }

    const QByteArray encodedPath = QFile::encodeName(path);
    TIFFOpenOptions* options = TIFFOpenOptionsAlloc();
    if (!options) {
        return {};
    }
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, kMaximumStripBytes);
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(
        TIFFOpenExt(encodedPath.constData(), "rm", options), &TIFFClose);
    TIFFOpenOptionsFree(options);
    if (!tiff) {
        return {};
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bits = 0;
    uint16_t samples = 0;
    uint16_t photometric = 0;
    uint16_t compression = 0;
    uint16_t planar = 0;
    uint16_t orientation = 0;
    uint16_t sampleFormat = 0;
    if (!TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) ||
        !TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &photometric) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_COMPRESSION, &compression) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planar) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_ORIENTATION, &orientation) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sampleFormat) || !width ||
        !height || width > uint32_t(std::numeric_limits<int>::max()) ||
        height > uint32_t(std::numeric_limits<int>::max()) ||
        quint64(width) * quint64(height) > kMaximumSourcePixels || bits != 16 || samples != 1 ||
        (photometric != PHOTOMETRIC_MINISBLACK && photometric != PHOTOMETRIC_MINISWHITE) ||
        compression != COMPRESSION_NONE || planar != PLANARCONFIG_CONTIG ||
        orientation != ORIENTATION_TOPLEFT || sampleFormat != SAMPLEFORMAT_UINT ||
        TIFFIsTiled(tiff.get())) {
        return {};
    }

    const tmsize_t scanlineBytes = TIFFScanlineSize(tiff.get());
    const tmsize_t stripBytes = TIFFStripSize(tiff.get());
    if (scanlineBytes < tmsize_t(quint64(width) * 2) || scanlineBytes > kMaximumScanlineBytes ||
        stripBytes <= 0 || stripBytes > kMaximumStripBytes) {
        return {};
    }

    const quint64 pixelLimit = std::min(maximumOutputPixels, quint64(maximumOutputBytes));
    QSize target = outputSize(QSize(int(width), int(height)), requestedSize, pixelLimit);
    if (!target.isValid() || target.isEmpty()) {
        return {};
    }
    QImage image;
    while (target.width() > 0 && target.height() > 0) {
        image = QImage(target, QImage::Format_Grayscale8);
        if (!image.isNull() && image.sizeInBytes() <= maximumOutputBytes) {
            break;
        }
        if (target.width() >= target.height()) {
            target.rwidth() -= 1;
        } else {
            target.rheight() -= 1;
        }
    }
    if (image.isNull() || image.sizeInBytes() > maximumOutputBytes) {
        return {};
    }

    std::vector<quint16> scanline(size_t((scanlineBytes + 1) / 2), quint16(0));
    for (int y = 0; y < target.height(); ++y) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            return {};
        }
        const uint32_t sourceY = uint32_t(quint64(y) * height / quint64(target.height()));
        if (TIFFReadScanline(tiff.get(), scanline.data(), sourceY, 0) < 0) {
            return {};
        }
        uchar* output = image.scanLine(y);
        for (int x = 0; x < target.width(); ++x) {
            const uint32_t sourceX = uint32_t(quint64(x) * width / quint64(target.width()));
            const quint16 value = scanline[sourceX];
            output[x] = uchar(
                (photometric == PHOTOMETRIC_MINISWHITE ? quint16(65535 - value) : value) >> 8);
        }
    }

    uint32_t iccBytes = 0;
    void* iccData = nullptr;
    if (TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &iccBytes, &iccData) && iccData &&
        iccBytes > 0 && iccBytes <= 1024 * 1024) {
        const QColorSpace profile = QColorSpace::fromIccProfile(
            QByteArray(static_cast<const char*>(iccData), int(iccBytes)));
        if (profile.isValid()) {
            image.setColorSpace(profile);
        }
    }
    return image;
}

} // namespace Licasa
