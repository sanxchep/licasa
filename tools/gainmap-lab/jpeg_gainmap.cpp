#include "jpeg_gainmap.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QColorSpace>
#include <QImageReader>
#include <QSysInfo>
#include <QTransform>
#include <QXmlStreamReader>
#include <QtEndian>

#include <ultrahdr_api.h>

#ifdef LICASA_USE_PRIVATE_UHDR
#include "codec_plugin_paths.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace Licasa::GainmapLab {
namespace {
constexpr qint64 maximumHeaderBytes = 1024 * 1024;
constexpr int maximumHeaderSegments = 512;
constexpr int maximumDimension = 4096;
constexpr quint64 maximumPrimarySizeDiscrepancy = 4096;
constexpr float maximumBoost = 64.0f;
constexpr QByteArrayView mpfSignature("MPF\0", 4);
constexpr char standardXmpId[] = "http://ns.adobe.com/xap/1.0/";
constexpr QByteArrayView xmpSignature(standardXmpId, sizeof(standardXmpId));
constexpr QByteArrayView soi("\xff\xd8", 2);
constexpr QByteArrayView eoi("\xff\xd9", 2);
constexpr QLatin1String rdfNamespace("http://www.w3.org/1999/02/22-rdf-syntax-ns#");
constexpr QLatin1String containerNamespace("http://ns.google.com/photos/1.0/container/");
constexpr QLatin1String itemNamespace("http://ns.google.com/photos/1.0/container/item/");
static_assert(UHDR_LIB_VERSION == 20002,
              "Requalify the gain-map lab for a new libultrahdr version.");
static_assert(QSysInfo::ByteOrder == QSysInfo::LittleEndian);

#define LICASA_UHDR_DECODER_API(X) \
    X(uhdr_create_decoder) \
    X(uhdr_release_decoder) \
    X(uhdr_dec_set_out_img_format) \
    X(uhdr_dec_set_out_color_transfer) \
    X(uhdr_dec_set_out_max_display_boost) \
    X(uhdr_dec_set_image) \
    X(uhdr_dec_probe) \
    X(uhdr_dec_get_base_image) \
    X(uhdr_dec_get_gainmap_image) \
    X(uhdr_dec_get_image_width) \
    X(uhdr_dec_get_image_height) \
    X(uhdr_dec_get_gainmap_width) \
    X(uhdr_dec_get_gainmap_height) \
    X(uhdr_dec_get_gainmap_metadata) \
    X(uhdr_decode) \
    X(uhdr_get_decoded_image)

struct DecoderApi {
#define LICASA_DECLARE(name) decltype(&::name) name = nullptr;
    LICASA_UHDR_DECODER_API(LICASA_DECLARE)
#undef LICASA_DECLARE
};

const DecoderApi* decoderApi()
{
#ifdef LICASA_USE_PRIVATE_UHDR
    // An admitted JPEG gain map is the first caller. Keep a successfully
    // resolved codec resident for simultaneous and later decode workers.
    struct PrivateDecoder {
        QLibrary library;
        DecoderApi api;
        bool available = false;

        PrivateDecoder()
        {
            const QDir executable(QCoreApplication::applicationDirPath());
            const QString installed = executable.absoluteFilePath(
                QStringLiteral(LICASA_INSTALL_PLUGIN_RELATIVE "/gainmap/libuhdr.so.2"));
            const QString inTree = executable.absoluteFilePath(
                QStringLiteral("plugins/gainmap/libuhdr.so.2"));
            const QString path = QFileInfo(installed).isFile() ? installed : inTree;
            if (!QFileInfo(path).isFile()) {
                return;
            }
            library.setFileName(path);
            library.setLoadHints(QLibrary::ResolveAllSymbolsHint);
            if (!library.load()) {
                return;
            }
#define LICASA_RESOLVE(name) \
    api.name = reinterpret_cast<decltype(api.name)>(library.resolve(#name)); \
    if (!api.name) { \
        library.unload(); \
        return; \
    }
            LICASA_UHDR_DECODER_API(LICASA_RESOLVE)
#undef LICASA_RESOLVE
            available = true;
        }
    };
    static const PrivateDecoder codec;
    return codec.available ? &codec.api : nullptr;
#else
#define LICASA_LINK(name) &::name,
    static constexpr DecoderApi codec{LICASA_UHDR_DECODER_API(LICASA_LINK)};
#undef LICASA_LINK
    return &codec;
#endif
}
#undef LICASA_UHDR_DECODER_API

struct JpegRanges {
    quint64 declaredBaseBytes;
    quint64 gainStart;
    quint64 totalBytes;
};

struct XmpLocation {
    qint64 offset;
    qint64 size;
};

bool cancelled(const std::atomic_bool* flag)
{
    return flag && flag->load(std::memory_order_relaxed);
}

std::optional<JpegRanges> mpfRanges(QByteArrayView tiff, quint64 tiffOffset, quint64 fileBytes)
{
    const bool little = tiff.startsWith("II");
    if (tiff.size() < 8 || (!little && !tiff.startsWith("MM"))) {
        return std::nullopt;
    }
    const auto read16 = [&](quint64 offset) {
        return little ? qFromLittleEndian<quint16>(tiff.data() + offset)
                      : qFromBigEndian<quint16>(tiff.data() + offset);
    };
    const auto read32 = [&](quint64 offset) {
        return little ? qFromLittleEndian<quint32>(tiff.data() + offset)
                      : qFromBigEndian<quint32>(tiff.data() + offset);
    };
    const quint64 size = quint64(tiff.size());
    const quint64 ifd = read32(4);
    if (read16(2) != 42 || ifd < 8 || ifd + 2 > size) {
        return std::nullopt;
    }
    const quint64 count = read16(ifd);
    const quint64 firstTag = ifd + 2;
    const quint64 endTags = firstTag + count * 12;
    if (endTags + 4 > size) {
        return std::nullopt;
    }
    bool twoImages = false;
    quint64 entries = 0;
    for (quint64 tag = firstTag; tag < endTags; tag += 12) {
        switch (read16(tag)) {
        case 0xb001: // NumberOfImages, one TIFF LONG.
            if (twoImages || read16(tag + 2) != 4 || read32(tag + 4) != 1 || read32(tag + 8) != 2) {
                return std::nullopt;
            }
            twoImages = true;
            break;
        case 0xb002: // MPEntry, two 16-byte records.
            if (entries || read16(tag + 2) != 7 || read32(tag + 4) != 32) {
                return std::nullopt;
            }
            entries = read32(tag + 8);
            if (entries < endTags + 4 || entries + 32 > size) {
                return std::nullopt;
            }
            break;
        default:
            break;
        }
    }
    constexpr quint32 imageFormatMask = 0x07000000;
    if (!twoImages || !entries || (read32(entries) & imageFormatMask) ||
        (read32(entries + 16) & imageFormatMask) || read32(entries + 8) != 0) {
        return std::nullopt;
    }

    const quint64 baseBytes = read32(entries + 4);
    const quint64 gainOffset = tiffOffset + read32(entries + 24);
    const quint64 gainBytes = read32(entries + 20);
    if (baseBytes < tiffOffset + size || gainOffset < baseBytes ||
        gainOffset - baseBytes > maximumPrimarySizeDiscrepancy || gainBytes < 4 ||
        gainOffset + gainBytes > fileBytes) {
        return std::nullopt;
    }
    return JpegRanges{baseBytes, gainOffset, gainOffset + gainBytes};
}

bool isElement(const QXmlStreamReader& reader, QLatin1String nameSpace, QLatin1String name)
{
    return reader.namespaceUri() == nameSpace && reader.name() == name;
}

std::optional<quint64> byteCount(QStringView value)
{
    if (value.isNull()) {
        return 0; // Optional primary length and padding default to zero.
    }
    if (value.isEmpty() || !std::all_of(value.begin(), value.end(), [](QChar digit) {
            return digit >= u'0' && digit <= u'9';
        })) {
        return std::nullopt;
    }
    bool ok = false;
    const auto size = value.toULongLong(&ok);
    return ok ? std::optional<quint64>(size) : std::nullopt;
}

std::optional<quint64> readDirectory(QXmlStreamReader& reader)
{
    if (!reader.readNextStartElement() || !isElement(reader, rdfNamespace, QLatin1String("Seq"))) {
        return std::nullopt;
    }
    int index = 0;
    quint64 gainBytes = 0;
    while (reader.readNextStartElement()) {
        if (index == 3 || !isElement(reader, rdfNamespace, QLatin1String("li")) ||
            !reader.readNextStartElement() ||
            !isElement(reader, containerNamespace, QLatin1String("Item"))) {
            return std::nullopt;
        }
        const auto fields = reader.attributes();
        const auto mime = fields.value(itemNamespace, QLatin1String("Mime"));
        const auto semantic = fields.value(itemNamespace, QLatin1String("Semantic"));
        const auto length = byteCount(fields.value(itemNamespace, QLatin1String("Length")));
        const auto padding = byteCount(fields.value(itemNamespace, QLatin1String("Padding")));
        if (!length || padding != 0 ||
            !fields.value(itemNamespace, QLatin1String("URI")).isNull()) {
            return std::nullopt;
        }
        if (index == 0) {
            if (semantic != QLatin1String("Primary") || mime != QLatin1String("image/jpeg") ||
                *length != 0) {
                return std::nullopt;
            }
        } else if (index == 1) {
            if (semantic != QLatin1String("GainMap") || mime != QLatin1String("image/jpeg") ||
                *length == 0) {
                return std::nullopt;
            }
            gainBytes = *length;
        } else if (semantic != QLatin1String("MotionPhoto") ||
                   (mime != QLatin1String("video/mp4") &&
                    mime != QLatin1String("video/quicktime")) ||
                   *length == 0) {
            return std::nullopt;
        }
        if (reader.readNextStartElement() || reader.readNextStartElement() || reader.hasError()) {
            return std::nullopt; // Item and rdf:li must both end here.
        }
        ++index;
    }
    if (reader.hasError() || gainBytes == 0 || reader.readNextStartElement()) {
        return std::nullopt;
    }
    return gainBytes;
}

std::optional<quint64> xmpGainLength(const QByteArray& packet)
{
    QXmlStreamReader reader(packet);
    std::optional<quint64> gainBytes;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            return std::nullopt;
        }
        if (token == QXmlStreamReader::StartElement &&
            isElement(reader, containerNamespace, QLatin1String("Directory"))) {
            if (gainBytes) {
                return std::nullopt;
            }
            gainBytes = readDirectory(reader);
            if (!gainBytes) {
                return std::nullopt;
            }
        }
    }
    return reader.hasError() ? std::nullopt : gainBytes;
}

std::optional<JpegRanges> readHeader(QFile& file, quint64 fileBytes,
                                     const std::atomic_bool* cancelFlag, bool& noGainMap)
{
    if (file.read(2) != soi) {
        noGainMap = true;
        return std::nullopt;
    }
    std::optional<JpegRanges> ranges;
    std::optional<XmpLocation> xmp;
    bool duplicateXmp = false;
    const qint64 limit = qint64(std::min(fileBytes, quint64(maximumHeaderBytes)));
    for (int segment = 0; segment < maximumHeaderSegments && file.pos() + 2 <= limit; ++segment) {
        char marker;
        if (cancelled(cancelFlag) || !file.getChar(&marker) || uchar(marker) != 0xff) {
            return std::nullopt;
        }
        do {
            if (file.pos() >= limit || !file.getChar(&marker)) {
                return std::nullopt;
            }
        } while (uchar(marker) == 0xff);
        if (uchar(marker) == 0xda) {
            if (ranges && ranges->declaredBaseBytes != ranges->gainStart) {
                if (!xmp || duplicateXmp || !file.seek(xmp->offset)) {
                    return std::nullopt;
                }
                const QByteArray packet = file.read(xmp->size);
                if (packet.size() != xmp->size ||
                    xmpGainLength(packet) != ranges->totalBytes - ranges->gainStart) {
                    return std::nullopt;
                }
            }
            noGainMap = !ranges;
            return ranges;
        }
        if (uchar(marker) == 0xd9 || file.pos() + 2 > limit) {
            return std::nullopt;
        }
        const QByteArray length = file.read(2);
        if (length.size() != 2) {
            return std::nullopt;
        }
        const int payloadBytes = int(qFromBigEndian<quint16>(length.constData())) - 2;
        const qint64 payloadOffset = file.pos();
        if (payloadBytes < 0 || payloadOffset + payloadBytes > limit) {
            return std::nullopt;
        }
        if (uchar(marker) == 0xe2 && payloadBytes >= mpfSignature.size() &&
            file.peek(mpfSignature.size()) == mpfSignature) {
            if (ranges) {
                return std::nullopt;
            }
            const QByteArray payload = file.read(payloadBytes);
            if (payload.size() != payloadBytes) {
                return std::nullopt;
            }
            ranges = mpfRanges(QByteArrayView(payload).sliced(mpfSignature.size()),
                               quint64(payloadOffset + mpfSignature.size()), fileBytes);
            if (!ranges) {
                return std::nullopt;
            }
        } else {
            if (uchar(marker) == 0xe1 && payloadBytes >= xmpSignature.size() &&
                file.peek(xmpSignature.size()) == xmpSignature) {
                duplicateXmp = xmp.has_value();
                xmp = XmpLocation{payloadOffset + xmpSignature.size(),
                                  payloadBytes - xmpSignature.size()};
            }
            if (!file.seek(payloadOffset + payloadBytes)) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

bool matches(const uhdr_mem_block_t* encoded, QByteArrayView declared)
{
    return encoded && encoded->data && encoded->data_sz == size_t(declared.size()) &&
           std::memcmp(encoded->data, declared.data(), encoded->data_sz) == 0;
}

bool validMetadata(const uhdr_gainmap_metadata_t* map)
{
    const auto within = [](float value, float min, float max) {
        return value >= min && value <= max;
    };
    if (!map || !within(map->hdr_capacity_min, 1.0f, maximumBoost) ||
        !within(map->hdr_capacity_max, map->hdr_capacity_min, maximumBoost) ||
        map->hdr_capacity_max == map->hdr_capacity_min) {
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        if (!within(map->min_content_boost[c], 1.0f / maximumBoost, maximumBoost) ||
            !within(map->max_content_boost[c], map->min_content_boost[c], maximumBoost) ||
            !within(map->gamma[c], 1.0f / 16, 16.0f) || !within(map->offset_sdr[c], 0.0f, 1.0f) ||
            !within(map->offset_hdr[c], 0.0f, 1.0f)) {
            return false;
        }
    }
    return true;
}

bool validSize(QSize size)
{
    return !size.isEmpty() && size.width() <= maximumDimension && size.height() <= maximumDimension;
}

std::optional<QImageIOHandler::Transformations> primaryOrientation(QByteArrayView jpeg,
                                                                   QSize expectedSize)
{
    QByteArray view = QByteArray::fromRawData(jpeg.data(), jpeg.size());
    QBuffer source(&view);
    if (!source.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QImageReader reader(&source, "jpeg");
    if (reader.size() != expectedSize) {
        return std::nullopt;
    }
    return reader.transformation();
}

QImage retainOriented(const QImage& source, QImageIOHandler::Transformations orientation)
{
    using Handler = QImageIOHandler;
    if (orientation == Handler::TransformationNone) {
        return source.copy();
    }
    const int h = orientation.testFlag(Handler::TransformationMirror) ? -1 : 1;
    const int v = orientation.testFlag(Handler::TransformationFlip) ? -1 : 1;
    // Match QImageReader's mirror/flip followed by its clockwise quarter
    // turn, in one transform of the borrowed reconstruction.
    const QTransform matrix = orientation.testFlag(Handler::TransformationRotate90)
                                  ? QTransform(0, h, -v, 0, 0, 0)
                                  : QTransform(h, 0, 0, v, 0, 0);
    return source.transformed(matrix, Qt::FastTransformation);
}
} // namespace

std::optional<Result> decode(const QString& path, const ExternalFileIdentity& expected,
                             float headroom, QString& error, Limits limits,
                             const std::atomic_bool* cancelFlag, DecodeFailure* failure,
                             const std::function<bool(quint64)>& reserveRaster)
{
    error.clear();
    if (failure) *failure = DecodeFailure::Unavailable;
    const auto reject = [&](const QString& message) -> std::optional<Result> {
        error = message;
        return std::nullopt;
    };
    const auto accepted = [&](uhdr_error_info_t status) {
        if (status.error_code == UHDR_CODEC_OK) {
            return true;
        }
        error = status.has_detail ? QString::fromUtf8(status.detail)
                                  : QStringLiteral("Ultra HDR codec rejected the image.");
        return false;
    };
    if (!std::isfinite(headroom) || headroom < 1.0f || headroom > maximumBoost) {
        return reject(QStringLiteral("Invalid display headroom."));
    }
    if (cancelled(cancelFlag)) {
        return reject(QStringLiteral("Gain-map work cancelled."));
    }
    QFile file(path);
    const auto sourceMatches = [&] {
        return externalFileIdentity(file) == expected &&
               externalFileIdentityAtPath(path) == expected;
    };
    if (externalFileIdentityAtPath(path) != expected || !file.open(QIODevice::ReadOnly) ||
        !sourceMatches()) {
        return reject(QStringLiteral("JPEG source changed or is unavailable."));
    }
    bool noGainMap = false;
    const auto ranges = readHeader(file, expected.size, cancelFlag, noGainMap);
    if (cancelled(cancelFlag)) {
        return reject(QStringLiteral("Gain-map work cancelled."));
    }
    if (!ranges) {
        if (failure && noGainMap && sourceMatches()) *failure = DecodeFailure::NoGainMap;
        return reject(QStringLiteral("JPEG has no valid contiguous two-image MPF range."));
    }
    if (ranges->totalBytes > std::min(limits.compressedBytes, Limits{}.compressedBytes)) {
        return reject(QStringLiteral("JPEG exceeds the compressed gain-map budget."));
    }
    if (!file.seek(0)) {
        return reject(QStringLiteral("Unable to read the JPEG gain-map ranges."));
    }
    QByteArray bytes = file.read(qint64(ranges->totalBytes));
    if (quint64(bytes.size()) != ranges->totalBytes || !sourceMatches()) {
        return reject(QStringLiteral("JPEG source changed during the read."));
    }
    if (cancelled(cancelFlag)) {
        return reject(QStringLiteral("Gain-map work cancelled."));
    }
    const QByteArrayView all(bytes);
    // The MPF gain offset is usable only at an actual JPEG boundary. Exclude
    // the trailer and confirm the same split with the decoder's scanner.
    const auto base = all.first(qsizetype(ranges->gainStart));
    const auto gain = all.sliced(qsizetype(ranges->gainStart));
    if (!base.endsWith(eoi) || !gain.startsWith(soi) || !gain.endsWith(eoi)) {
        return reject(QStringLiteral("Declared JPEG ranges have invalid endpoints."));
    }
    const auto* api = decoderApi();
    if (!api) {
        return reject(QStringLiteral("Private Ultra HDR codec is unavailable."));
    }
    const std::unique_ptr<uhdr_codec_private_t, decltype(api->uhdr_release_decoder)> decoder(
        api->uhdr_create_decoder(), api->uhdr_release_decoder);
    if (!decoder) {
        return reject(QStringLiteral("Unable to allocate the Ultra HDR decoder."));
    }
    auto* context = decoder.get();
    uhdr_compressed_image_t input{bytes.data(),        size_t(bytes.size()), size_t(bytes.size()),
                                  UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED,  UHDR_CR_UNSPECIFIED};
    // libultrahdr seals the configuration at probe time.
    if (!accepted(api->uhdr_dec_set_out_img_format(context, UHDR_IMG_FMT_64bppRGBAHalfFloat)) ||
        !accepted(api->uhdr_dec_set_out_color_transfer(context, UHDR_CT_LINEAR)) ||
        !accepted(api->uhdr_dec_set_out_max_display_boost(context, headroom)) ||
        !accepted(api->uhdr_dec_set_image(context, &input)) || !accepted(api->uhdr_dec_probe(context))) {
        return std::nullopt;
    }
    if (!matches(api->uhdr_dec_get_base_image(context), base) ||
        !matches(api->uhdr_dec_get_gainmap_image(context), gain)) {
        return reject(QStringLiteral("Codec JPEG ranges disagree with the MPF index."));
    }
    const QSize baseSize(api->uhdr_dec_get_image_width(context), api->uhdr_dec_get_image_height(context));
    const QSize gainSize(api->uhdr_dec_get_gainmap_width(context), api->uhdr_dec_get_gainmap_height(context));
    if (!validSize(baseSize) || !validSize(gainSize) || gainSize.width() > baseSize.width() ||
        gainSize.height() > baseSize.height()) {
        return reject(QStringLiteral("JPEG exceeds the gain-map raster budget."));
    }
    const quint64 rasterPixels = quint64(baseSize.width()) * baseSize.height()
        + quint64(gainSize.width()) * gainSize.height();
    if (rasterPixels > std::min(limits.rasterPixels, maximumRasterPixels)) {
        return reject(QStringLiteral("JPEG exceeds the gain-map raster budget."));
    }
    if (!validMetadata(api->uhdr_dec_get_gainmap_metadata(context))) {
        return reject(QStringLiteral("Gain-map parameters exceed the lab limits."));
    }
    const auto orientation = primaryOrientation(base, baseSize);
    if (!orientation) {
        return reject(QStringLiteral("Unable to verify the primary JPEG orientation."));
    }
    bytes = {}; // The decoder owns its copy from uhdr_dec_set_image().
    if (cancelled(cancelFlag)) {
        return reject(QStringLiteral("Gain-map work cancelled."));
    }
    // Probe data is bounded by the compressed reservation. Charge the actual
    // combined raster before libultrahdr allocates its decoded images.
    if (reserveRaster && !reserveRaster(rasterPixels)) {
        if (failure && !cancelled(cancelFlag)) *failure = DecodeFailure::Memory;
        return reject(QStringLiteral("JPEG gain-map decode exceeds the shared memory budget."));
    }
    if (cancelled(cancelFlag)) {
        return reject(QStringLiteral("Gain-map work cancelled."));
    }
    if (!accepted(api->uhdr_decode(context))) {
        return std::nullopt;
    }
    const auto* pixels = api->uhdr_get_decoded_image(context);
    if (!pixels || pixels->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat || pixels->ct != UHDR_CT_LINEAR ||
        !pixels->planes[0] || pixels->w != unsigned(baseSize.width()) ||
        pixels->h != unsigned(baseSize.height()) || pixels->stride[0] != pixels->w) {
        return reject(QStringLiteral("Unexpected linear gain-map output."));
    }
    QColorSpace colorSpace;
    if (pixels->cg == UHDR_CG_BT_709) {
        colorSpace = QColorSpace::SRgbLinear;
    } else if (pixels->cg == UHDR_CG_DISPLAY_P3) {
        colorSpace =
            QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear);
    } else {
        return reject(QStringLiteral("Unsupported gain-map output color gamut."));
    }
    const QImage reconstructed(static_cast<const uchar*>(pixels->planes[0]), baseSize.width(),
                               baseSize.height(), qsizetype(pixels->stride[0]) * 8,
                               QImage::Format_RGBA16FPx4);
    QImage linear = retainOriented(reconstructed, *orientation);
    if (linear.isNull() || linear.format() != QImage::Format_RGBA16FPx4) {
        return reject(QStringLiteral("Unable to retain the gain-map raster."));
    }
    if (cancelled(cancelFlag)) {
        return reject(QStringLiteral("Gain-map work cancelled."));
    }
    if (!sourceMatches()) {
        return reject(QStringLiteral("JPEG source changed during reconstruction."));
    }
    linear.setColorSpace(colorSpace);
    return Result{std::move(linear), gainSize, qsizetype(ranges->totalBytes)};
}
} // namespace Licasa::GainmapLab
