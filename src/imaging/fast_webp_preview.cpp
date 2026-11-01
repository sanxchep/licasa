#include "imaging/fast_webp_preview.h"
#include "imaging/parallel_smooth_scale.h"

#include <QColorSpace>
#include <QFile>
#include <QtEndian>

#include <webp/decode.h>

#include <cstring>
#include <limits>
#include <utility>

namespace Licasa {
namespace {
bool staticWebpEnvelope(QFile& file)
{
    if (file.size() < 20 || file.size() > 512LL * 1024 * 1024 || !file.seek(0)) {
        return false;
    }
    const QByteArray header = file.read(12);
    if (header.size() != 12 || !header.startsWith("RIFF") || header.mid(8, 4) != "WEBP" ||
        quint64(qFromLittleEndian<quint32>(header.constData() + 4)) + 8 != quint64(file.size())) {
        return false;
    }
    bool image = false;
    for (int chunk = 0; chunk < 64 && file.pos() + 8 <= file.size(); ++chunk) {
        const QByteArray entry = file.read(8);
        if (entry.size() != 8) {
            return false;
        }
        const quint64 length = qFromLittleEndian<quint32>(entry.constData() + 4);
        const quint64 end = quint64(file.pos()) + length + (length & 1);
        if (end > quint64(file.size())) {
            return false;
        }
        const QByteArray type = entry.left(4);
        if (type == "VP8X") {
            if (chunk != 0 || length != 10) {
                return false;
            }
            const QByteArray extended = file.read(10);
            // Keep animation, ICC and reserved flags on Qt's metadata path.
            if (extended.size() != 10 || (uchar(extended[0]) & ~0x1c) != 0) {
                return false;
            }
        } else if (type == "VP8 " || type == "VP8L") {
            if (image) {
                return false;
            }
            image = true;
        } else if (type != "ALPH" && type != "EXIF" && type != "XMP ") {
            return false;
        }
        if (!file.seek(qint64(end))) {
            return false;
        }
        if (end == quint64(file.size())) {
            return image;
        }
    }
    return false;
}
} // namespace

bool fastWebpBaseEligible(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.size() >= 16 * 1024 * 1024 &&
           staticWebpEnvelope(file);
}

QImage readFastWebpPreview(const QString& path, const QSize& target, quint64 maximumPixels,
                           qsizetype maximumPreviewBytes, const std::atomic_bool* cancelled,
                           QImage* retainedFullBase, qsizetype maximumRetainedBytes)
{
    if (retainedFullBase) {
        *retainedFullBase = {};
    }
    if (!target.isValid() || target.isEmpty() || target.width() > 8192 || target.height() > 8192 ||
        quint64(target.width()) * target.height() > 2'000'000 || maximumPreviewBytes <= 0 ||
        (cancelled && cancelled->load(std::memory_order_relaxed))) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 20 || file.size() > 512LL * 1024 * 1024 ||
        quint64(file.size()) > std::numeric_limits<size_t>::max()) {
        return {};
    }
    if (!staticWebpEnvelope(file)) {
        return {};
    }
    const uchar* const bytes = file.map(0, file.size());
    if (!bytes) {
        return {};
    }
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config) ||
        WebPGetFeatures(bytes, size_t(file.size()), &config.input) != VP8_STATUS_OK ||
        config.input.has_animation || config.input.width <= 0 || config.input.height <= 0 ||
        quint64(config.input.width) * config.input.height > maximumPixels ||
        target.width() > config.input.width || target.height() > config.input.height) {
        return {};
    }
    const quint64 sourceBytes = quint64(config.input.width) * config.input.height * 4;
    const bool retainBase = retainedFullBase && maximumRetainedBytes > 0 &&
                            file.size() >= 16 * 1024 * 1024 && sourceBytes >= 100'000'000 &&
                            sourceBytes <= quint64(maximumRetainedBytes);
    // Opaque native-endian output can be consumed directly by the software
    // renderer. Alpha/big-endian paths retain the existing RGBA contract.
    const bool nativeOpaque = Q_BYTE_ORDER == Q_LITTLE_ENDIAN && !config.input.has_alpha;
    QImage image(retainBase ? QSize(config.input.width, config.input.height) : target,
                 nativeOpaque ? QImage::Format_RGB32 : QImage::Format_RGBA8888);
    if (image.isNull() || (!retainBase && image.sizeInBytes() > maximumPreviewBytes) ||
        (cancelled && cancelled->load(std::memory_order_relaxed))) {
        return {};
    }
    if (!retainBase) {
        config.options.use_scaling = 1;
        config.options.scaled_width = target.width();
        config.options.scaled_height = target.height();
    }
    config.options.use_threads = 1;
    config.output.colorspace = nativeOpaque ? MODE_BGRA : MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = image.bits();
    config.output.u.RGBA.stride = int(image.bytesPerLine());
    config.output.u.RGBA.size = size_t(image.sizeInBytes());
    const VP8StatusCode status = WebPDecode(bytes, size_t(file.size()), &config);
    WebPFreeDecBuffer(&config.output);
    if (status != VP8_STATUS_OK || (cancelled && cancelled->load(std::memory_order_relaxed))) {
        return {};
    }
    image.setColorSpace(QColorSpace::SRgb);
    if (retainBase) {
        QImage preview = parallelSmoothScale(image, target);
        if (preview.isNull() || preview.sizeInBytes() > maximumPreviewBytes ||
            (cancelled && cancelled->load(std::memory_order_relaxed))) {
            return {};
        }
        *retainedFullBase = std::move(image);
        return preview;
    }
    return image;
}
} // namespace Licasa
