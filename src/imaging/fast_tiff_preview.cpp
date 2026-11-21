#include "imaging/fast_tiff_preview.h"

#include <QByteArray>
#include <QColorSpace>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace Licasa {
namespace {
constexpr quint64 kMaximumSourcePixels = 200000000;
constexpr tmsize_t kMaximumScanlineBytes = 1024 * 1024;
constexpr tmsize_t kMaximumStripBytes = 8 * 1024 * 1024;
constexpr size_t kMaximumSamplingOffsets = 64 * 1024 / sizeof(uint32_t);

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
    const bool tracing = qEnvironmentVariableIsSet("LICASA_TRACE_FAST_TIFF");
    QElapsedTimer timer;
    if (tracing) {
        timer.start();
    }
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
        quint64(width) * quint64(height) > kMaximumSourcePixels || planar != PLANARCONFIG_CONTIG ||
        orientation != ORIENTATION_TOPLEFT || sampleFormat != SAMPLEFORMAT_UINT ||
        TIFFIsTiled(tiff.get())) {
        return {};
    }

    const bool gray16 =
        bits == 16 && samples == 1 &&
        (photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE) &&
        compression == COMPRESSION_NONE;
    const bool rgb8 = bits == 8 && samples == 3 && photometric == PHOTOMETRIC_RGB;
    const bool rgba8 = bits == 8 && samples == 4 && photometric == PHOTOMETRIC_RGB;
    if (!gray16 && !rgb8 && !rgba8) {
        return {};
    }
    if ((rgb8 || rgba8) && compression != COMPRESSION_NONE &&
        compression != COMPRESSION_ADOBE_DEFLATE && compression != COMPRESSION_DEFLATE) {
        return {};
    }
    if (rgba8) {
        uint16_t extraCount = 0;
        uint16_t* extraTypes = nullptr;
        if (!TIFFGetField(tiff.get(), TIFFTAG_EXTRASAMPLES, &extraCount, &extraTypes) ||
            extraCount != 1 || !extraTypes || extraTypes[0] != EXTRASAMPLE_UNASSALPHA) {
            return {};
        }
    }

    const tmsize_t scanlineBytes = TIFFScanlineSize(tiff.get());
    const tmsize_t stripBytes = TIFFStripSize(tiff.get());
    uint32_t rowsPerStrip = 0;
    if (!TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_ROWSPERSTRIP, &rowsPerStrip) ||
        rowsPerStrip == 0) {
        return {};
    }
    const int bytesPerPixel = gray16 ? 2 : int(samples);
    if (scanlineBytes < tmsize_t(quint64(width) * quint64(bytesPerPixel)) ||
        scanlineBytes > kMaximumScanlineBytes || stripBytes <= 0 ||
        stripBytes > kMaximumStripBytes) {
        return {};
    }

    uint16_t fillOrder = 0;
    const bool sampleRawRows = !gray16 && compression == COMPRESSION_NONE &&
        quint64(width) * height >= 25000000 &&
        scanlineBytes == tmsize_t(quint64(width) * bytesPerPixel) &&
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_FILLORDER, &fillOrder) &&
        fillOrder == FILLORDER_MSB2LSB;
    const thandle_t handle = TIFFClientdata(tiff.get());
    const quint64 fileSize = sampleRawRows ? TIFFGetSizeProc(tiff.get())(handle) : 0;

    const int outputBytesPerPixel = gray16 ? 1 : bytesPerPixel;
    const quint64 pixelLimit =
        std::min(maximumOutputPixels, quint64(maximumOutputBytes) / quint64(outputBytesPerPixel));
    QSize target = outputSize(QSize(int(width), int(height)), requestedSize, pixelLimit);
    if (!target.isValid() || target.isEmpty()) {
        return {};
    }
    QImage image;
    while (target.width() > 0 && target.height() > 0) {
        image = QImage(target, gray16 ? QImage::Format_Grayscale8
                               : rgb8 ? QImage::Format_RGB888
                                      : QImage::Format_RGBA8888);
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

    // Reuse the exact floor mapping across rows, with at most 64 KiB of
    // additional scratch. Very wide outputs keep the original direct mapping.
    std::vector<uint32_t> sourceColumns;
    if (size_t(target.width()) <= kMaximumSamplingOffsets) {
        try {
            sourceColumns.resize(size_t(target.width()));
        } catch (const std::bad_alloc&) {
            // Sampling remains valid without the optional coordinate table.
        }
        for (size_t x = 0; x < sourceColumns.size(); ++x) {
            sourceColumns[x] = uint32_t(quint64(x) * width / quint64(target.width()));
        }
    }
    const auto sourceColumn = [&](int x) {
        return sourceColumns.empty() ? uint32_t(quint64(x) * width / quint64(target.width()))
                                     : sourceColumns[size_t(x)];
    };

    std::vector<quint16> grayScanline(gray16 ? size_t((scanlineBytes + 1) / 2) : 0);
    std::vector<quint16> stripBuffer(
        gray16 ? 0 : size_t(((sampleRawRows ? scanlineBytes : stripBytes) + 1) / 2));
    quint64 rawStripOffset = 0;
    quint64 rawStripBytes = 0;
    tstrip_t loadedStrip = std::numeric_limits<tstrip_t>::max();
    tmsize_t loadedBytes = 0;
    for (int y = 0; y < target.height(); ++y) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            return {};
        }
        const uint32_t sourceY = uint32_t(quint64(y) * height / quint64(target.height()));
        const uchar* sourceBytes = nullptr;
        if (gray16) {
            if (TIFFReadScanline(tiff.get(), grayScanline.data(), sourceY, 0) < 0) {
                return {};
            }
            sourceBytes = reinterpret_cast<const uchar*>(grayScanline.data());
        } else if (sampleRawRows) {
            const tstrip_t strip = TIFFComputeStrip(tiff.get(), sourceY, 0);
            if (strip != loadedStrip) {
                int offsetError = 0;
                int countError = 0;
                rawStripOffset = TIFFGetStrileOffsetWithErr(tiff.get(), strip, &offsetError);
                rawStripBytes = TIFFGetStrileByteCountWithErr(tiff.get(), strip, &countError);
                if (offsetError || countError || !rawStripOffset || rawStripOffset > fileSize ||
                    rawStripBytes > fileSize - rawStripOffset) {
                    return {};
                }
                loadedStrip = strip;
            }
            const quint64 rowOffset = quint64(sourceY % rowsPerStrip) * quint64(scanlineBytes);
            if (rowOffset > rawStripBytes || quint64(scanlineBytes) > rawStripBytes - rowOffset) {
                return {};
            }
            // With no compression or bit reversal, these 8-bit samples are
            // already in output channel order. Read from the same TIFF handle.
            const quint64 position = rawStripOffset + rowOffset;
            if (TIFFGetSeekProc(tiff.get())(handle, position, SEEK_SET) != position ||
                TIFFGetReadProc(tiff.get())(handle, stripBuffer.data(), scanlineBytes) !=
                    scanlineBytes) {
                return {};
            }
            sourceBytes = reinterpret_cast<const uchar*>(stripBuffer.data());
        } else {
            const tstrip_t strip = TIFFComputeStrip(tiff.get(), sourceY, 0);
            if (strip != loadedStrip) {
                loadedBytes =
                    TIFFReadEncodedStrip(tiff.get(), strip, stripBuffer.data(), stripBytes);
                if (loadedBytes < 0) {
                    return {};
                }
                loadedStrip = strip;
            }
            const quint64 rowOffset = quint64(sourceY % rowsPerStrip) * quint64(scanlineBytes);
            if (rowOffset > quint64(loadedBytes) ||
                quint64(width) * quint64(bytesPerPixel) > quint64(loadedBytes) - rowOffset) {
                return {};
            }
            sourceBytes = reinterpret_cast<const uchar*>(stripBuffer.data()) + rowOffset;
        }
        uchar* output = image.scanLine(y);
        if (gray16) {
            const auto* sourceGray = reinterpret_cast<const quint16*>(sourceBytes);
            for (int x = 0; x < target.width(); ++x) {
                const quint16 value = sourceGray[sourceColumn(x)];
                output[x] = uchar(
                    (photometric == PHOTOMETRIC_MINISWHITE ? quint16(65535 - value) : value) >> 8);
            }
        } else if (target.width() == int(width)) {
            // The admitted strip already has the output channel order. Copy
            // visible bytes only; QImage may pad the end of an RGB888 row.
            std::memcpy(output, sourceBytes, size_t(width) * size_t(bytesPerPixel));
        } else if (rgb8) {
            for (int x = 0; x < target.width(); ++x) {
                const uchar* pixel = sourceBytes + size_t(sourceColumn(x)) * 3;
                output[3 * x] = pixel[0];
                output[3 * x + 1] = pixel[1];
                output[3 * x + 2] = pixel[2];
            }
        } else {
            for (int x = 0; x < target.width(); ++x) {
                std::memcpy(output + 4 * x, sourceBytes + size_t(sourceColumn(x)) * 4, 4);
            }
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
    if (tracing) {
        qInfo().noquote() << "[Licasa] Fast TIFF preview:" << "source="
                          << QSize(int(width), int(height)) << "output=" << image.size()
                          << "elapsed_ms=" << double(timer.nsecsElapsed()) / 1000000.0;
    }
    return image;
}

} // namespace Licasa
