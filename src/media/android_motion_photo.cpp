#include "media/android_motion_photo.h"

#include <QByteArray>
#include <QFile>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace Licasa {
namespace {

constexpr qsizetype maximumXmpBytes = 1024 * 1024;
constexpr qsizetype maximumContainerItems = 64;
constexpr quint16 samsungMotionPhotoDataType = 0x0a30;
constexpr quint64 samsungMotionPhotoContentHeaderBytes = 24;
constexpr quint64 maximumSamsungSeftHeaderBytes = 64 * 1024;
constexpr quint32 maximumSamsungSeftRecords = 4096;
constexpr char samsungMotionPhotoDataName[] = "MotionPhoto_Data";
constexpr qsizetype samsungMotionPhotoDataNameSize = sizeof(samsungMotionPhotoDataName) - 1;

struct ContainerItem {
    QString mime;
    QString semantic;
    std::optional<quint64> length;
    std::optional<quint64> padding;
};

struct ParsedXmp {
    std::optional<qint64> motionPhoto;
    std::optional<qint64> version;
    std::optional<qint64> presentationTimestampUs;
    std::optional<qint64> microVideo;
    std::optional<qint64> microVideoVersion;
    std::optional<quint64> microVideoOffset;
    std::optional<qint64> microVideoPresentationTimestampUs;
    std::vector<ContainerItem> items;
    bool sawDirectory = false;
};

AndroidMotionPhotoDetection invalid(QString error)
{
    AndroidMotionPhotoDetection result;
    result.status = AndroidMotionPhotoStatus::Invalid;
    result.error = std::move(error);
    return result;
}

AndroidMotionPhotoDetection notMotionPhoto()
{
    AndroidMotionPhotoDetection result;
    result.status = AndroidMotionPhotoStatus::NotMotionPhoto;
    return result;
}

bool parseSignedInteger(QStringView value, qint64* result)
{
    bool ok = false;
    const qint64 parsed = value.toString().trimmed().toLongLong(&ok, 10);
    if (!ok) {
        return false;
    }
    *result = parsed;
    return true;
}

bool parseUnsignedInteger(QStringView value, quint64* result)
{
    const QString text = value.toString().trimmed();
    if (text.startsWith(QLatin1Char('-'))) {
        return false;
    }
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok, 10);
    if (!ok) {
        return false;
    }
    *result = parsed;
    return true;
}

bool assignSignedAttribute(std::optional<qint64>* destination, QStringView value,
                           QStringView fieldName, QString* error)
{
    if (destination->has_value()) {
        *error =
            QStringLiteral("Duplicate Android Motion Photo field: %1").arg(fieldName.toString());
        return false;
    }
    qint64 parsed = 0;
    if (!parseSignedInteger(value, &parsed)) {
        *error =
            QStringLiteral("Invalid Android Motion Photo integer: %1").arg(fieldName.toString());
        return false;
    }
    *destination = parsed;
    return true;
}

bool assignUnsignedAttribute(std::optional<quint64>* destination, QStringView value,
                             QStringView fieldName, QString* error)
{
    if (destination->has_value()) {
        *error =
            QStringLiteral("Duplicate Android Motion Photo field: %1").arg(fieldName.toString());
        return false;
    }
    quint64 parsed = 0;
    if (!parseUnsignedInteger(value, &parsed)) {
        *error =
            QStringLiteral("Invalid Android Motion Photo integer: %1").arg(fieldName.toString());
        return false;
    }
    *destination = parsed;
    return true;
}

bool parseXmp(QByteArrayView xmp, ParsedXmp* parsed, QString* error)
{
    if (xmp.size() > maximumXmpBytes) {
        *error = QStringLiteral("Android Motion Photo XMP exceeds the 1 MiB safety limit");
        return false;
    }
    if (xmp.isEmpty()) {
        return true;
    }

    const QByteArray packet(xmp.data(), xmp.size());
    QXmlStreamReader reader(packet);

    const QString cameraNamespace = QStringLiteral("http://ns.google.com/photos/1.0/camera/");
    const QString containerNamespace = QStringLiteral("http://ns.google.com/photos/1.0/container/");
    const QString itemNamespace = QStringLiteral("http://ns.google.com/photos/1.0/container/item/");

    int depth = 0;
    int directoryDepth = -1;

    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            *error = QStringLiteral("DTD/entity declarations are not accepted in Motion Photo XMP");
            return false;
        }

        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            const auto attributes = reader.attributes();

            const auto motionPhoto =
                attributes.value(cameraNamespace, QStringLiteral("MotionPhoto"));
            if (!motionPhoto.isNull() &&
                !assignSignedAttribute(&parsed->motionPhoto, motionPhoto,
                                       QStringLiteral("MotionPhoto"), error)) {
                return false;
            }

            const auto version =
                attributes.value(cameraNamespace, QStringLiteral("MotionPhotoVersion"));
            if (!version.isNull() &&
                !assignSignedAttribute(&parsed->version, version,
                                       QStringLiteral("MotionPhotoVersion"), error)) {
                return false;
            }

            const auto timestamp = attributes.value(
                cameraNamespace, QStringLiteral("MotionPhotoPresentationTimestampUs"));
            if (!timestamp.isNull() &&
                !assignSignedAttribute(&parsed->presentationTimestampUs, timestamp,
                                       QStringLiteral("MotionPhotoPresentationTimestampUs"),
                                       error)) {
                return false;
            }

            const auto microVideo = attributes.value(cameraNamespace, QStringLiteral("MicroVideo"));
            if (!microVideo.isNull() &&
                !assignSignedAttribute(&parsed->microVideo, microVideo,
                                       QStringLiteral("MicroVideo"), error)) {
                return false;
            }

            const auto microVideoVersion =
                attributes.value(cameraNamespace, QStringLiteral("MicroVideoVersion"));
            if (!microVideoVersion.isNull() &&
                !assignSignedAttribute(&parsed->microVideoVersion, microVideoVersion,
                                       QStringLiteral("MicroVideoVersion"), error)) {
                return false;
            }

            const auto microVideoOffset =
                attributes.value(cameraNamespace, QStringLiteral("MicroVideoOffset"));
            if (!microVideoOffset.isNull() &&
                !assignUnsignedAttribute(&parsed->microVideoOffset, microVideoOffset,
                                         QStringLiteral("MicroVideoOffset"), error)) {
                return false;
            }

            const auto microVideoTimestamp = attributes.value(
                cameraNamespace, QStringLiteral("MicroVideoPresentationTimestampUs"));
            if (!microVideoTimestamp.isNull() &&
                !assignSignedAttribute(
                    &parsed->microVideoPresentationTimestampUs, microVideoTimestamp,
                    QStringLiteral("MicroVideoPresentationTimestampUs"), error)) {
                return false;
            }

            if (reader.namespaceUri() == containerNamespace &&
                reader.name() == QStringLiteral("Directory")) {
                if (parsed->sawDirectory || directoryDepth >= 0) {
                    *error =
                        QStringLiteral("Motion Photo XMP contains multiple Container directories");
                    return false;
                }
                parsed->sawDirectory = true;
                directoryDepth = depth;
                continue;
            }

            if (directoryDepth >= 0 && depth > directoryDepth &&
                reader.namespaceUri() == containerNamespace &&
                reader.name() == QStringLiteral("Item")) {
                if (parsed->items.size() >= size_t(maximumContainerItems)) {
                    *error = QStringLiteral("Motion Photo Container directory exceeds 64 items");
                    return false;
                }

                ContainerItem item;
                const auto mime = attributes.value(itemNamespace, QStringLiteral("Mime"));
                const auto semantic = attributes.value(itemNamespace, QStringLiteral("Semantic"));
                const auto length = attributes.value(itemNamespace, QStringLiteral("Length"));
                const auto padding = attributes.value(itemNamespace, QStringLiteral("Padding"));

                if (mime.isNull() || semantic.isNull()) {
                    *error = QStringLiteral("Motion Photo Container item lacks Mime or Semantic");
                    return false;
                }
                item.mime = mime.toString();
                item.semantic = semantic.toString();

                if (!length.isNull()) {
                    quint64 parsedLength = 0;
                    if (!parseUnsignedInteger(length, &parsedLength)) {
                        *error = QStringLiteral("Invalid Motion Photo Container item Length");
                        return false;
                    }
                    item.length = parsedLength;
                }
                if (!padding.isNull()) {
                    quint64 parsedPadding = 0;
                    if (!parseUnsignedInteger(padding, &parsedPadding)) {
                        *error = QStringLiteral("Invalid Motion Photo Container item Padding");
                        return false;
                    }
                    item.padding = parsedPadding;
                }
                parsed->items.push_back(std::move(item));
            }
            continue;
        }

        if (token == QXmlStreamReader::EndElement) {
            if (directoryDepth == depth && reader.namespaceUri() == containerNamespace &&
                reader.name() == QStringLiteral("Directory")) {
                directoryDepth = -1;
            }
            --depth;
        }
    }

    if (reader.hasError()) {
        *error = QStringLiteral("Malformed Motion Photo XMP at line %1, column %2: %3")
                     .arg(reader.lineNumber())
                     .arg(reader.columnNumber())
                     .arg(reader.errorString());
        return false;
    }
    return true;
}

bool verifyIsoBmffVideoHeader(ByteRangeDevice* video, quint64 videoLength, QString* error)
{
    constexpr quint64 maximumHeaderScan = 64 * 1024;
    constexpr int maximumLeadingBoxes = 8;

    quint64 offset = 0;
    for (int boxIndex = 0;
         boxIndex < maximumLeadingBoxes && offset < std::min(videoLength, maximumHeaderScan);
         ++boxIndex) {
        if (!video->seek(qint64(offset))) {
            *error = video->errorString().isEmpty()
                         ? QStringLiteral("Unable to seek in Motion Photo video range")
                         : video->errorString();
            return false;
        }

        const QByteArray basic = video->read(8);
        if (basic.size() != 8) {
            *error = video->errorString().isEmpty()
                         ? QStringLiteral("Motion Photo video box header is truncated")
                         : video->errorString();
            return false;
        }

        const auto* bytes = reinterpret_cast<const uchar*>(basic.constData());
        const quint32 boxSize32 = qFromBigEndian<quint32>(bytes);
        const QByteArray type = basic.mid(4, 4);
        const bool isFtyp = type == QByteArray("ftyp", 4);
        const bool isAllowedPreamble = type == QByteArray("free", 4) ||
                                       type == QByteArray("skip", 4) ||
                                       type == QByteArray("wide", 4);

        // Classify an unrecognized payload before trusting its size field.
        // Residual Motion Photo metadata often points at arbitrary trailing
        // bytes; treating those bytes as an ISO BMFF size can otherwise turn
        // a clear "no ftyp" result into a misleading range-overflow error.
        if (!isFtyp && !isAllowedPreamble) {
            *error =
                QStringLiteral("Motion Photo video does not begin with a recognized ftyp preamble");
            return false;
        }

        quint64 boxSize = boxSize32;
        quint64 headerSize = 8;

        if (boxSize32 == 0) {
            *error = QStringLiteral("Motion Photo video uses an open-ended box before ftyp");
            return false;
        }
        if (boxSize32 == 1) {
            const QByteArray extended = video->read(8);
            if (extended.size() != 8) {
                *error = video->errorString().isEmpty()
                             ? QStringLiteral("Motion Photo extended box header is truncated")
                             : video->errorString();
                return false;
            }
            boxSize = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(extended.constData()));
            headerSize = 16;
        }

        if (boxSize < headerSize || boxSize > videoLength - offset) {
            *error = QStringLiteral("Motion Photo video box exceeds the bounded video range");
            return false;
        }
        if (isFtyp) {
            return true;
        }

        // QuickTime files may carry one or more harmless alignment/padding
        // boxes before ftyp. Do not scan through arbitrary media payloads.
        const auto next = checkedByteOffsetAdd(offset, boxSize);
        if (!next) {
            *error = QStringLiteral("Motion Photo video box offsets overflow");
            return false;
        }
        offset = *next;
    }

    *error =
        QStringLiteral("Motion Photo video ftyp box was not found within the bounded header scan");
    return false;
}

std::optional<quint64> verifiedMpvdPayloadLength(const QString& sourcePath, quint64 videoOffset,
                                                 quint64 declaredVideoLength,
                                                 bool allowSamsungSefdTrailer,
                                                 const std::atomic_bool* cancelled, QString* error)
{
    if (videoOffset < 8 || declaredVideoLength > quint64(std::numeric_limits<quint32>::max()) - 8) {
        *error =
            QStringLiteral("HEIC/AVIF Motion Photo mpvd range cannot use an 8-byte box header");
        return std::nullopt;
    }

    ByteRangeDevice header(sourcePath, videoOffset - 8, 8, cancelled);
    if (!header.open(QIODevice::ReadOnly)) {
        *error = header.errorString();
        return std::nullopt;
    }
    const QByteArray bytes = header.read(8);
    if (bytes.size() != 8) {
        *error = header.errorString().isEmpty()
                     ? QStringLiteral("Unable to read Motion Photo mpvd box header")
                     : header.errorString();
        return std::nullopt;
    }
    if (bytes.mid(4, 4) != QByteArray("mpvd", 4)) {
        *error = QStringLiteral("HEIC/AVIF Motion Photo is missing the required mpvd box");
        return std::nullopt;
    }

    const quint64 boxSize =
        quint64(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData())));
    const auto expectedBoxSize = checkedByteOffsetAdd(declaredVideoLength, quint64(8));
    if (!expectedBoxSize) {
        *error = QStringLiteral("Motion Photo mpvd size arithmetic overflowed");
        return std::nullopt;
    }
    if (boxSize == *expectedBoxSize) {
        return declaredVideoLength;
    }

    // Samsung HEIC Motion Photos can put a top-level SEF metadata box after
    // mpvd while still counting that box in the XMP MotionPhoto Item:Length.
    // Accept only the observed, structurally self-verifying layout:
    //   [mpvd header][MP4 payload][sefd box to EOF]
    // The returned playback range excludes sefd and therefore remains a pure
    // ISO-BMFF video byte range. AVIF intentionally stays spec-strict.
    if (!allowSamsungSefdTrailer || boxSize < 8 || boxSize > *expectedBoxSize) {
        *error = QStringLiteral("Motion Photo mpvd box size does not match Item:Length");
        return std::nullopt;
    }

    const quint64 mpvdPayloadLength = boxSize - 8;
    if (mpvdPayloadLength >= declaredVideoLength) {
        *error = QStringLiteral("Samsung Motion Photo mpvd payload does not leave a sefd trailer");
        return std::nullopt;
    }
    const quint64 trailerLength = declaredVideoLength - mpvdPayloadLength;
    if (trailerLength < 8 || trailerLength > quint64(std::numeric_limits<quint32>::max())) {
        *error = QStringLiteral("Samsung Motion Photo sefd trailer length is invalid");
        return std::nullopt;
    }

    const auto trailerOffset = checkedByteOffsetAdd(videoOffset, mpvdPayloadLength);
    if (!trailerOffset) {
        *error = QStringLiteral("Samsung Motion Photo sefd offset overflowed");
        return std::nullopt;
    }

    ByteRangeDevice trailer(sourcePath, *trailerOffset, trailerLength, cancelled);
    if (!trailer.open(QIODevice::ReadOnly)) {
        *error = trailer.errorString();
        return std::nullopt;
    }
    const QByteArray trailerHeader = trailer.read(8);
    if (trailerHeader.size() != 8) {
        *error = trailer.errorString().isEmpty()
                     ? QStringLiteral("Samsung Motion Photo sefd header is truncated")
                     : trailer.errorString();
        return std::nullopt;
    }
    const quint32 sefdSize =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(trailerHeader.constData()));
    if (trailerHeader.mid(4, 4) != QByteArray("sefd", 4) || quint64(sefdSize) != trailerLength) {
        *error = QStringLiteral("Samsung Motion Photo trailing bytes are not one bounded sefd box");
        return std::nullopt;
    }

    return mpvdPayloadLength;
}

AndroidMotionPhotoDetection detectLegacyPixelMicroVideo(const QString& sourcePath,
                                                        const ParsedXmp& parsed,
                                                        const std::atomic_bool* cancelled)
{
    if (!parsed.microVideo.has_value() || *parsed.microVideo != 1) {
        return notMotionPhoto();
    }

    if (parsed.microVideoVersion.has_value() && *parsed.microVideoVersion != 1) {
        return invalid(QStringLiteral("Legacy Pixel MicroVideo version is not supported"));
    }
    if (!parsed.microVideoOffset.has_value() || *parsed.microVideoOffset == 0) {
        return invalid(QStringLiteral("Legacy Pixel MicroVideoOffset is missing or zero"));
    }
    if (parsed.microVideoPresentationTimestampUs.has_value() &&
        *parsed.microVideoPresentationTimestampUs < -1) {
        return invalid(
            QStringLiteral("Legacy Pixel MicroVideo presentation timestamp is below -1"));
    }
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        return invalid(QStringLiteral("Motion Photo detection cancelled"));
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return invalid(source.errorString());
    }
    const QByteArray signature = source.read(2);
    if (signature != QByteArray::fromHex("ffd8")) {
        return invalid(QStringLiteral("Legacy Pixel MicroVideo requires a JPEG source"));
    }
    const qint64 signedFileSize = source.size();
    source.close();
    if (signedFileSize < 0) {
        return invalid(QStringLiteral("Unable to determine Motion Photo source size"));
    }

    const quint64 fileSize = quint64(signedFileSize);
    const quint64 videoLength = *parsed.microVideoOffset;
    if (videoLength >= fileSize) {
        return invalid(
            QStringLiteral("Legacy Pixel MicroVideoOffset exceeds or consumes the source file"));
    }
    const quint64 videoOffset = fileSize - videoLength;
    const auto videoRange = checkedByteRange(fileSize, videoOffset, videoLength);
    if (!videoRange) {
        return invalid(QStringLiteral("Legacy Pixel MicroVideo range is outside the source file"));
    }

    QString error;
    ByteRangeDevice videoDevice(sourcePath, videoOffset, videoLength, cancelled);
    if (!videoDevice.open(QIODevice::ReadOnly)) {
        return invalid(videoDevice.errorString());
    }
    if (!verifyIsoBmffVideoHeader(&videoDevice, videoLength, &error)) {
        return invalid(error);
    }

    // MicroVideoOffset can survive edits that strip or replace the appended
    // movie. Confirm both the referenced media header and a stable EOF before
    // promoting the JPEG to a Motion Photo.
    QFile finalSource(sourcePath);
    if (!finalSource.open(QIODevice::ReadOnly)) {
        return invalid(finalSource.errorString());
    }
    const qint64 finalSize = finalSource.size();
    finalSource.close();
    if (finalSize != signedFileSize) {
        return invalid(QStringLiteral("Motion Photo source size changed during detection"));
    }

    AndroidMotionPhotoV1 metadata;
    metadata.primaryMimeType = QStringLiteral("image/jpeg");
    metadata.videoMimeType = QStringLiteral("video/mp4");
    metadata.primaryImageLength = videoOffset;
    metadata.primaryPadding = 0;
    metadata.videoRange = *videoRange;
    metadata.presentationTimestampUs = parsed.microVideoPresentationTimestampUs;

    AndroidMotionPhotoDetection result;
    result.status = AndroidMotionPhotoStatus::Valid;
    result.metadata = std::move(metadata);
    return result;
}

} // namespace

AndroidMotionPhotoDetection detectAndroidMotionPhotoV1(const QString& sourcePath,
                                                       QByteArrayView xmp,
                                                       const std::atomic_bool* cancelled)
{
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        return invalid(QStringLiteral("Motion Photo detection cancelled"));
    }

    ParsedXmp parsed;
    QString error;
    if (!parseXmp(xmp, &parsed, &error)) {
        return invalid(error);
    }

    // The Container-based Motion Photo 1.0 vocabulary is authoritative when
    // present. Without a modern declaration, a structurally indexed Samsung
    // SEF/SEFT MotionPhoto_Data record is more specific than the legacy Google
    // MicroVideoOffset compatibility vocabulary and therefore gets first chance
    // to define the exact MP4 range. Legacy Pixel remains the final fallback.
    if (!parsed.motionPhoto.has_value()) {
        const AndroidMotionPhotoDetection samsung =
            detectSamsungMotionPhotoJpegSeft(sourcePath, xmp, cancelled);
        if (samsung.status != AndroidMotionPhotoStatus::NotMotionPhoto) {
            return samsung;
        }
        return detectLegacyPixelMicroVideo(sourcePath, parsed, cancelled);
    }

    // Format 1.0 explicitly treats any value other than 1 as non-Motion-Photo.
    if (*parsed.motionPhoto != 1) {
        return notMotionPhoto();
    }

    if (!parsed.version.has_value() || *parsed.version != 1) {
        return invalid(
            QStringLiteral("Motion Photo flag is set but version 1 metadata is missing"));
    }
    if (parsed.presentationTimestampUs.has_value() && *parsed.presentationTimestampUs < -1) {
        return invalid(QStringLiteral("Motion Photo presentation timestamp is below -1"));
    }
    if (!parsed.sawDirectory || parsed.items.size() < 2) {
        return invalid(QStringLiteral("Motion Photo Container directory is missing or incomplete"));
    }

    qsizetype primaryIndex = -1;
    qsizetype motionIndex = -1;
    quint64 secondaryLengthTotal = 0;

    for (qsizetype index = 0; index < qsizetype(parsed.items.size()); ++index) {
        const ContainerItem& item = parsed.items[size_t(index)];
        if (item.semantic == QStringLiteral("Primary")) {
            if (primaryIndex >= 0) {
                return invalid(QStringLiteral("Motion Photo Container has multiple Primary items"));
            }
            primaryIndex = index;
        }
        if (item.semantic == QStringLiteral("MotionPhoto")) {
            if (motionIndex >= 0) {
                return invalid(
                    QStringLiteral("Motion Photo Container has multiple MotionPhoto items"));
            }
            motionIndex = index;
        }

        if (index == 0) {
            continue;
        }
        // Motion Photo 1.0 reserves Padding for the primary item. Some real
        // Google-header Samsung files nevertheless serialize Padding="0" on
        // secondary items. Zero has no layout effect, so tolerate only that
        // compatibility spelling; non-zero secondary padding remains invalid
        // and is never incorporated into range arithmetic.
        if (item.padding.has_value() && *item.padding != 0) {
            return invalid(QStringLiteral(
                "Secondary Motion Photo Container item Padding must be zero when present"));
        }
        if (!item.length.has_value()) {
            return invalid(QStringLiteral("Secondary Motion Photo Container item lacks Length"));
        }
        const auto sum = checkedByteOffsetAdd(secondaryLengthTotal, *item.length);
        if (!sum) {
            return invalid(QStringLiteral("Motion Photo Container item lengths overflow"));
        }
        secondaryLengthTotal = *sum;
    }

    if (primaryIndex != 0) {
        return invalid(QStringLiteral("The first Motion Photo Container item must be Primary"));
    }
    if (motionIndex != qsizetype(parsed.items.size()) - 1) {
        return invalid(QStringLiteral("The MotionPhoto item must be the final Container item"));
    }

    const ContainerItem& primary = parsed.items.front();
    const ContainerItem& video = parsed.items.back();
    if (primary.length.has_value() && *primary.length != 0) {
        return invalid(
            QStringLiteral("Primary Motion Photo item Length must be zero when present"));
    }

    const bool jpegPrimary = primary.mime == QStringLiteral("image/jpeg");
    const bool isobmffPrimary = primary.mime == QStringLiteral("image/heic") ||
                                primary.mime == QStringLiteral("image/avif");
    if (!jpegPrimary && !isobmffPrimary) {
        return invalid(QStringLiteral("Unsupported Android Motion Photo primary MIME type"));
    }
    if (video.mime != QStringLiteral("video/mp4") &&
        video.mime != QStringLiteral("video/quicktime")) {
        return invalid(QStringLiteral("Unsupported Android Motion Photo video MIME type"));
    }
    if (!video.length.has_value() || *video.length == 0) {
        return invalid(QStringLiteral("Motion Photo video item must have a positive Length"));
    }

    const quint64 primaryPadding = primary.padding.value_or(0);
    if (isobmffPrimary && primaryPadding != 8) {
        return invalid(QStringLiteral("HEIC/AVIF Motion Photo primary Padding must be exactly 8"));
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return invalid(source.errorString());
    }
    const qint64 signedFileSize = source.size();
    source.close();
    if (signedFileSize < 0) {
        return invalid(QStringLiteral("Unable to determine Motion Photo source size"));
    }
    const quint64 fileSize = quint64(signedFileSize);

    const auto packedBytes = checkedByteOffsetAdd(primaryPadding, secondaryLengthTotal);
    if (!packedBytes || *packedBytes > fileSize) {
        return invalid(QStringLiteral("Motion Photo Container lengths exceed the source file"));
    }
    const quint64 primaryImageLength = fileSize - *packedBytes;
    if (primaryImageLength == 0) {
        return invalid(QStringLiteral("Motion Photo primary image is empty"));
    }

    const quint64 declaredVideoLength = *video.length;
    if (declaredVideoLength > fileSize) {
        return invalid(QStringLiteral("Motion Photo video Length exceeds the source file"));
    }
    const quint64 videoOffset = fileSize - declaredVideoLength;

    quint64 playbackVideoLength = declaredVideoLength;
    if (isobmffPrimary) {
        const auto verifiedLength = verifiedMpvdPayloadLength(
            sourcePath, videoOffset, declaredVideoLength,
            primary.mime == QStringLiteral("image/heic"), cancelled, &error);
        if (!verifiedLength) {
            return invalid(error);
        }
        playbackVideoLength = *verifiedLength;
    }

    const auto videoRange = checkedByteRange(fileSize, videoOffset, playbackVideoLength);
    if (!videoRange) {
        return invalid(QStringLiteral("Motion Photo video range is outside the source file"));
    }

    ByteRangeDevice videoDevice(sourcePath, videoOffset, playbackVideoLength, cancelled);
    if (!videoDevice.open(QIODevice::ReadOnly)) {
        return invalid(videoDevice.errorString());
    }
    if (!verifyIsoBmffVideoHeader(&videoDevice, playbackVideoLength, &error)) {
        return invalid(error);
    }

    // Re-check EOF after the bounded read so a concurrent append cannot make a
    // stale Item:Length look like a valid end-of-file MotionPhoto item.
    QFile finalSource(sourcePath);
    if (!finalSource.open(QIODevice::ReadOnly)) {
        return invalid(finalSource.errorString());
    }
    const qint64 finalSize = finalSource.size();
    finalSource.close();
    if (finalSize != signedFileSize) {
        return invalid(QStringLiteral("Motion Photo source size changed during detection"));
    }

    CheckedByteRange normalizedVideoRange = *videoRange;
    if (jpegPrimary && video.mime == QStringLiteral("video/mp4")) {
        // Some newer Samsung JPEGs publish a valid modern Container item whose
        // MotionPhoto Length reaches EOF and therefore includes the trailing
        // SEFH directory + SEFT footer. If the independently validated Samsung
        // record points to the exact same video start, it is safe to narrow the
        // playback range to that indexed payload. Never move the modern start or
        // use SEFT to expand the modern range; conflicting layouts keep the
        // modern Container result unchanged.
        const AndroidMotionPhotoDetection samsung =
            detectSamsungMotionPhotoJpegSeft(sourcePath, xmp, cancelled);
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            return invalid(QStringLiteral("Motion Photo detection cancelled"));
        }
        if (samsung.status == AndroidMotionPhotoStatus::Valid && samsung.metadata) {
            const CheckedByteRange& samsungRange = samsung.metadata->videoRange;
            const auto modernEnd =
                checkedByteOffsetAdd(normalizedVideoRange.offset, normalizedVideoRange.length);
            const auto samsungEnd = checkedByteOffsetAdd(samsungRange.offset, samsungRange.length);
            if (modernEnd && samsungEnd && samsungRange.offset == normalizedVideoRange.offset &&
                samsungRange.length < normalizedVideoRange.length && *samsungEnd < *modernEnd) {
                normalizedVideoRange = samsungRange;
            }
        }
    }

    AndroidMotionPhotoV1 metadata;
    metadata.primaryMimeType = primary.mime;
    metadata.videoMimeType = video.mime;
    metadata.primaryImageLength = primaryImageLength;
    metadata.primaryPadding = primaryPadding;
    metadata.videoRange = normalizedVideoRange;
    metadata.presentationTimestampUs = parsed.presentationTimestampUs;

    AndroidMotionPhotoDetection result;
    result.status = AndroidMotionPhotoStatus::Valid;
    result.metadata = std::move(metadata);
    return result;
}

AndroidMotionPhotoDetection detectSamsungMotionPhotoJpegSeft(const QString& sourcePath,
                                                             QByteArrayView xmp,
                                                             const std::atomic_bool* cancelled)
{
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        return invalid(QStringLiteral("Motion Photo detection cancelled"));
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        return invalid(source.errorString());
    }
    const QByteArray signature = source.read(2);
    const qint64 signedFileSize = source.size();
    source.close();
    if (signature != QByteArray::fromHex("ffd8")) {
        return notMotionPhoto();
    }
    if (signedFileSize < 8) {
        return notMotionPhoto();
    }

    const quint64 fileSize = quint64(signedFileSize);
    ByteRangeDevice tail(sourcePath, fileSize - 8, 8, cancelled);
    if (!tail.open(QIODevice::ReadOnly)) {
        return invalid(tail.errorString());
    }
    const QByteArray tailBytes = tail.read(8);
    if (tailBytes.size() != 8) {
        return invalid(tail.errorString().isEmpty()
                           ? QStringLiteral("Samsung SEFT tail is truncated")
                           : tail.errorString());
    }
    if (tailBytes.mid(4, 4) != QByteArrayLiteral("SEFT")) {
        return notMotionPhoto();
    }

    const quint64 headerLength =
        quint64(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(tailBytes.constData())));
    if (headerLength < 12 || headerLength > maximumSamsungSeftHeaderBytes ||
        headerLength > fileSize - 8) {
        return invalid(QStringLiteral("Samsung SEFT directory length is invalid"));
    }
    const quint64 headerStart = fileSize - 8 - headerLength;

    ByteRangeDevice headerDevice(sourcePath, headerStart, headerLength, cancelled);
    if (!headerDevice.open(QIODevice::ReadOnly)) {
        return invalid(headerDevice.errorString());
    }
    const QByteArray header = headerDevice.read(qint64(headerLength));
    if (quint64(header.size()) != headerLength) {
        return invalid(QStringLiteral("Samsung SEFH directory is truncated"));
    }
    if (header.left(4) != QByteArrayLiteral("SEFH")) {
        return invalid(QStringLiteral("Samsung SEFT tail does not point to SEFH"));
    }

    const quint32 recordCount =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 8));
    if (recordCount == 0 || recordCount > maximumSamsungSeftRecords) {
        return invalid(QStringLiteral("Samsung SEFH record count is invalid"));
    }
    const quint64 expectedHeaderLength = 12 + quint64(recordCount) * 12;
    if (expectedHeaderLength != headerLength) {
        return invalid(QStringLiteral("Samsung SEFH directory size does not match record count"));
    }

    bool foundMotionRecord = false;
    quint16 motionPadding = 0;
    quint32 motionOffsetBack = 0;
    quint32 motionContentLength = 0;
    for (quint32 index = 0; index < recordCount; ++index) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            return invalid(QStringLiteral("Motion Photo detection cancelled"));
        }
        const char* entry = header.constData() + 12 + qsizetype(index) * 12;
        const quint16 padding = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(entry));
        const quint16 type = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(entry + 2));
        const quint32 offsetBack =
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(entry + 4));
        const quint32 contentLength =
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(entry + 8));
        if (type != samsungMotionPhotoDataType) {
            continue;
        }
        if (foundMotionRecord) {
            return invalid(QStringLiteral("Samsung SEFT has multiple MotionPhoto_Data records"));
        }
        foundMotionRecord = true;
        motionPadding = padding;
        motionOffsetBack = offsetBack;
        motionContentLength = contentLength;
    }

    // SEF is also used for many Samsung still-only metadata records. Its
    // presence alone must never turn a normal JPEG into a motion photo.
    if (!foundMotionRecord) {
        return notMotionPhoto();
    }
    if (motionPadding != 0) {
        return invalid(QStringLiteral("Samsung MotionPhoto_Data index padding is not zero"));
    }
    if (motionOffsetBack == 0 || quint64(motionOffsetBack) > headerStart) {
        return invalid(QStringLiteral("Samsung MotionPhoto_Data offset is outside the file"));
    }
    if (quint64(motionContentLength) <= samsungMotionPhotoContentHeaderBytes) {
        return invalid(QStringLiteral("Samsung MotionPhoto_Data record is too short"));
    }

    const quint64 contentStart = headerStart - quint64(motionOffsetBack);
    const auto contentEnd = checkedByteOffsetAdd(contentStart, quint64(motionContentLength));
    if (!contentEnd || *contentEnd > headerStart) {
        return invalid(
            QStringLiteral("Samsung MotionPhoto_Data record overlaps the SEFH directory"));
    }

    ByteRangeDevice contentHeader(sourcePath, contentStart, samsungMotionPhotoContentHeaderBytes,
                                  cancelled);
    if (!contentHeader.open(QIODevice::ReadOnly)) {
        return invalid(contentHeader.errorString());
    }
    const QByteArray content = contentHeader.read(qint64(samsungMotionPhotoContentHeaderBytes));
    if (quint64(content.size()) != samsungMotionPhotoContentHeaderBytes) {
        return invalid(QStringLiteral("Samsung MotionPhoto_Data content header is truncated"));
    }

    const quint16 contentPadding =
        qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData()));
    const quint16 contentType =
        qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + 2));
    const quint32 nameLength =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(content.constData() + 4));
    if (contentPadding != 0 || contentType != samsungMotionPhotoDataType ||
        nameLength != quint32(samsungMotionPhotoDataNameSize) ||
        content.mid(8, samsungMotionPhotoDataNameSize) !=
            QByteArray(samsungMotionPhotoDataName, samsungMotionPhotoDataNameSize)) {
        return invalid(QStringLiteral("Samsung MotionPhoto_Data content header is inconsistent"));
    }

    const auto videoOffset =
        checkedByteOffsetAdd(contentStart, samsungMotionPhotoContentHeaderBytes);
    if (!videoOffset) {
        return invalid(QStringLiteral("Samsung MotionPhoto_Data video offset overflowed"));
    }
    const quint64 videoLength = quint64(motionContentLength) - samsungMotionPhotoContentHeaderBytes;
    const auto videoRange = checkedByteRange(fileSize, *videoOffset, videoLength);
    if (!videoRange) {
        return invalid(
            QStringLiteral("Samsung MotionPhoto_Data video range is outside the record"));
    }

    QString videoError;
    ByteRangeDevice videoDevice(sourcePath, videoRange->offset, videoRange->length, cancelled);
    if (!videoDevice.open(QIODevice::ReadOnly)) {
        return invalid(videoDevice.errorString());
    }
    if (!verifyIsoBmffVideoHeader(&videoDevice, videoRange->length, &videoError)) {
        return invalid(videoError);
    }

    QFile finalSource(sourcePath);
    if (!finalSource.open(QIODevice::ReadOnly)) {
        return invalid(finalSource.errorString());
    }
    const qint64 finalSize = finalSource.size();
    finalSource.close();
    if (finalSize != signedFileSize) {
        return invalid(QStringLiteral("Motion Photo source size changed during detection"));
    }

    std::optional<qint64> presentationTimestampUs;
    if (!xmp.isEmpty()) {
        ParsedXmp parsed;
        QString ignoredError;
        if (parseXmp(xmp, &parsed, &ignoredError)) {
            const auto candidate = parsed.presentationTimestampUs.has_value()
                                       ? parsed.presentationTimestampUs
                                       : parsed.microVideoPresentationTimestampUs;
            if (candidate.has_value() && *candidate >= -1) {
                presentationTimestampUs = candidate;
            }
        }
    }

    AndroidMotionPhotoV1 metadata;
    metadata.primaryMimeType = QStringLiteral("image/jpeg");
    metadata.videoMimeType = QStringLiteral("video/mp4");
    metadata.primaryImageLength = videoRange->offset;
    metadata.primaryPadding = 0;
    metadata.videoRange = *videoRange;
    metadata.presentationTimestampUs = presentationTimestampUs;

    AndroidMotionPhotoDetection result;
    result.status = AndroidMotionPhotoStatus::Valid;
    result.metadata = std::move(metadata);
    return result;
}

} // namespace Licasa
