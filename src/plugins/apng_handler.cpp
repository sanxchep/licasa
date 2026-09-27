#include "imaging/image_decode_contract.h"

#include <QByteArray>
#include <QColorSpace>
#include <QIODevice>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVariant>
#include <QVector>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <limits>

namespace {
namespace Contract = Licasa::ImageDecodeContract;

constexpr char pngSignatureBytes[] = "\x89PNG\r\n\x1a\n";
constexpr qint64 pngSignatureSize = 8;
constexpr quint32 maximumAnimationFrames = 100000;
constexpr int maximumProbeChunks = 4096;
constexpr int maximumParsedChunks = 262144;
constexpr qint64 crcBufferSize = 64 * 1024;

enum class DisposeOp : quint8 { None = 0, Background = 1, Previous = 2 };

enum class BlendOp : quint8 { Source = 0, Over = 1 };

struct SourceChunk {
    qint64 chunkOffset = -1;
    quint32 dataLength = 0;
};

struct FrameDataChunk {
    enum class Kind { OriginalIdat, FdatPayload } kind = Kind::OriginalIdat;
    // For IDAT this is the complete source chunk. For fdAT this is the fdAT
    // chunk start; payload CRC validation and IDAT-CRC conversion are deferred
    // until that frame is actually requested.
    qint64 sourceOffset = -1;
    quint32 sourceLength = 0;
    quint32 sequence = 0;
};

struct FrameControl {
    QSize size;
    QPoint offset;
    int delayMs = 0;
    DisposeOp dispose = DisposeOp::None;
    BlendOp blend = BlendOp::Source;
    QVector<FrameDataChunk> data;
};

struct VirtualSegment {
    qint64 start = 0;
    qint64 length = 0;
    QByteArray memory;
    qint64 sourceOffset = -1;
};

const std::array<quint32, 256>& crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> values{};
        for (quint32 n = 0; n < values.size(); ++n) {
            quint32 c = n;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? 0xedb88320u ^ (c >> 1u) : c >> 1u;
            }
            values[n] = c;
        }
        return values;
    }();
    return table;
}

quint32 crcUpdate(quint32 crc, const char* data, qint64 length)
{
    const auto& table = crcTable();
    for (qint64 i = 0; i < length; ++i) {
        const auto byte = static_cast<quint8>(data[i]);
        crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8u);
    }
    return crc;
}

quint32 crcFinish(quint32 crc) { return crc ^ 0xffffffffu; }

QByteArray makeChunk(const QByteArray& type, const QByteArray& payload)
{
    if (type.size() != 4 || payload.size() > std::numeric_limits<quint32>::max()) {
        return {};
    }

    QByteArray result;
    result.resize(12 + payload.size());
    qToBigEndian<quint32>(quint32(payload.size()), reinterpret_cast<uchar*>(result.data()));
    std::copy(type.cbegin(), type.cend(), result.begin() + 4);
    if (!payload.isEmpty()) {
        std::copy(payload.cbegin(), payload.cend(), result.begin() + 8);
    }

    quint32 crc = 0xffffffffu;
    crc = crcUpdate(crc, type.constData(), type.size());
    crc = crcUpdate(crc, payload.constData(), payload.size());
    qToBigEndian<quint32>(crcFinish(crc),
                          reinterpret_cast<uchar*>(result.data() + 8 + payload.size()));
    return result;
}

QByteArray makeIdatHeader(quint32 payloadLength)
{
    QByteArray result(8, Qt::Uninitialized);
    qToBigEndian<quint32>(payloadLength, reinterpret_cast<uchar*>(result.data()));
    std::copy_n("IDAT", 4, result.data() + 4);
    return result;
}

QByteArray makeCrcBytes(quint32 crc)
{
    QByteArray result(4, Qt::Uninitialized);
    qToBigEndian<quint32>(crc, reinterpret_cast<uchar*>(result.data()));
    return result;
}

bool isPngSignature(const QByteArray& signature)
{
    return signature.size() == pngSignatureSize &&
           signature == QByteArray::fromRawData(pngSignatureBytes, pngSignatureSize);
}

bool checkedChunkEnd(qint64 offset, quint32 length, qint64 total, qint64* next)
{
    if (!next || offset < 0 || total < 0 || offset > total - 12) {
        return false;
    }
    const quint64 end = quint64(offset) + 12u + quint64(length);
    if (end > quint64(total) || end > quint64(std::numeric_limits<qint64>::max())) {
        return false;
    }
    *next = qint64(end);
    return true;
}

QByteArray readAt(QIODevice* device, qint64 offset, qint64 length)
{
    if (!device || offset < 0 || length < 0 || length > std::numeric_limits<int>::max() ||
        !device->seek(offset)) {
        return {};
    }
    const QByteArray result = device->read(length);
    return result.size() == length ? result : QByteArray{};
}

bool hasApngControl(QIODevice* device)
{
    if (!device || !device->isReadable() || device->isSequential()) {
        return false;
    }

    const qint64 original = device->pos();
    const qint64 total = device->size();
    auto restore = [device, original] {
        if (original >= 0) {
            device->seek(original);
        }
    };

    if (total < pngSignatureSize + 12 || !isPngSignature(readAt(device, 0, pngSignatureSize))) {
        restore();
        return false;
    }

    qint64 offset = pngSignatureSize;
    for (int chunks = 0; chunks < maximumProbeChunks; ++chunks) {
        const QByteArray header = readAt(device, offset, 8);
        if (header.size() != 8) {
            restore();
            return false;
        }
        const quint32 length =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.constData()));
        const QByteArray type = header.mid(4, 4);
        qint64 next = 0;
        if (!checkedChunkEnd(offset, length, total, &next)) {
            restore();
            return false;
        }
        if (type == "acTL") {
            restore();
            return length == 8;
        }
        if (type == "IDAT" || type == "IEND") {
            restore();
            return false;
        }
        offset = next;
    }

    restore();
    return false;
}

bool decodeRelevantGlobalChunk(const QByteArray& type)
{
    return type == "PLTE" || type == "tRNS" || type == "cHRM" || type == "gAMA" || type == "iCCP" ||
           type == "sBIT" || type == "sRGB" || type == "cICP" || type == "mDCv" || type == "cLLi";
}

bool knownCriticalChunk(const QByteArray& type)
{
    return type == "IHDR" || type == "PLTE" || type == "IDAT" || type == "IEND";
}

bool isCriticalChunk(const QByteArray& type)
{
    return type.size() == 4 && type[0] >= 'A' && type[0] <= 'Z';
}

int apngDelayMilliseconds(quint16 numerator, quint16 denominator)
{
    const quint32 divisor = denominator == 0 ? 100u : denominator;
    const quint64 milliseconds = (quint64(numerator) * 1000u + divisor / 2u) / divisor;
    return int(std::min<quint64>(milliseconds, quint64(std::numeric_limits<int>::max())));
}

QImage::Format compositionFormat(QImage::Format source)
{
    switch (source) {
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
        return QImage::Format_RGBA64_Premultiplied;
    default:
        return QImage::Format_ARGB32_Premultiplied;
    }
}

class FramePngDevice final : public QIODevice {
  public:
    FramePngDevice(QIODevice* source, QVector<VirtualSegment> segments)
        : source_(source), segments_(std::move(segments))
    {
        for (const auto& segment : segments_) {
            if (segment.length < 0 ||
                totalSize_ > std::numeric_limits<qint64>::max() - segment.length) {
                totalSize_ = -1;
                break;
            }
            totalSize_ += segment.length;
        }
        open(QIODevice::ReadOnly);
    }

    qint64 size() const override { return std::max<qint64>(0, totalSize_); }

    bool seek(qint64 position) override
    {
        if (position < 0 || totalSize_ < 0 || position > totalSize_) {
            return false;
        }
        return QIODevice::seek(position);
    }

  protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        if (!data || maximum <= 0 || totalSize_ < 0) {
            return 0;
        }
        const qint64 position = pos();
        if (position < 0 || position >= totalSize_) {
            return 0;
        }

        qint64 written = 0;
        qint64 cursor = position;
        while (written < maximum && cursor < totalSize_) {
            const auto it = std::upper_bound(
                segments_.cbegin(), segments_.cend(), cursor,
                [](qint64 value, const VirtualSegment& segment) { return value < segment.start; });
            if (it == segments_.cbegin()) {
                return written > 0 ? written : -1;
            }
            const auto& segment = *std::prev(it);
            if (cursor < segment.start || cursor >= segment.start + segment.length) {
                return written > 0 ? written : -1;
            }

            const qint64 within = cursor - segment.start;
            const qint64 amount =
                std::min({maximum - written, segment.length - within, totalSize_ - cursor});
            if (amount <= 0) {
                break;
            }

            if (!segment.memory.isEmpty()) {
                std::copy_n(segment.memory.constData() + within, amount, data + written);
            } else {
                if (!source_ || segment.sourceOffset < 0 ||
                    !source_->seek(segment.sourceOffset + within)) {
                    return written > 0 ? written : -1;
                }
                const qint64 count = source_->read(data + written, amount);
                if (count != amount) {
                    return written > 0 ? written : -1;
                }
            }
            written += amount;
            cursor += amount;
        }
        return written;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

  private:
    QIODevice* source_ = nullptr;
    QVector<VirtualSegment> segments_;
    qint64 totalSize_ = 0;
};

class ApngHandler final : public QImageIOHandler {
  public:
    bool canRead() const override
    {
        const bool readable = !readComplete_ && (metadataReady_ || hasApngControl(device()));
        if (readable && format().isEmpty()) {
            setFormat("apng");
        }
        return readable;
    }

    int imageCount() const override
    {
        if (!const_cast<ApngHandler*>(this)->readMetadata()) {
            return 0;
        }
        return frames_.size();
    }

    int currentImageNumber() const override { return frameNumber_; }

    int nextImageDelay() const override { return delayMs_; }

    int loopCount() const override
    {
        if (!const_cast<ApngHandler*>(this)->readMetadata()) {
            return 0;
        }
        if (playCount_ == 0) {
            return -1;
        }
        return int(std::min<quint32>(playCount_ - 1u, quint32(std::numeric_limits<int>::max())));
    }

    bool jumpToNextImage() override
    {
        if (!readMetadata()) {
            return false;
        }
        const int next = frameNumber_ < 0 ? 0 : frameNumber_ + 1;
        if (next < 0 || next >= frames_.size()) {
            return false;
        }
        pendingFrame_ = next;
        readComplete_ = false;
        return true;
    }

    bool jumpToImage(int number) override
    {
        if (!readMetadata() || number < 0 || number >= frames_.size()) {
            return false;
        }
        pendingFrame_ = number;
        readComplete_ = false;
        return true;
    }

    bool supportsOption(ImageOption option) const override
    {
        // Deliberately do not claim ScaledSize. APNG composition requires the
        // admitted native canvas; QImageReader may post-scale an already-safe
        // canvas, but an oversized APNG must not pass the bounded-preview gate.
        return option == Size || option == Description || option == Animation;
    }

    QVariant option(ImageOption option) const override
    {
        if (!const_cast<ApngHandler*>(this)->readMetadata()) {
            return {};
        }
        if (option == Size) {
            return canvasSize_;
        }
        if (option == Animation) {
            return true;
        }
        if (option == Description) {
            return QStringLiteral(
                       "Backend: Qt PNG frame decoder + Licasa APNG compositor\n\n"
                       "Frames: %1\n\n"
                       "PreviewPath: full admitted APNG canvas; no native requested-size shortcut")
                .arg(frames_.size());
        }
        return {};
    }

    bool read(QImage* output) override
    {
        // Native-raster telemetry describes the current decode attempt, not
        // the most recently successful animation frame. Clear it before a new
        // read so a failure that occurs before raster decode cannot expose
        // stale pixel-count telemetry from the previous frame.
        if (device()) {
            device()->setProperty("_licasaNativeRasterPixels", QVariant{});
        }

        if (!output || readComplete_ || !readMetadata() || cancelled()) {
            return false;
        }
        if (pendingFrame_ < 0 || pendingFrame_ >= frames_.size()) {
            readComplete_ = true;
            return false;
        }
        if (!Contract::allows(canvasSize_, pixelBudget_)) {
            return limitFailure();
        }

        if (pendingFrame_ != composedUntil_ + 1) {
            resetComposition();
        }

        for (int index = composedUntil_ + 1; index <= pendingFrame_; ++index) {
            if (cancelled()) {
                return fail(QStringLiteral("APNG decoding was cancelled."));
            }
            QImage frame = decodeFrame(index);
            if (frame.isNull()) {
                return false;
            }
            if (!composeFrame(index, frame)) {
                return false;
            }
        }

        frameNumber_ = pendingFrame_;
        delayMs_ = frames_.at(frameNumber_).delayMs;
        pendingFrame_ = frameNumber_ + 1;
        readComplete_ = pendingFrame_ >= frames_.size();
        *output = canvas_;
        return !output->isNull();
    }

  private:
    bool cancelled() const { return Contract::cancelled(device()); }

    bool fail(const QString& message)
    {
        readComplete_ = true;
        if (device()) {
            device()->setProperty(Contract::errorProperty, message);
        }
        return false;
    }

    bool limitFailure()
    {
        return fail(
            QStringLiteral("APNG cannot decode this canvas within the selected image limit. "
                           "APNG composition requires the full admitted canvas."));
    }

    bool verifyChunkCrc(qint64 chunkOffset, quint32 length, const QByteArray& type)
    {
        if (!device() || type.size() != 4) {
            return false;
        }
        quint32 crc = 0xffffffffu;
        crc = crcUpdate(crc, type.constData(), type.size());

        qint64 remaining = length;
        qint64 offset = chunkOffset + 8;
        QByteArray buffer(int(std::min<qint64>(crcBufferSize, std::max<qint64>(1, remaining))),
                          Qt::Uninitialized);
        while (remaining > 0) {
            if (cancelled()) {
                return false;
            }
            const qint64 amount = std::min<qint64>(remaining, buffer.size());
            if (!device()->seek(offset)) {
                return false;
            }
            const qint64 count = device()->read(buffer.data(), amount);
            if (count != amount) {
                return false;
            }
            crc = crcUpdate(crc, buffer.constData(), count);
            remaining -= count;
            offset += count;
        }

        const QByteArray expected = readAt(device(), chunkOffset + 8 + length, 4);
        return expected.size() == 4 &&
               qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(expected.constData())) ==
                   crcFinish(crc);
    }

    bool validateFdatAndComputeIdatCrc(qint64 chunkOffset, quint32 length, quint32* sequence,
                                       quint32* idatCrc)
    {
        if (!device() || !sequence || !idatCrc || length < 4) {
            return false;
        }
        const QByteArray sequenceBytes = readAt(device(), chunkOffset + 8, 4);
        if (sequenceBytes.size() != 4) {
            return false;
        }
        *sequence =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(sequenceBytes.constData()));

        quint32 sourceCrc = 0xffffffffu;
        sourceCrc = crcUpdate(sourceCrc, "fdAT", 4);
        sourceCrc = crcUpdate(sourceCrc, sequenceBytes.constData(), sequenceBytes.size());
        quint32 convertedCrc = 0xffffffffu;
        convertedCrc = crcUpdate(convertedCrc, "IDAT", 4);

        qint64 remaining = qint64(length) - 4;
        qint64 offset = chunkOffset + 12;
        QByteArray buffer(int(std::min<qint64>(crcBufferSize, std::max<qint64>(1, remaining))),
                          Qt::Uninitialized);
        while (remaining > 0) {
            if (cancelled()) {
                return false;
            }
            const qint64 amount = std::min<qint64>(remaining, buffer.size());
            if (!device()->seek(offset)) {
                return false;
            }
            const qint64 count = device()->read(buffer.data(), amount);
            if (count != amount) {
                return false;
            }
            sourceCrc = crcUpdate(sourceCrc, buffer.constData(), count);
            convertedCrc = crcUpdate(convertedCrc, buffer.constData(), count);
            remaining -= count;
            offset += count;
        }

        const QByteArray expected = readAt(device(), chunkOffset + 8 + length, 4);
        if (expected.size() != 4 || qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(
                                        expected.constData())) != crcFinish(sourceCrc)) {
            return false;
        }
        *idatCrc = crcFinish(convertedCrc);
        return true;
    }

    bool readMetadata()
    {
        if (metadataAttempted_) {
            return metadataReady_;
        }
        metadataAttempted_ = true;

        if (!device() || !device()->isReadable() || device()->isSequential()) {
            return fail(QStringLiteral("APNG requires a seekable local image."));
        }
        if (!hasApngControl(device())) {
            return fail(QStringLiteral("The file is not a supported APNG animation."));
        }
        if (cancelled()) {
            return fail(QStringLiteral("APNG decoding was cancelled."));
        }

        pixelBudget_ = Contract::pixelBudget(device());
        sourceSize_ = device()->size();
        if (sourceSize_ < pngSignatureSize + 25 ||
            !isPngSignature(readAt(device(), 0, pngSignatureSize))) {
            return fail(QStringLiteral("APNG has an invalid PNG signature."));
        }

        qint64 offset = pngSignatureSize;
        const QByteArray firstHeader = readAt(device(), offset, 8);
        if (firstHeader.size() != 8) {
            return fail(QStringLiteral("APNG is missing IHDR metadata."));
        }
        const quint32 ihdrLength =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(firstHeader.constData()));
        if (ihdrLength != 13 || firstHeader.mid(4, 4) != "IHDR" ||
            !verifyChunkCrc(offset, ihdrLength, QByteArrayLiteral("IHDR"))) {
            return fail(QStringLiteral("APNG has an invalid IHDR chunk."));
        }
        ihdr_ = readAt(device(), offset + 8, 13);
        if (ihdr_.size() != 13) {
            return fail(QStringLiteral("APNG IHDR metadata is truncated."));
        }

        const quint32 canvasWidth =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(ihdr_.constData()));
        const quint32 canvasHeight =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(ihdr_.constData() + 4));
        if (canvasWidth == 0 || canvasHeight == 0 ||
            canvasWidth > quint32(std::numeric_limits<int>::max()) ||
            canvasHeight > quint32(std::numeric_limits<int>::max())) {
            return fail(QStringLiteral("APNG declares invalid canvas dimensions."));
        }
        canvasSize_ = QSize(int(canvasWidth), int(canvasHeight));

        qint64 next = 0;
        if (!checkedChunkEnd(offset, ihdrLength, sourceSize_, &next)) {
            return fail(QStringLiteral("APNG IHDR bounds are invalid."));
        }
        offset = next;

        bool seenActl = false;
        bool seenImageData = false;
        bool defaultImageIsFrame = false;
        bool ended = false;
        quint32 declaredFrames = 0;
        quint32 expectedSequence = 0;
        int currentFrame = -1;

        for (int chunkNumber = 1; chunkNumber < maximumParsedChunks; ++chunkNumber) {
            if (cancelled()) {
                return fail(QStringLiteral("APNG decoding was cancelled."));
            }
            const QByteArray header = readAt(device(), offset, 8);
            if (header.size() != 8) {
                return fail(QStringLiteral("APNG chunk header is truncated."));
            }
            const quint32 length =
                qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.constData()));
            const QByteArray type = header.mid(4, 4);
            if (!checkedChunkEnd(offset, length, sourceSize_, &next)) {
                return fail(QStringLiteral("APNG chunk exceeds the file bounds."));
            }

            if (isCriticalChunk(type) && !knownCriticalChunk(type)) {
                return fail(QStringLiteral("APNG contains an unsupported critical PNG chunk."));
            }

            if (type == "acTL") {
                if (seenActl || seenImageData || length != 8 ||
                    !verifyChunkCrc(offset, length, type)) {
                    return fail(QStringLiteral("APNG has invalid animation-control metadata."));
                }
                const QByteArray data = readAt(device(), offset + 8, 8);
                if (data.size() != 8) {
                    return fail(QStringLiteral("APNG animation-control metadata is truncated."));
                }
                declaredFrames =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData()));
                playCount_ =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + 4));
                if (declaredFrames == 0 || declaredFrames > maximumAnimationFrames) {
                    return fail(
                        QStringLiteral("APNG declares an unsupported number of animation frames."));
                }
                seenActl = true;
            } else if (type == "fcTL") {
                if (!seenActl || length != 26 || !verifyChunkCrc(offset, length, type)) {
                    return fail(QStringLiteral("APNG has invalid frame-control metadata."));
                }
                if (currentFrame >= 0 && frames_.at(currentFrame).data.isEmpty()) {
                    return fail(QStringLiteral("APNG frame is missing image data."));
                }
                const QByteArray data = readAt(device(), offset + 8, 26);
                if (data.size() != 26) {
                    return fail(QStringLiteral("APNG frame-control metadata is truncated."));
                }
                const quint32 sequence =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData()));
                if (sequence != expectedSequence++) {
                    return fail(QStringLiteral("APNG frame sequence numbers are invalid."));
                }

                const quint32 width =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + 4));
                const quint32 height =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + 8));
                const quint32 x =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + 12));
                const quint32 y =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + 16));
                const quint16 delayNumerator =
                    qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + 20));
                const quint16 delayDenominator =
                    qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + 22));
                const quint8 dispose = quint8(data[24]);
                const quint8 blend = quint8(data[25]);

                if (width == 0 || height == 0 || width > canvasWidth || height > canvasHeight ||
                    x > canvasWidth - width || y > canvasHeight - height ||
                    width > quint32(std::numeric_limits<int>::max()) ||
                    height > quint32(std::numeric_limits<int>::max()) ||
                    x > quint32(std::numeric_limits<int>::max()) ||
                    y > quint32(std::numeric_limits<int>::max()) ||
                    dispose > quint8(DisposeOp::Previous) || blend > quint8(BlendOp::Over)) {
                    return fail(QStringLiteral("APNG frame-control values are invalid."));
                }

                FrameControl frame;
                frame.size = QSize(int(width), int(height));
                frame.offset = QPoint(int(x), int(y));
                frame.delayMs = apngDelayMilliseconds(delayNumerator, delayDenominator);
                frame.dispose = DisposeOp(dispose);
                frame.blend = BlendOp(blend);

                if (!seenImageData && frames_.isEmpty()) {
                    if (frame.size != canvasSize_ || !frame.offset.isNull()) {
                        return fail(QStringLiteral(
                            "APNG default animation frame must cover the full canvas."));
                    }
                    defaultImageIsFrame = true;
                    if (frame.dispose == DisposeOp::Previous) {
                        frame.dispose = DisposeOp::Background;
                    }
                } else if (!seenImageData) {
                    return fail(
                        QStringLiteral("APNG has multiple frame controls before image data."));
                }

                frames_.append(std::move(frame));
                currentFrame = frames_.size() - 1;
                if (frames_.size() > int(maximumAnimationFrames)) {
                    return fail(QStringLiteral("APNG contains too many animation frames."));
                }
            } else if (type == "IDAT") {
                if (!seenActl) {
                    return fail(QStringLiteral("PNG animation data appears before acTL."));
                }
                if (currentFrame >= 0) {
                    if (!defaultImageIsFrame || currentFrame != 0) {
                        return fail(QStringLiteral(
                            "APNG IDAT data appears in a non-default animation frame."));
                    }
                    frames_[0].data.append({FrameDataChunk::Kind::OriginalIdat, offset, length, 0});
                }
                seenImageData = true;
            } else if (type == "fdAT") {
                if (!seenActl || !seenImageData || currentFrame < 0 || length < 4) {
                    return fail(
                        QStringLiteral("APNG frame data appears without a valid frame control."));
                }
                const QByteArray sequenceBytes = readAt(device(), offset + 8, 4);
                if (sequenceBytes.size() != 4) {
                    return fail(QStringLiteral("APNG frame data is truncated."));
                }
                const quint32 sequence = qFromBigEndian<quint32>(
                    reinterpret_cast<const uchar*>(sequenceBytes.constData()));
                if (sequence != expectedSequence++) {
                    return fail(QStringLiteral("APNG frame sequence numbers are invalid."));
                }
                frames_[currentFrame].data.append(
                    {FrameDataChunk::Kind::FdatPayload, offset, length, sequence});
            } else if (type == "IEND") {
                if (length != 0 || !verifyChunkCrc(offset, length, type)) {
                    return fail(QStringLiteral("APNG has an invalid IEND chunk."));
                }
                ended = true;
                offset = next;
                break;
            } else if (!seenImageData && decodeRelevantGlobalChunk(type)) {
                globalChunks_.append({offset, length});
            }

            offset = next;
        }

        if (!ended) {
            return fail(QStringLiteral("APNG is missing IEND or contains too many chunks."));
        }
        if (!seenActl || declaredFrames == 0 || frames_.isEmpty()) {
            return fail(QStringLiteral("APNG does not contain animation frames."));
        }
        if (frames_.size() != int(declaredFrames)) {
            return fail(QStringLiteral("APNG frame count does not match acTL."));
        }
        for (const auto& frame : frames_) {
            if (frame.data.isEmpty()) {
                return fail(QStringLiteral("APNG frame is missing compressed image data."));
            }
        }

        metadataReady_ = true;
        readComplete_ = false;
        if (device()) {
            device()->setProperty("_licasaApngHandler", true);
        }
        return true;
    }

    QVector<VirtualSegment> frameSegments(int frameIndex)
    {
        QVector<VirtualSegment> segments;
        qint64 cursor = 0;
        auto memory = [&segments, &cursor](QByteArray bytes) {
            if (bytes.isEmpty()) {
                return false;
            }
            const qint64 length = bytes.size();
            segments.append({cursor, length, std::move(bytes), -1});
            cursor += length;
            return true;
        };
        auto source = [&segments, &cursor](qint64 offset, qint64 length) {
            if (offset < 0 || length < 0 || cursor > std::numeric_limits<qint64>::max() - length) {
                return false;
            }
            if (length == 0) {
                return true;
            }
            segments.append({cursor, length, {}, offset});
            cursor += length;
            return true;
        };

        if (!memory(QByteArray::fromRawData(pngSignatureBytes, pngSignatureSize))) {
            return {};
        }

        QByteArray frameIhdr = ihdr_;
        const auto& frame = frames_.at(frameIndex);
        qToBigEndian<quint32>(quint32(frame.size.width()),
                              reinterpret_cast<uchar*>(frameIhdr.data()));
        qToBigEndian<quint32>(quint32(frame.size.height()),
                              reinterpret_cast<uchar*>(frameIhdr.data() + 4));
        if (!memory(makeChunk(QByteArrayLiteral("IHDR"), frameIhdr))) {
            return {};
        }

        for (const auto& chunk : globalChunks_) {
            const qint64 length = qint64(chunk.dataLength) + 12;
            if (!source(chunk.chunkOffset, length)) {
                return {};
            }
        }

        for (const auto& chunk : frame.data) {
            if (chunk.kind == FrameDataChunk::Kind::OriginalIdat) {
                if (!source(chunk.sourceOffset, qint64(chunk.sourceLength) + 12)) {
                    return {};
                }
            } else {
                quint32 sequence = 0;
                quint32 generatedCrc = 0;
                if (!validateFdatAndComputeIdatCrc(chunk.sourceOffset, chunk.sourceLength,
                                                   &sequence, &generatedCrc) ||
                    sequence != chunk.sequence) {
                    fail(QStringLiteral(
                        "APNG frame data has an invalid CRC or changed after metadata parsing."));
                    return {};
                }
                const quint32 payloadLength = chunk.sourceLength - 4;
                if (!memory(makeIdatHeader(payloadLength)) ||
                    !source(chunk.sourceOffset + 12, payloadLength) ||
                    !memory(makeCrcBytes(generatedCrc))) {
                    return {};
                }
            }
        }

        if (!memory(makeChunk(QByteArrayLiteral("IEND"), {}))) {
            return {};
        }
        return segments;
    }

    QImage decodeFrame(int frameIndex)
    {
        const auto& control = frames_.at(frameIndex);
        if (!Contract::allows(control.size, pixelBudget_)) {
            limitFailure();
            return {};
        }

        QVector<VirtualSegment> segments = frameSegments(frameIndex);
        if (segments.isEmpty()) {
            // frameSegments() may already have recorded a precise validation
            // failure (for example a lazily checked fdAT CRC mismatch). Do not
            // replace that diagnostic with a generic stream-construction error.
            const QString existingError =
                device() ? device()->property(Contract::errorProperty).toString() : QString{};
            if (existingError.isEmpty()) {
                fail(QStringLiteral("APNG could not construct a bounded frame stream."));
            }
            return {};
        }
        FramePngDevice frameDevice(device(), std::move(segments));
        frameDevice.setProperty(Contract::pixelBudgetProperty, QVariant::fromValue(pixelBudget_));
        if (device()) {
            frameDevice.setProperty(Contract::cancellationProperty,
                                    device()->property(Contract::cancellationProperty));
        }

        QImageReader reader(&frameDevice);
        if (!reader.canRead()) {
            fail(QStringLiteral("APNG frame is not a readable PNG image: %1")
                     .arg(reader.errorString()));
            return {};
        }
        const QSize reported = reader.size();
        if (reported != control.size || !Contract::allows(reported, pixelBudget_)) {
            fail(QStringLiteral("APNG frame dimensions do not match frame-control metadata."));
            return {};
        }
        if (cancelled()) {
            fail(QStringLiteral("APNG decoding was cancelled."));
            return {};
        }

        QImage frame = reader.read();
        if (frame.isNull()) {
            fail(QStringLiteral("APNG PNG-frame decode failed: %1").arg(reader.errorString()));
            return {};
        }
        if (frame.size() != control.size || !Contract::allows(frame.size(), pixelBudget_)) {
            fail(QStringLiteral("APNG decoded an unexpected frame raster."));
            return {};
        }
        if (device()) {
            device()->setProperty(
                "_licasaNativeRasterPixels",
                QVariant::fromValue(quint64(frame.width()) * quint64(frame.height())));
        }
        return frame;
    }

    void clearRect(const QRect& rect)
    {
        if (canvas_.isNull() || rect.isEmpty()) {
            return;
        }
        QPainter painter(&canvas_);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect, Qt::transparent);
    }

    bool ensureCanvas(const QImage& frame)
    {
        if (!canvas_.isNull()) {
            return true;
        }
        canvas_ = QImage(canvasSize_, compositionFormat(frame.format()));
        if (canvas_.isNull()) {
            return fail(QStringLiteral("APNG canvas allocation failed."));
        }
        canvas_.fill(Qt::transparent);
        if (frame.colorSpace().isValid()) {
            canvas_.setColorSpace(frame.colorSpace());
        }
        return true;
    }

    bool applyPreviousDisposal()
    {
        if (composedUntil_ < 0) {
            return true;
        }
        const auto& previous = frames_.at(composedUntil_);
        const QRect rect(previous.offset, previous.size);
        if (previous.dispose == DisposeOp::Background) {
            clearRect(rect);
        } else if (previous.dispose == DisposeOp::Previous) {
            if (previousRegion_.isNull() || previousRect_ != rect) {
                return fail(QStringLiteral("APNG previous-frame disposal state is unavailable."));
            }
            QPainter painter(&canvas_);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(rect.topLeft(), previousRegion_);
        }
        previousRegion_ = {};
        previousRect_ = {};
        return true;
    }

    bool composeFrame(int frameIndex, const QImage& frame)
    {
        if (!ensureCanvas(frame) || !applyPreviousDisposal()) {
            return false;
        }

        const auto& control = frames_.at(frameIndex);
        const QRect rect(control.offset, control.size);
        if (control.dispose == DisposeOp::Previous) {
            previousRegion_ = canvas_.copy(rect);
            previousRect_ = rect;
            if (previousRegion_.isNull()) {
                return fail(QStringLiteral("APNG could not preserve the disposal region."));
            }
        }

        QPainter painter(&canvas_);
        painter.setCompositionMode(control.blend == BlendOp::Source
                                       ? QPainter::CompositionMode_Source
                                       : QPainter::CompositionMode_SourceOver);
        painter.drawImage(control.offset, frame);
        if (!painter.isActive()) {
            return fail(QStringLiteral("APNG frame composition failed."));
        }
        painter.end();
        composedUntil_ = frameIndex;
        return true;
    }

    void resetComposition()
    {
        canvas_ = {};
        previousRegion_ = {};
        previousRect_ = {};
        composedUntil_ = -1;
    }

    bool metadataAttempted_ = false;
    bool metadataReady_ = false;
    bool readComplete_ = false;
    qint64 sourceSize_ = 0;
    quint64 pixelBudget_ = 0;
    QSize canvasSize_;
    QByteArray ihdr_;
    QVector<SourceChunk> globalChunks_;
    QVector<FrameControl> frames_;
    quint32 playCount_ = 0;
    int frameNumber_ = -1;
    int pendingFrame_ = 0;
    int delayMs_ = 0;
    int composedUntil_ = -1;
    QImage canvas_;
    QImage previousRegion_;
    QRect previousRect_;
};
} // namespace

extern "C" Q_DECL_EXPORT QImageIOHandler* licasaCreateImageHandler() { return new ApngHandler; }
