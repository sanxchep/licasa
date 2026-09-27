#include "imaging/image_decode_contract.h"

#include <QByteArray>
#include <QColorSpace>
#include <QImageIOHandler>
#include <QThread>
#include <QTransform>
#include <QtEndian>

#include <avif/avif.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace {
namespace Contract = Licasa::ImageDecodeContract;
constexpr quint64 metadataBudget = 4 * 1024 * 1024;
constexpr qsizetype maximumMotionXmpBytes = 1024 * 1024;
constexpr quint64 minimumReadWindow = 16 * 1024 * 1024;
constexpr quint64 maximumReadWindow = 256 * 1024 * 1024;
using Decoder = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;

bool isAvifBrand(const QByteArray& brand) { return brand == "avif" || brand == "avis"; }

bool avifSignature(QIODevice* device)
{
    if (!device || !device->isReadable()) {
        return false;
    }

    // The entire file-type box is not trusted or buffered. The major brand is
    // sufficient for normal AVIFs; a bounded compatible-brand scan also covers
    // the common mif1-compatible layout.
    const QByteArray header = device->peek(512);
    if (header.size() < 16 || header.mid(4, 4) != "ftyp") {
        return false;
    }

    const quint32 size32 = qFromBigEndian<quint32>(header.constData());
    qsizetype brandOffset = 8;
    quint64 boxSize = size32;
    if (size32 == 1) {
        if (header.size() < 24) {
            return false;
        }
        boxSize = qFromBigEndian<quint64>(header.constData() + 8);
        brandOffset = 16;
    } else if (size32 == 0) {
        const qint64 size = device->size();
        if (size <= 0) {
            return false;
        }
        boxSize = quint64(size);
    }

    if (boxSize < quint64(brandOffset + 8)) {
        return false;
    }
    if (isAvifBrand(header.mid(brandOffset, 4))) {
        return true;
    }

    const qsizetype scanEnd = std::min<quint64>(boxSize, quint64(header.size()));
    for (qsizetype offset = brandOffset + 8; offset + 4 <= scanEnd; offset += 4) {
        if (isAvifBrand(header.mid(offset, 4))) {
            return true;
        }
    }
    return false;
}

class DeviceIO final {
  public:
    bool initialize(QIODevice* device, quint64 pixelBudget)
    {
        if (!device || !device->isReadable() || device->isSequential()) {
            return false;
        }
        const qint64 origin = device->pos();
        const qint64 end = device->size();
        if (origin < 0 || end < origin) {
            return false;
        }

        device_ = device;
        origin_ = origin;
        length_ = quint64(end - origin);
        const quint64 budgetBytes =
            pixelBudget > (std::numeric_limits<quint64>::max() - minimumReadWindow) / 4
                ? std::numeric_limits<quint64>::max()
                : pixelBudget * 4 + minimumReadWindow;
        maximumRead_ = std::min(maximumReadWindow, std::max(minimumReadWindow, budgetBytes));

        io_ = {};
        io_.read = &DeviceIO::read;
        io_.write = nullptr;
        io_.sizeHint = length_;
        io_.persistent = AVIF_FALSE;
        io_.data = this;
        return true;
    }

    avifIO* io() { return &io_; }

  private:
    static avifResult read(avifIO* io, uint32_t readFlags, uint64_t offset, size_t requested,
                           avifROData* out)
    {
        if (!io || !out || readFlags != 0) {
            return AVIF_RESULT_IO_ERROR;
        }
        auto* self = static_cast<DeviceIO*>(io->data);
        if (!self || !self->device_ || Contract::cancelled(self->device_)) {
            return AVIF_RESULT_IO_ERROR;
        }

        out->data = nullptr;
        out->size = 0;
        if (offset > self->length_) {
            return AVIF_RESULT_IO_ERROR;
        }
        if (offset == self->length_) {
            return AVIF_RESULT_OK;
        }

        const quint64 available = self->length_ - offset;
        const quint64 wanted = std::min<quint64>(quint64(requested), available);
        if (wanted > self->maximumRead_ ||
            wanted > quint64(std::numeric_limits<qsizetype>::max()) ||
            wanted > quint64(std::numeric_limits<qint64>::max())) {
            return AVIF_RESULT_IO_ERROR;
        }
        if (offset > quint64(std::numeric_limits<qint64>::max()) ||
            quint64(self->origin_) > quint64(std::numeric_limits<qint64>::max()) - offset) {
            return AVIF_RESULT_IO_ERROR;
        }

        const qint64 absolute = self->origin_ + qint64(offset);
        if (!self->device_->seek(absolute)) {
            return AVIF_RESULT_IO_ERROR;
        }

        self->buffer_.resize(qsizetype(wanted));
        qint64 total = 0;
        while (total < qint64(wanted)) {
            if (Contract::cancelled(self->device_)) {
                return AVIF_RESULT_IO_ERROR;
            }
            const qint64 count =
                self->device_->read(self->buffer_.data() + total, qint64(wanted) - total);
            if (count <= 0) {
                return AVIF_RESULT_IO_ERROR;
            }
            total += count;
        }
        out->data = reinterpret_cast<const uint8_t*>(self->buffer_.constData());
        out->size = size_t(wanted);
        return AVIF_RESULT_OK;
    }

    avifIO io_{};
    QIODevice* device_ = nullptr;
    QByteArray buffer_;
    qint64 origin_ = 0;
    quint64 length_ = 0;
    quint64 maximumRead_ = minimumReadWindow;
};

QColorSpace colorSpaceFor(const avifImage* image)
{
    if (!image) {
        return {};
    }
    if (image->icc.size > 0 && image->icc.size <= metadataBudget && image->icc.data) {
        const QByteArray profile(reinterpret_cast<const char*>(image->icc.data),
                                 qsizetype(image->icc.size));
        const QColorSpace space = QColorSpace::fromIccProfile(profile);
        if (space.isValid()) {
            return space;
        }
    }
    if (image->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_SRGB) {
        if (image->colorPrimaries == AVIF_COLOR_PRIMARIES_BT709) {
            return QColorSpace(QColorSpace::SRgb);
        }
        if (image->colorPrimaries == AVIF_COLOR_PRIMARIES_SMPTE432) {
            return QColorSpace(QColorSpace::DisplayP3);
        }
    }
    // Do not label unknown/PQ/HLG content as sRGB. Gain-map/HDR presentation is
    // a later explicit roadmap stage.
    return {};
}

bool fallbackCropRectForFractionalClap(const avifImage* image, avifCropRect* rect)
{
    if (!image || !rect || image->width == 0 || image->height == 0) {
        return false;
    }

    // avifCropRectFromCleanApertureBox() rejects clean apertures whose
    // display rectangle cannot be represented as an exact integer source
    // rectangle. Real AVIFs exist with half-pixel clean-aperture positions.
    // Licasa converts the complete YUV raster to RGB before applying clap, so
    // those cases can be mapped safely in RGB space.
    //
    // ISO-BMFF defines the aperture relative to the image centre. The integer
    // raster mapping below follows the centre-relative clap mapping used by
    // libheif:
    //   pcX  = horizOff + (imageWidth  - 1) / 2
    //   pcY  = vertOff  + (imageHeight - 1) / 2
    //   left = pcX - (clapWidth  - 1) / 2
    //   top  = pcY - (clapHeight - 1) / 2
    //
    // Horizontal origin is rounded down; vertical origin and the inclusive
    // right/bottom edges are rounded to nearest. All arithmetic is bounded by
    // the 32-bit clap representation before it is converted back to a raster
    // rectangle.
    const int32_t widthN = static_cast<int32_t>(image->clap.widthN);
    const int32_t widthD = static_cast<int32_t>(image->clap.widthD);
    const int32_t heightN = static_cast<int32_t>(image->clap.heightN);
    const int32_t heightD = static_cast<int32_t>(image->clap.heightD);
    const int32_t horizOffN = static_cast<int32_t>(image->clap.horizOffN);
    const int32_t horizOffD = static_cast<int32_t>(image->clap.horizOffD);
    const int32_t vertOffN = static_cast<int32_t>(image->clap.vertOffN);
    const int32_t vertOffD = static_cast<int32_t>(image->clap.vertOffD);

    if (widthN <= 0 || widthD <= 0 || heightN <= 0 || heightD <= 0 || horizOffD <= 0 ||
        vertOffD <= 0) {
        return false;
    }

    const long double cleanWidth = static_cast<long double>(widthN) / widthD;
    const long double cleanHeight = static_cast<long double>(heightN) / heightD;
    const long double horizOff = static_cast<long double>(horizOffN) / horizOffD;
    const long double vertOff = static_cast<long double>(vertOffN) / vertOffD;
    if (!std::isfinite(cleanWidth) || !std::isfinite(cleanHeight) || !std::isfinite(horizOff) ||
        !std::isfinite(vertOff) || cleanWidth <= 0.0L || cleanHeight <= 0.0L) {
        return false;
    }

    const long double centreX = (static_cast<long double>(image->width) - 1.0L) / 2.0L + horizOff;
    const long double centreY = (static_cast<long double>(image->height) - 1.0L) / 2.0L + vertOff;
    const long double leftExact = centreX - (cleanWidth - 1.0L) / 2.0L;
    const long double topExact = centreY - (cleanHeight - 1.0L) / 2.0L;

    const long double left = std::floor(leftExact);
    const long double top = std::floor(topExact + 0.5L);
    const long double right = std::floor((cleanWidth - 1.0L + left) + 0.5L);
    const long double bottom = std::floor((cleanHeight - 1.0L + top) + 0.5L);

    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom) || left < 0.0L || top < 0.0L || right < left || bottom < top ||
        right >= static_cast<long double>(image->width) ||
        bottom >= static_cast<long double>(image->height) ||
        right > static_cast<long double>(std::numeric_limits<uint32_t>::max()) ||
        bottom > static_cast<long double>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }

    const uint32_t x = static_cast<uint32_t>(left);
    const uint32_t y = static_cast<uint32_t>(top);
    const uint32_t rightPixel = static_cast<uint32_t>(right);
    const uint32_t bottomPixel = static_cast<uint32_t>(bottom);
    rect->x = x;
    rect->y = y;
    rect->width = rightPixel - x + 1;
    rect->height = bottomPixel - y + 1;
    return rect->width > 0 && rect->height > 0;
}

bool cropRectFor(const avifImage* image, avifCropRect* rect)
{
    if (!image || !rect) {
        return false;
    }
    rect->x = 0;
    rect->y = 0;
    rect->width = image->width;
    rect->height = image->height;
    if (!(image->transformFlags & AVIF_TRANSFORM_CLAP)) {
        return true;
    }

    avifDiagnostics diagnostics{};
    if (avifCropRectFromCleanApertureBox(rect, &image->clap, image->width, image->height,
                                         &diagnostics) == AVIF_TRUE) {
        return true;
    }

    return fallbackCropRectForFractionalClap(image, rect);
}

QSize displaySizeFor(const avifImage* image)
{
    avifCropRect crop{};
    if (!cropRectFor(image, &crop) || crop.width == 0 || crop.height == 0) {
        return {};
    }
    QSize size(int(crop.width), int(crop.height));
    if ((image->transformFlags & AVIF_TRANSFORM_IROT) && (image->irot.angle & 1)) {
        size.transpose();
    }
    return size;
}

QImage applyTransforms(QImage image, const avifImage* metadata)
{
    if (!metadata || image.isNull()) {
        return {};
    }

    // MIAF item properties are applied in file-defined transformation order;
    // libavif exposes the normative AVIF set as clean aperture, rotation, then
    // mirror. The conversion above has already upsampled to RGB, satisfying the
    // odd-offset clean-aperture requirement for subsampled sources.
    if (metadata->transformFlags & AVIF_TRANSFORM_CLAP) {
        avifCropRect crop{};
        if (!cropRectFor(metadata, &crop)) {
            return {};
        }
        image = image.copy(int(crop.x), int(crop.y), int(crop.width), int(crop.height));
        if (image.isNull()) {
            return {};
        }
    }
    if ((metadata->transformFlags & AVIF_TRANSFORM_IROT) && metadata->irot.angle) {
        // AVIF irot is anti-clockwise. In image/widget coordinates Qt's
        // positive visual rotation is clockwise, hence the negative sign.
        image = image.transformed(QTransform().rotate(-90.0 * metadata->irot.angle),
                                  Qt::FastTransformation);
    }
    if (metadata->transformFlags & AVIF_TRANSFORM_IMIR) {
        if (metadata->imir.axis == 0) {
            image = image.mirrored(false, true);
        } else if (metadata->imir.axis == 1) {
            image = image.mirrored(true, false);
        } else {
            return {};
        }
    }
    return image;
}

// AVIF_STAGE4_ANIMATION_IMPLEMENTED_V1
constexpr uint32_t animationFrameLimit = 100000;

int animationDelayMs(const avifImageTiming& timing)
{
    const long double milliseconds = static_cast<long double>(timing.duration) * 1000.0L;

    if (!std::isfinite(static_cast<double>(milliseconds)) || milliseconds <= 0.0L) {
        return 100;
    }

    return int(std::clamp(milliseconds, 10.0L, 60000.0L));
}

int animationLoopCount(const avifDecoder* decoder)
{
    if (!decoder) {
        return 0;
    }

    if (decoder->repetitionCount == AVIF_REPETITION_COUNT_INFINITE) {
        return -1;
    }

    if (decoder->repetitionCount == AVIF_REPETITION_COUNT_UNKNOWN || decoder->repetitionCount < 0) {
        return 0;
    }

    return decoder->repetitionCount;
}

class AvifHandler final : public QImageIOHandler {
  public:
    AvifHandler() : decoder_(nullptr, avifDecoderDestroy) {}

    bool canRead() const override
    {
        const bool readable = !readComplete_ && (metadataReady_ || avifSignature(device()));
        if (readable && format().isEmpty()) {
            setFormat("avif");
        }
        return readable;
    }

    int imageCount() const override
    {
        if (!const_cast<AvifHandler*>(this)->readMetadata()) {
            return 0;
        }
        return sequencePresent_ ? decoder_->imageCount : 1;
    }

    int currentImageNumber() const override { return frameNumber_; }

    int nextImageDelay() const override { return sequencePresent_ ? delayMs_ : 0; }

    int loopCount() const override
    {
        if (!const_cast<AvifHandler*>(this)->readMetadata() || !sequencePresent_) {
            return 0;
        }
        return animationLoopCount(decoder_.get());
    }

    bool jumpToNextImage() override
    {
        if (!readMetadata()) {
            return false;
        }

        const int count = sequencePresent_ ? decoder_->imageCount : 1;
        const int next = frameNumber_ < 0 ? 0 : frameNumber_ + 1;

        if (next < 0 || next >= count) {
            return false;
        }

        pendingFrame_ = next;
        readComplete_ = false;
        return true;
    }

    bool jumpToImage(int number) override
    {
        if (!readMetadata()) {
            return false;
        }

        const int count = sequencePresent_ ? decoder_->imageCount : 1;
        if (number < 0 || number >= count) {
            return false;
        }

        pendingFrame_ = number;
        readComplete_ = false;
        return true;
    }

    bool supportsOption(ImageOption option) const override
    {
        return option == Size || option == ScaledSize || option == ImageFormat ||
               option == Description || option == Animation;
    }

    QVariant option(ImageOption option) const override
    {
        if (option == ScaledSize) {
            return scaledSize_;
        }
        if (option == ImageFormat) {
            return QImage::Format_RGBA8888;
        }
        if (!const_cast<AvifHandler*>(this)->readMetadata()) {
            return {};
        }
        if (option == Size) {
            return displaySize_;
        }
        if (option == Animation) {
            return sequencePresent_;
        }
        if (option == Description) {
            const char* codec = avifCodecName(AVIF_CODEC_CHOICE_DAV1D, AVIF_CODEC_FLAG_CAN_DECODE);
            const QString previewPath =
                sequencePresent_
                    ? QStringLiteral("AVIF image sequence (full native raster required per frame)")
                    : QStringLiteral("single AV1 coded image (full native raster required)");
            QString description =
                QStringLiteral("Backend: libavif %1; %2; libyuv %3\n\n"
                               "PreviewPath: %4\n\n"
                               "BackendPixelLimit: %5")
                    .arg(QString::fromLatin1(avifVersion()),
                         codec ? QString::fromLatin1(codec) : QStringLiteral("dav1d unavailable"))
                    .arg(avifLibYUVVersion())
                    .arg(previewPath)
                    .arg(quint64(AVIF_DEFAULT_IMAGE_SIZE_LIMIT));
            description += xmpDescriptionFields();
            return description;
        }
        return {};
    }

    void setOption(ImageOption option, const QVariant& value) override
    {
        if (option == ScaledSize) {
            scaledSize_ = value.toSize();
        }
    }

    bool read(QImage* output) override
    {
        if (!output || readComplete_ || !readMetadata() || cancelled()) {
            return false;
        }

        const int count = sequencePresent_ ? decoder_->imageCount : 1;
        if (pendingFrame_ < 0 || pendingFrame_ >= count) {
            readComplete_ = true;
            return false;
        }

        if (nativeSize_.isEmpty() || !Contract::allows(nativeSize_, effectivePixelLimit_)) {
            return limitFailure();
        }

        const QSize target =
            scaledSize_.isValid() && !scaledSize_.isEmpty() ? scaledSize_ : displaySize_;
        if (!Contract::allows(target, pixelBudget_)) {
            return limitFailure();
        }
        if (cancelled()) {
            return fail("AVIF decoding was cancelled.");
        }

        const int requestedFrame = pendingFrame_;

        avifResult result;
        if (decoder_->imageIndex + 1 == requestedFrame) {
            result = avifDecoderNextImage(decoder_.get());
        } else {
            result = avifDecoderNthImage(decoder_.get(), static_cast<uint32_t>(requestedFrame));
        }

        if (result != AVIF_RESULT_OK) {
            return fail(result);
        }

        if (decoder_->imageIndex != requestedFrame) {
            return fail("AVIF decoder returned an unexpected animation frame.");
        }
        if (cancelled()) {
            return fail("AVIF decoding was cancelled.");
        }

        const QSize decodedSize(int(decoder_->image->width), int(decoder_->image->height));
        if (!Contract::allows(decodedSize, effectivePixelLimit_)) {
            return limitFailure();
        }
        if (device()) {
            device()->setProperty(
                "_licasaNativeRasterPixels",
                QVariant::fromValue(quint64(decodedSize.width()) * quint64(decodedSize.height())));
        }

        QImage image(decodedSize, QImage::Format_RGBA8888);
        if (image.isNull()) {
            return fail("AVIF RGB output allocation failed.");
        }

        avifRGBImage rgb{};
        avifRGBImageSetDefaults(&rgb, decoder_->image);
        rgb.depth = 8;
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.chromaUpsampling = AVIF_CHROMA_UPSAMPLING_AUTOMATIC;
        rgb.avoidLibYUV = AVIF_FALSE;
        rgb.ignoreAlpha = AVIF_FALSE;
        rgb.alphaPremultiplied = AVIF_FALSE;
        rgb.maxThreads = threadCount_;
        rgb.pixels = image.bits();
        rgb.rowBytes = uint32_t(image.bytesPerLine());
        result = avifImageYUVToRGB(decoder_->image, &rgb);
        if (result != AVIF_RESULT_OK) {
            return fail(result);
        }
        if (cancelled()) {
            return fail("AVIF decoding was cancelled.");
        }

        const QColorSpace colorSpace = colorSpaceFor(decoder_->image);
        if (colorSpace.isValid()) {
            image.setColorSpace(colorSpace);
        }
        image = applyTransforms(std::move(image), decoder_->image);
        if (image.isNull()) {
            return fail("AVIF declares an invalid display transformation.");
        }
        if (!Contract::allows(image.size(), pixelBudget_)) {
            return limitFailure();
        }

        if (target != image.size()) {
            image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (image.isNull()) {
                return fail("AVIF scaled output allocation failed.");
            }
        }

        frameNumber_ = requestedFrame;
        pendingFrame_ = requestedFrame + 1;
        delayMs_ = sequencePresent_ ? animationDelayMs(decoder_->imageTiming) : 0;
        readComplete_ = !sequencePresent_ || pendingFrame_ >= count;

        *output = std::move(image);
        return true;
    }

  private:
    QString xmpDescriptionFields() const
    {
        // Normal raster decoding deliberately keeps ignoreXMP enabled. Motion
        // Photo probing uses a separate metadata-only parse and never advances
        // or decodes an image frame.
        QIODevice* source = device();
        if (!source || source->isSequential()) {
            return {};
        }

        const auto errorField = [](QStringView message) {
            return QStringLiteral("\n\nLicasaXmpError: %1").arg(message.toString());
        };
        const qint64 savedPosition = source->pos();
        if (savedPosition < 0 || !source->seek(0)) {
            return errorField(QStringLiteral("AVIF XMP metadata input could not be rewound"));
        }

        struct PositionGuard {
            QIODevice* device = nullptr;
            qint64 position = -1;
            ~PositionGuard()
            {
                if (device && position >= 0) {
                    device->seek(position);
                }
            }
        } positionGuard{source, savedPosition};

        if (cancelled()) {
            return errorField(QStringLiteral("AVIF XMP metadata probing was cancelled"));
        }

        DeviceIO metadataIo;
        if (!metadataIo.initialize(source, effectivePixelLimit_)) {
            return errorField(QStringLiteral("AVIF XMP metadata input is not readable"));
        }

        Decoder metadataDecoder(avifDecoderCreate(), avifDecoderDestroy);
        if (!metadataDecoder) {
            return errorField(QStringLiteral("AVIF XMP decoder allocation failed"));
        }
        metadataDecoder->codecChoice = AVIF_CODEC_CHOICE_DAV1D;
        metadataDecoder->maxThreads = 1;
        metadataDecoder->allowProgressive = AVIF_FALSE;
        metadataDecoder->allowIncremental = AVIF_FALSE;
        metadataDecoder->ignoreExif = AVIF_TRUE;
        metadataDecoder->ignoreXMP = AVIF_FALSE;
        metadataDecoder->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
        metadataDecoder->imageDimensionLimit = AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT;
        metadataDecoder->imageCountLimit = animationFrameLimit;
        metadataDecoder->strictFlags = AVIF_STRICT_ENABLED & ~AVIF_STRICT_CLAP_VALID;
        metadataDecoder->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA;
        avifDecoderSetIO(metadataDecoder.get(), metadataIo.io());

        const avifResult result = avifDecoderParse(metadataDecoder.get());
        if (result != AVIF_RESULT_OK || !metadataDecoder->image) {
            return errorField(QStringLiteral("AVIF XMP metadata parse failed"));
        }
        if (cancelled()) {
            return errorField(QStringLiteral("AVIF XMP metadata probing was cancelled"));
        }

        const avifRWData& xmp = metadataDecoder->image->xmp;
        if (xmp.size == 0) {
            return {};
        }
        if (!xmp.data || xmp.size > size_t(maximumMotionXmpBytes) ||
            xmp.size > size_t(std::numeric_limits<qsizetype>::max())) {
            return errorField(QStringLiteral("AVIF XMP metadata exceeds the 1 MiB probe limit"));
        }

        const QByteArray packet(reinterpret_cast<const char*>(xmp.data), qsizetype(xmp.size));
        return QStringLiteral("\n\nLicasaXmpBase64: %1")
            .arg(QString::fromLatin1(packet.toBase64()));
    }

    bool cancelled() const { return Contract::cancelled(device()); }

    bool fail(const QString& message)
    {
        readComplete_ = true;
        if (device()) {
            device()->setProperty(Contract::errorProperty, message);
        }
        return false;
    }

    bool fail(avifResult result)
    {
        QString message =
            QStringLiteral("AVIF: %1").arg(QString::fromLatin1(avifResultToString(result)));
        if (decoder_ && decoder_->diag.error[0] != '\0') {
            message += QStringLiteral(" (%1)").arg(QString::fromUtf8(decoder_->diag.error));
        }
        return fail(message);
    }

    bool limitFailure()
    {
        return fail(
            QStringLiteral(
                "AVIF cannot decode this raster within the selected image limit. "
                "The AVIF backend is additionally capped at %1 pixels and %2 per dimension.")
                .arg(quint64(AVIF_DEFAULT_IMAGE_SIZE_LIMIT))
                .arg(AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT));
    }

    bool readMetadata()
    {
        if (metadataAttempted_) {
            return metadataReady_;
        }
        metadataAttempted_ = true;
        if (!canRead()) {
            return fail("The file is not a supported AVIF image.");
        }
        if (!device() || device()->isSequential()) {
            return fail("AVIF requires a seekable local image.");
        }

        pixelBudget_ = Contract::pixelBudget(device());
        effectivePixelLimit_ =
            std::min<quint64>(pixelBudget_, quint64(AVIF_DEFAULT_IMAGE_SIZE_LIMIT));
        threadCount_ = std::max(1, QThread::idealThreadCount());
        if (cancelled()) {
            return fail("AVIF decoding was cancelled.");
        }
        if (!io_.initialize(device(), effectivePixelLimit_)) {
            return fail("AVIF could not initialize bounded image input.");
        }

        decoder_.reset(avifDecoderCreate());
        if (!decoder_) {
            return fail("AVIF decoder allocation failed.");
        }
        decoder_->codecChoice = AVIF_CODEC_CHOICE_DAV1D;
        decoder_->maxThreads = threadCount_;
        decoder_->allowProgressive = AVIF_FALSE;
        decoder_->allowIncremental = AVIF_FALSE;
        decoder_->ignoreExif = AVIF_TRUE;
        decoder_->ignoreXMP = AVIF_TRUE;
        decoder_->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
        decoder_->imageDimensionLimit = AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT;
        // Animated AVIF is decoded one frame at a time. Keep an explicit
        // application ceiling so hostile files cannot force metadata parsing of
        // arbitrarily large sequence tables. No all-frame raster cache exists.
        decoder_->imageCountLimit = animationFrameLimit;
        // Keep libavif's structural/pixi/alpha strict checks, but validate clap
        // ourselves. AVIF_STRICT_CLAP_VALID requires an exact integer source
        // crop and rejects valid half-pixel clean apertures before Licasa can
        // apply them after YUV-to-RGB upsampling. cropRectFor() performs the
        // bounded clean-aperture validation used for both metadata and decode.
        decoder_->strictFlags = AVIF_STRICT_ENABLED & ~AVIF_STRICT_CLAP_VALID;
        decoder_->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA;
        avifDecoderSetIO(decoder_.get(), io_.io());

        const avifResult result = avifDecoderParse(decoder_.get());
        if (result != AVIF_RESULT_OK) {
            return fail(result);
        }
        if (cancelled()) {
            return fail("AVIF decoding was cancelled.");
        }
        if (!decoder_->image || decoder_->image->width == 0 || decoder_->image->height == 0) {
            return fail("AVIF declares invalid image dimensions.");
        }
        if (decoder_->image->icc.size > metadataBudget) {
            return fail("AVIF ICC metadata exceeds Licasa's metadata budget.");
        }

        nativeSize_ = QSize(int(decoder_->image->width), int(decoder_->image->height));
        displaySize_ = displaySizeFor(decoder_->image);
        if (displaySize_.isEmpty()) {
            return fail("AVIF declares an invalid clean-aperture or rotation transform.");
        }

        // A progressive still with allowProgressive=false remains one final
        // still even when libavif reports multiple progressive layers. Only a
        // true image sequence is exposed through Qt's animation contract.
        sequencePresent_ = decoder_->imageSequenceTrackPresent == AVIF_TRUE ||
                           (decoder_->imageCount > 1 &&
                            decoder_->progressiveState == AVIF_PROGRESSIVE_STATE_UNAVAILABLE);

        if (sequencePresent_ &&
            (decoder_->imageCount <= 1 || decoder_->imageCount > int(animationFrameLimit))) {
            return fail("AVIF animation declares an invalid frame count.");
        }

        pendingFrame_ = 0;
        frameNumber_ = -1;
        delayMs_ = 100;
        metadataReady_ = true;
        return true;
    }

    DeviceIO io_;
    Decoder decoder_;
    QSize nativeSize_;
    QSize displaySize_;
    QSize scaledSize_;
    quint64 pixelBudget_ = 0;
    quint64 effectivePixelLimit_ = 0;
    int threadCount_ = 1;
    int pendingFrame_ = 0;
    int frameNumber_ = -1;
    int delayMs_ = 100;
    bool metadataAttempted_ = false;
    bool metadataReady_ = false;
    bool sequencePresent_ = false;
    bool readComplete_ = false;
};
} // namespace

extern "C" Q_DECL_EXPORT QImageIOHandler* licasaCreateImageHandler() { return new AvifHandler; }
