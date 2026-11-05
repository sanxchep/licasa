#include "imaging/image_decode_contract.h"

#include <QColorSpace>
#include <QIODevice>
#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <limits>

namespace {
constexpr int headerBytes = 14;
constexpr quint64 maximumSourcePixels = 1000ull * 1000 * 1000;
constexpr char endMarker[8] = {0, 0, 0, 0, 0, 0, 0, 1};

struct Pixel {
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
    uchar a = 0;
};

struct Header {
    QSize size;
    int channels = 0;
    int colorSpace = 0;
};

bool parseHeader(const QByteArray& data, Header* header)
{
    if (data.size() < headerBytes || data.left(4) != QByteArrayLiteral("qoif")) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
    const quint32 width = qFromBigEndian<quint32>(bytes + 4);
    const quint32 height = qFromBigEndian<quint32>(bytes + 8);
    if (!width || !height || width > 100000 || height > 100000 ||
        quint64(width) * height > maximumSourcePixels || (bytes[12] != 3 && bytes[12] != 4) ||
        bytes[13] > 1) {
        return false;
    }
    *header = {QSize(int(width), int(height)), int(bytes[12]), int(bytes[13])};
    return true;
}

class BufferedInput {
  public:
    explicit BufferedInput(QIODevice* device) : device_(device) {}

    int byte()
    {
        if (position_ == available_) {
            const qint64 count = device_->read(buffer_.data(), qint64(buffer_.size()));
            if (count <= 0) {
                return -1;
            }
            position_ = 0;
            available_ = qsizetype(count);
        }
        return uchar(buffer_[position_++]);
    }

    template <int Channels> bool literal(Pixel& pixel)
    {
        static_assert(Channels == 3 || Channels == 4);
        if (available_ - position_ >= Channels) {
            const auto* bytes = reinterpret_cast<const uchar*>(buffer_.data() + position_);
            pixel.r = bytes[0];
            pixel.g = bytes[1];
            pixel.b = bytes[2];
            if constexpr (Channels == 4) {
                pixel.a = bytes[3];
            }
            position_ += Channels;
            return true;
        }
        // A literal may straddle a refill or end in truncated input. Keep the
        // checked byte reader for that boundary while handling normal literals
        // with a single contiguous-range check.
        const int r = byte(), g = byte(), b = byte();
        const int a = Channels == 4 ? byte() : pixel.a;
        if (r < 0 || g < 0 || b < 0 || a < 0) {
            return false;
        }
        pixel = {uchar(r), uchar(g), uchar(b), uchar(a)};
        return true;
    }

  private:
    QIODevice* device_;
    std::array<char, 64 * 1024> buffer_;
    qsizetype position_ = 0;
    qsizetype available_ = 0;
};

// Select output-width behavior once, without a per-pixel dispatch or scaled
// sample-position bookkeeping on a full-width row. Both paths share the parser.
template <bool FullWidth>
bool decodePixels(QIODevice* device, QSize sourceSize, QSize outputSize, QImage& decoded)
{
    BufferedInput input(device);
    std::array<Pixel, 64> index{};
    Pixel pixel{0, 0, 0, 255};
    int run = 0;
    int nextOutputY = 0;
    const int sourceWidth = sourceSize.width();
    const int sourceHeight = sourceSize.height();
    const int sampleDenominator = 2 * outputSize.width();
    const int sampleStep = sourceWidth / outputSize.width();
    const int sampleFractionStep = 2 * (sourceWidth % outputSize.width());
    const int initialSampleFraction = sourceWidth % sampleDenominator;
    for (int y = 0; y < sourceHeight; ++y) {
        const bool selectedRow =
            nextOutputY < outputSize.height() &&
            y == (2ll * nextOutputY + 1) * sourceHeight / (2ll * outputSize.height());
        int nextOutputX = 0;
        int nextSourceX = sourceWidth / sampleDenominator;
        int sampleFraction = initialSampleFraction;
        const auto advanceSample = [&] {
            // Exactly advance floor((2*k + 1)*sourceWidth / (2*outputWidth)).
            // The remainder stays below sampleDenominator; one carry suffices.
            nextSourceX += sampleStep;
            sampleFraction += sampleFractionStep;
            if (sampleFraction >= sampleDenominator) {
                sampleFraction -= sampleDenominator;
                ++nextSourceX;
            }
        };
        int nextCancellationX = 0;
        QRgb* row = selectedRow ? reinterpret_cast<QRgb*>(decoded.scanLine(nextOutputY)) : nullptr;
        for (int x = 0; x < sourceWidth;) {
            if (x >= nextCancellationX) {
                if (Licasa::ImageDecodeContract::cancelled(device)) {
                    return false;
                }
                nextCancellationX = x + 4096;
            }
            if (run == 0) {
                const int tag = input.byte();
                if (tag < 0) {
                    return false;
                }
                if (tag == 0xfe) {
                    if (!input.literal<3>(pixel)) {
                        return false;
                    }
                } else if (tag == 0xff) {
                    if (!input.literal<4>(pixel)) {
                        return false;
                    }
                } else {
                    switch (tag & 0xc0) {
                    case 0x00:
                        pixel = index[size_t(tag & 63)];
                        break;
                    case 0x40:
                        pixel.r = uchar(pixel.r + ((tag >> 4 & 3) - 2));
                        pixel.g = uchar(pixel.g + ((tag >> 2 & 3) - 2));
                        pixel.b = uchar(pixel.b + ((tag & 3) - 2));
                        break;
                    case 0x80: {
                        const int second = input.byte();
                        if (second < 0) {
                            return false;
                        }
                        const int dg = (tag & 63) - 32;
                        pixel.r = uchar(pixel.r + dg + (second >> 4) - 8);
                        pixel.g = uchar(pixel.g + dg);
                        pixel.b = uchar(pixel.b + dg + (second & 15) - 8);
                        break;
                    }
                    default:
                        run = tag & 63;
                        break;
                    }
                }
                const int hash = (pixel.r * 3 + pixel.g * 5 + pixel.b * 7 + pixel.a * 11) & 63;
                index[size_t(hash)] = pixel;
                ++run; // The tag's pixel and any QOI_OP_RUN repetitions form one span.
            }
            if (run == 1) {
                // Literal/diff/index-heavy images should keep the scalar
                // path: form a pixel only when this source pixel is selected.
                if constexpr (FullWidth) {
                    if (selectedRow) {
                        row[x] = qRgba(pixel.r, pixel.g, pixel.b, pixel.a);
                        nextOutputX = x + 1;
                    }
                } else if (selectedRow && nextOutputX < outputSize.width() && x == nextSourceX) {
                    row[nextOutputX++] = qRgba(pixel.r, pixel.g, pixel.b, pixel.a);
                    advanceSample();
                }
                run = 0;
                ++x;
                continue;
            }
            const int count = std::min(run, sourceWidth - x);
            const int spanEnd = x + count;
            if (selectedRow) {
                const QRgb value = qRgba(pixel.r, pixel.g, pixel.b, pixel.a);
                if constexpr (FullWidth) {
                    std::fill(row + x, row + spanEnd, value);
                    nextOutputX = spanEnd;
                    nextSourceX = spanEnd;
                } else {
                    // Compute a sample position only when an output pixel is
                    // written. Long runs and unselected source pixels require
                    // neither per-pixel division nor per-pixel expansion.
                    while (nextOutputX < outputSize.width() && nextSourceX < spanEnd) {
                        row[nextOutputX++] = value;
                        advanceSample();
                    }
                }
            }
            run -= count;
            x = spanEnd;
        }
        if (selectedRow) {
            if (nextOutputX != outputSize.width()) {
                return false;
            }
            ++nextOutputY;
        }
    }
    if (nextOutputY != outputSize.height()) {
        return false;
    }
    for (char expected : endMarker) {
        if (input.byte() != uchar(expected)) {
            return false;
        }
    }
    return true;
}

class QoiHandler final : public QImageIOHandler {
  public:
    bool canRead() const override
    {
        Header header;
        return device() && device()->isReadable() &&
               parseHeader(device()->peek(headerBytes), &header);
    }

    bool supportsOption(ImageOption option) const override
    {
        return option == Size || option == ScaledSize;
    }

    QVariant option(ImageOption option) const override
    {
        if (option == ScaledSize) {
            return scaledSize_;
        }
        if (option == Size && device()) {
            Header header;
            if (parseHeader(device()->peek(headerBytes), &header)) {
                return header.size;
            }
        }
        return {};
    }

    void setOption(ImageOption option, const QVariant& value) override
    {
        if (option == ScaledSize) {
            scaledSize_ = value.toSize();
        }
    }

    bool read(QImage* image) override
    {
        if (!image || !device() || !device()->isReadable()) {
            return false;
        }
        Header header;
        if (!parseHeader(device()->peek(headerBytes), &header) ||
            device()->read(headerBytes).size() != headerBytes) {
            return false;
        }
        const QSize requestedSize = scaledSize_.isValid() ? scaledSize_ : header.size;
        const QSize outputSize = requestedSize.boundedTo(header.size);
        if (!Licasa::ImageDecodeContract::allows(
                requestedSize, Licasa::ImageDecodeContract::pixelBudget(device()))) {
            return false;
        }
        const quint64 outputBytes = quint64(requestedSize.width()) * requestedSize.height() * 4;
        const int allocationMiB = QImageReader::allocationLimit();
        if (allocationMiB > 0 && outputBytes > quint64(allocationMiB) * 1024 * 1024) {
            return false;
        }
        QImage decoded(outputSize,
                       header.channels == 4 ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        if (decoded.isNull()) {
            return false;
        }

        const bool decodedPixels =
            outputSize.width() == header.size.width()
                ? decodePixels<true>(device(), header.size, outputSize, decoded)
                : decodePixels<false>(device(), header.size, outputSize, decoded);
        if (!decodedPixels) {
            return false;
        }
        decoded.setColorSpace(
            QColorSpace(header.colorSpace == 1 ? QColorSpace::SRgbLinear : QColorSpace::SRgb));
        *image = outputSize == requestedSize
                     ? std::move(decoded)
                     : decoded.scaled(requestedSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        return true;
    }

  private:
    QSize scaledSize_;
};

class QoiPlugin final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "qoi.json")
  public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override
    {
        if (!format.isEmpty() && format.toLower() != QByteArrayLiteral("qoi")) {
            return {};
        }
        if (!device) {
            return format.isEmpty() ? Capabilities{} : CanRead;
        }
        Header header;
        return parseHeader(device->peek(headerBytes), &header) ? CanRead : Capabilities{};
    }

    QImageIOHandler* create(QIODevice* device, const QByteArray&) const override
    {
        auto* handler = new QoiHandler;
        handler->setDevice(device);
        handler->setFormat("qoi");
        return handler;
    }
};
} // namespace

#include "qoi_plugin.moc"
