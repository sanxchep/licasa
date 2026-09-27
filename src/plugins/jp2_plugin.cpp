#include "imaging/image_decode_contract.h"

#include <QColorSpace>
#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QtEndian>

#include <openjpeg.h>

#include <algorithm>
#include <limits>
#include <memory>

namespace {
constexpr uchar jp2Signature[] = {0, 0, 0, 12, 0x6a, 0x50, 0x20, 0x20, 13, 10, 0x87, 10};
constexpr uchar j2kSignature[] = {0xff, 0x4f, 0xff, 0x51};
constexpr quint64 maximumSourcePixels = 1000ull * 1000 * 1000;

QSize codestreamSize(QFile* file, OPJ_CODEC_FORMAT format)
{
    if (!file || !file->isOpen() || file->isSequential()) {
        return {};
    }
    const qint64 savedPosition = file->pos();
    const qint64 fileLength = file->size();
    qint64 codestream = 0;
    qint64 end = fileLength;
    if (format == OPJ_CODEC_JP2) {
        // Walk top-level boxes without loading their payloads. The codestream's
        // SIZ marker, rather than the JP2 image header, controls native decode.
        qint64 position = 0;
        bool found = false;
        for (int box = 0; box < 256 && position <= fileLength - 8; ++box) {
            if (!file->seek(position)) {
                break;
            }
            const QByteArray header = file->read(8);
            if (header.size() != 8) {
                break;
            }
            const auto* bytes = reinterpret_cast<const uchar*>(header.constData());
            const quint32 shortLength = qFromBigEndian<quint32>(bytes);
            qint64 headerLength = 8;
            quint64 boxLength = shortLength;
            if (shortLength == 1) {
                const QByteArray extended = file->read(8);
                if (extended.size() != 8) {
                    break;
                }
                boxLength =
                    qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(extended.constData()));
                headerLength = 16;
            } else if (shortLength == 0) {
                boxLength = quint64(fileLength - position);
            }
            if (boxLength < quint64(headerLength) || boxLength > quint64(fileLength - position)) {
                break;
            }
            if (header.mid(4, 4) == "jp2c") {
                codestream = position + headerLength;
                end = position + qint64(boxLength);
                found = true;
                break;
            }
            position += qint64(boxLength);
        }
        if (!found) {
            file->seek(savedPosition);
            return {};
        }
    } else if (format != OPJ_CODEC_J2K) {
        file->seek(savedPosition);
        return {};
    }
    QSize size;
    if (end - codestream >= 42 && file->seek(codestream)) {
        const QByteArray header = file->read(42);
        if (header.size() == 42) {
            const auto* bytes = reinterpret_cast<const uchar*>(header.constData());
            const quint16 markerLength = qFromBigEndian<quint16>(bytes + 4);
            const quint16 components = qFromBigEndian<quint16>(bytes + 40);
            const quint32 x1 = qFromBigEndian<quint32>(bytes + 8);
            const quint32 y1 = qFromBigEndian<quint32>(bytes + 12);
            const quint32 x0 = qFromBigEndian<quint32>(bytes + 16);
            const quint32 y0 = qFromBigEndian<quint32>(bytes + 20);
            if (bytes[0] == 0xff && bytes[1] == 0x4f && bytes[2] == 0xff && bytes[3] == 0x51 &&
                components >= 1 && components <= 4 && markerLength == 38 + 3 * components &&
                quint64(markerLength) + 4 <= quint64(end - codestream) && x1 > x0 && y1 > y0 &&
                x1 - x0 <= quint32(std::numeric_limits<int>::max()) &&
                y1 - y0 <= quint32(std::numeric_limits<int>::max())) {
                size = QSize(int(x1 - x0), int(y1 - y0));
            }
        }
    }
    file->seek(savedPosition);
    return size;
}

OPJ_CODEC_FORMAT formatFromSignature(QIODevice* device)
{
    if (!device || !device->isReadable()) {
        return OPJ_CODEC_UNKNOWN;
    }
    const QByteArray bytes = device->peek(12);
    if (bytes.size() >= 12 && std::equal(std::begin(jp2Signature), std::end(jp2Signature),
                                         reinterpret_cast<const uchar*>(bytes.constData()))) {
        return OPJ_CODEC_JP2;
    }
    if (bytes.size() >= 4 && std::equal(std::begin(j2kSignature), std::end(j2kSignature),
                                        reinterpret_cast<const uchar*>(bytes.constData()))) {
        return OPJ_CODEC_J2K;
    }
    return OPJ_CODEC_UNKNOWN;
}

void ignoreOpenJpegMessage(const char*, void*) {}

OPJ_SIZE_T readFile(void* buffer, OPJ_SIZE_T count, void* userData)
{
    auto* file = static_cast<QFile*>(userData);
    const qint64 read = file->read(static_cast<char*>(buffer), qint64(count));
    return read > 0 ? OPJ_SIZE_T(read) : OPJ_SIZE_T(-1);
}

OPJ_OFF_T skipFile(OPJ_OFF_T count, void* userData)
{
    auto* file = static_cast<QFile*>(userData);
    const qint64 position = file->pos();
    if (count < -position || count > file->size() - position || !file->seek(position + count)) {
        return -1;
    }
    return count;
}

OPJ_BOOL seekFile(OPJ_OFF_T position, void* userData)
{
    auto* file = static_cast<QFile*>(userData);
    return position >= 0 && position <= file->size() && file->seek(position) ? OPJ_TRUE : OPJ_FALSE;
}

struct Decoder {
    std::unique_ptr<opj_stream_t, decltype(&opj_stream_destroy)> stream{nullptr,
                                                                        &opj_stream_destroy};
    std::unique_ptr<opj_codec_t, decltype(&opj_destroy_codec)> codec{nullptr, &opj_destroy_codec};
    std::unique_ptr<opj_image_t, decltype(&opj_image_destroy)> image{nullptr, &opj_image_destroy};

    bool open(QFile* file, OPJ_CODEC_FORMAT format, int reduce,
              quint64 maximumDecodedPixels = maximumSourcePixels)
    {
        if (!file || !file->seek(0)) {
            return false;
        }
        stream.reset(opj_stream_create(64 * 1024, OPJ_TRUE));
        codec.reset(opj_create_decompress(format));
        if (!stream || !codec) {
            return false;
        }
        opj_stream_set_read_function(stream.get(), readFile);
        opj_stream_set_skip_function(stream.get(), skipFile);
        opj_stream_set_seek_function(stream.get(), seekFile);
        opj_stream_set_user_data(stream.get(), file, nullptr);
        opj_stream_set_user_data_length(stream.get(), OPJ_UINT64(file->size()));
        opj_set_info_handler(codec.get(), ignoreOpenJpegMessage, nullptr);
        opj_set_warning_handler(codec.get(), ignoreOpenJpegMessage, nullptr);
        opj_set_error_handler(codec.get(), ignoreOpenJpegMessage, nullptr);
        opj_dparameters_t parameters;
        opj_set_default_decoder_parameters(&parameters);
        parameters.cp_reduce = OPJ_UINT32(reduce);
        if (!opj_setup_decoder(codec.get(), &parameters)) {
            return false;
        }
        opj_codec_set_threads(codec.get(), std::min(8, std::max(1, opj_get_num_cpus())));
        opj_image_t* rawImage = nullptr;
        if (!opj_read_header(stream.get(), codec.get(), &rawImage)) {
            return false;
        }
        image.reset(rawImage);
        if (!image || image->x1 <= image->x0 || image->y1 <= image->y0 ||
            quint64(image->x1 - image->x0) * (image->y1 - image->y0) > maximumSourcePixels ||
            image->numcomps == 0 || image->numcomps == 2 || image->numcomps > 4) {
            return false;
        }
        // cp_reduce applies during decode, after the header is read. Admit the
        // native component dimensions for this attempt before OpenJPEG allocates
        // their sample buffers. The fallback loop must make the same check.
        const quint64 divisor = quint64(1) << reduce;
        for (OPJ_UINT32 index = 0; index < image->numcomps; ++index) {
            const auto& component = image->comps[index];
            const quint64 width = (quint64(component.w) + divisor - 1) / divisor;
            const quint64 height = (quint64(component.h) + divisor - 1) / divisor;
            if (width == 0 || height == 0 || width * height > maximumDecodedPixels) {
                return false;
            }
        }
        return true;
    }

    bool decode()
    {
        return opj_decode(codec.get(), stream.get(), image.get()) &&
               opj_end_decompress(codec.get(), stream.get());
    }
};

int toByte(const opj_image_comp_t& component, int x, int y, int width, int height)
{
    if (!component.data || component.w == 0 || component.h == 0 || component.prec == 0 ||
        component.prec > 31) {
        return 0;
    }
    const int column = std::min(int(component.w) - 1, int(quint64(x) * component.w / width));
    const int row = std::min(int(component.h) - 1, int(quint64(y) * component.h / height));
    const qint64 raw = component.data[size_t(row) * component.w + size_t(column)];
    const qint64 unsignedValue = component.sgnd ? raw + (qint64(1) << (component.prec - 1)) : raw;
    const qint64 maximum = (qint64(1) << component.prec) - 1;
    return int(std::clamp((unsignedValue * 255 + maximum / 2) / maximum, qint64(0), qint64(255)));
}

class Jp2Handler final : public QImageIOHandler {
  public:
    bool canRead() const override { return formatFromSignature(device()) != OPJ_CODEC_UNKNOWN; }

    bool supportsOption(ImageOption option) const override
    {
        return option == Size || option == ScaledSize;
    }

    QVariant option(ImageOption option) const override
    {
        if (option == ScaledSize) {
            return scaledSize_;
        }
        if (option != Size || !device()) {
            return {};
        }
        if (!originalSize_.isValid()) {
            auto* file = qobject_cast<QFile*>(device());
            if (!file) {
                return {};
            }
            originalSize_ = codestreamSize(file, formatFromSignature(device()));
        }
        return originalSize_;
    }

    void setOption(ImageOption option, const QVariant& value) override
    {
        if (option == ScaledSize) {
            scaledSize_ = value.toSize();
        }
    }

    bool read(QImage* output) override
    {
        if (!output || !device() || Licasa::ImageDecodeContract::cancelled(device())) {
            return false;
        }
        auto* file = qobject_cast<QFile*>(device());
        const OPJ_CODEC_FORMAT format = formatFromSignature(device());
        if (!file || format == OPJ_CODEC_UNKNOWN) {
            return false;
        }
        // Re-read SIZ for every decode. A prior size() query may have cached
        // dimensions before a caller changed the contents of the open file.
        const QSize sourceSize = codestreamSize(file, format);
        if (!sourceSize.isValid()) {
            return false;
        }
        const quint64 budget = Licasa::ImageDecodeContract::pixelBudget(device());
        if (!Licasa::ImageDecodeContract::allows(sourceSize, budget)) {
            return false;
        }
        const QSize target = scaledSize_.isValid() ? scaledSize_ : sourceSize;
        if (!Licasa::ImageDecodeContract::allows(target, budget)) {
            return false;
        }
        int reduce = 0;
        while (scaledSize_.isValid() && reduce < 5 &&
               (sourceSize.width() >> (reduce + 1)) >= target.width() &&
               (sourceSize.height() >> (reduce + 1)) >= target.height()) {
            ++reduce;
        }
        std::unique_ptr<Decoder> decoder;
        for (int attempt = reduce; attempt >= 0; --attempt) {
            auto candidate = std::make_unique<Decoder>();
            if (candidate->open(file, format, attempt, budget) && candidate->decode()) {
                decoder = std::move(candidate);
                break;
            }
        }
        if (!decoder || Licasa::ImageDecodeContract::cancelled(device()) ||
            decoder->image->numcomps == 0 || decoder->image->numcomps == 2 ||
            decoder->image->numcomps > 4) {
            return false;
        }
        const opj_image_t& source = *decoder->image;
        if (source.color_space == OPJ_CLRSPC_EYCC || source.color_space == OPJ_CLRSPC_CMYK) {
            return false;
        }
        const int width = int(source.comps[0].w);
        const int height = int(source.comps[0].h);
        if (width <= 0 || height <= 0 || quint64(width) * height > maximumSourcePixels) {
            return false;
        }
        const bool alpha = source.numcomps == 4;
        QImage image(QSize(width, height), alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        if (image.isNull()) {
            return false;
        }
        for (int y = 0; y < height; ++y) {
            if ((y & 31) == 0 && Licasa::ImageDecodeContract::cancelled(device())) {
                return false;
            }
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                int r = toByte(source.comps[0], x, y, width, height);
                int g = source.numcomps == 1 ? r : toByte(source.comps[1], x, y, width, height);
                int b = source.numcomps == 1 ? r : toByte(source.comps[2], x, y, width, height);
                if (source.color_space == OPJ_CLRSPC_SYCC && source.numcomps >= 3) {
                    const int cb = g - 128;
                    const int cr = b - 128;
                    g = std::clamp(r - ((88 * cb + 183 * cr) >> 8), 0, 255);
                    b = std::clamp(r + ((454 * cb) >> 8), 0, 255);
                    r = std::clamp(r + ((359 * cr) >> 8), 0, 255);
                }
                const int a = alpha ? toByte(source.comps[3], x, y, width, height) : 255;
                row[x] = qRgba(r, g, b, a);
            }
        }
        if (source.icc_profile_buf && source.icc_profile_len > 0 &&
            source.icc_profile_len <= 4 * 1024 * 1024) {
            image.setColorSpace(QColorSpace::fromIccProfile(
                QByteArray(reinterpret_cast<const char*>(source.icc_profile_buf),
                           int(source.icc_profile_len))));
        } else {
            image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        }
        *output = scaledSize_.isValid() && image.size() != scaledSize_
                      ? image.scaled(scaledSize_, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                      : std::move(image);
        return !output->isNull();
    }

  private:
    QSize scaledSize_;
    mutable QSize originalSize_;
};

class Jp2Plugin final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "jp2.json")
  public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override
    {
        const QByteArray normalized = format.toLower();
        if (!normalized.isEmpty() && normalized != "jp2" && normalized != "j2k" &&
            normalized != "j2c" && normalized != "jpc") {
            return {};
        }
        if (!device) {
            return normalized.isEmpty() ? Capabilities{} : CanRead;
        }
        return formatFromSignature(device) != OPJ_CODEC_UNKNOWN ? CanRead : Capabilities{};
    }

    QImageIOHandler* create(QIODevice* device, const QByteArray&) const override
    {
        auto* handler = new Jp2Handler;
        handler->setDevice(device);
        handler->setFormat("jp2");
        return handler;
    }
};
} // namespace

#include "jp2_plugin.moc"
