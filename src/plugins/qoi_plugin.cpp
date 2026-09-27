#include "imaging/image_decode_contract.h"

#include <QColorSpace>
#include <QIODevice>
#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QtEndian>

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
    uchar a = 255;
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

  private:
    QIODevice* device_;
    std::array<char, 64 * 1024> buffer_{};
    qsizetype position_ = 0;
    qsizetype available_ = 0;
};

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

        BufferedInput input(device());
        std::array<Pixel, 64> index{};
        Pixel pixel;
        int run = 0;
        int nextOutputY = 0;
        const int sourceWidth = header.size.width();
        const int sourceHeight = header.size.height();
        for (int y = 0; y < sourceHeight; ++y) {
            const bool selectedRow =
                nextOutputY < outputSize.height() &&
                y == (2ll * nextOutputY + 1) * sourceHeight / (2ll * outputSize.height());
            int nextOutputX = 0;
            QRgb* row =
                selectedRow ? reinterpret_cast<QRgb*>(decoded.scanLine(nextOutputY)) : nullptr;
            for (int x = 0; x < sourceWidth; ++x) {
                if ((x & 4095) == 0 && Licasa::ImageDecodeContract::cancelled(device())) {
                    return false;
                }
                if (run > 0) {
                    --run;
                } else {
                    const int tag = input.byte();
                    if (tag < 0) {
                        return false;
                    }
                    if (tag == 0xfe) {
                        const int r = input.byte(), g = input.byte(), b = input.byte();
                        if (r < 0 || g < 0 || b < 0) {
                            return false;
                        }
                        pixel.r = uchar(r);
                        pixel.g = uchar(g);
                        pixel.b = uchar(b);
                    } else if (tag == 0xff) {
                        const int r = input.byte(), g = input.byte(), b = input.byte(),
                                  a = input.byte();
                        if (r < 0 || g < 0 || b < 0 || a < 0) {
                            return false;
                        }
                        pixel = {uchar(r), uchar(g), uchar(b), uchar(a)};
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
                }
                if (selectedRow && nextOutputX < outputSize.width() &&
                    x == (2ll * nextOutputX + 1) * sourceWidth / (2ll * outputSize.width())) {
                    row[nextOutputX] = qRgba(pixel.r, pixel.g, pixel.b, pixel.a);
                    ++nextOutputX;
                }
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
