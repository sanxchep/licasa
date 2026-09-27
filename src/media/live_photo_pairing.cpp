#include "media/live_photo_pairing.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace Licasa {
namespace {

constexpr quint64 maximumJpegMetadataBytes = 1024 * 1024;
constexpr int maximumJpegSegments = 512;
constexpr int maximumTiffEntries = 512;
constexpr int maximumAppleEntries = 256;
constexpr int maximumQuickTimeBoxes = 1024;
constexpr quint64 maximumQuickTimeKeyBytes = 256 * 1024;
constexpr quint64 maximumContentIdentifierBytes = 256;
constexpr char exifSignature[] = "Exif\0\0";
constexpr char appleSignature[] = "Apple iOS\0";
constexpr char contentIdentifierKey[] = "com.apple.quicktime.content.identifier";

constexpr quint32 fourCc(char a, char b, char c, char d) noexcept
{
    return (quint32(quint8(a)) << 24) | (quint32(quint8(b)) << 16) | (quint32(quint8(c)) << 8) |
           quint32(quint8(d));
}

bool isCancelled(const std::atomic_bool* flag)
{
    return flag && flag->load(std::memory_order_relaxed);
}

bool readExactAt(QFile& file, quint64 offset, char* destination, qsizetype size,
                 const std::atomic_bool* cancelFlag)
{
    if (size < 0 || isCancelled(cancelFlag) ||
        offset > quint64(std::numeric_limits<qint64>::max()) ||
        quint64(size) > quint64(std::numeric_limits<qint64>::max()) - offset) {
        return false;
    }
    if (!file.seek(qint64(offset))) {
        return false;
    }
    qsizetype done = 0;
    while (done < size) {
        if (isCancelled(cancelFlag)) {
            return false;
        }
        const qint64 got = file.read(destination + done, size - done);
        if (got <= 0) {
            return false;
        }
        done += qsizetype(got);
    }
    return true;
}

struct TextProbe {
    enum class State { Absent, Found, Invalid, Cancelled };
    State state = State::Absent;
    QString value;
    QString error;
};

enum class ByteOrder { Little, Big };

std::optional<quint16> read16(const QByteArray& bytes, quint64 offset, ByteOrder order)
{
    if (offset > quint64(bytes.size()) || quint64(bytes.size()) - offset < 2) {
        return std::nullopt;
    }
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + qsizetype(offset));
    return order == ByteOrder::Big ? qFromBigEndian<quint16>(data)
                                   : qFromLittleEndian<quint16>(data);
}

std::optional<quint32> read32(const QByteArray& bytes, quint64 offset, ByteOrder order)
{
    if (offset > quint64(bytes.size()) || quint64(bytes.size()) - offset < 4) {
        return std::nullopt;
    }
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + qsizetype(offset));
    return order == ByteOrder::Big ? qFromBigEndian<quint32>(data)
                                   : qFromLittleEndian<quint32>(data);
}

quint64 tiffTypeSize(quint16 type)
{
    switch (type) {
    case 1: // BYTE
    case 2: // ASCII
    case 7: // UNDEFINED
        return 1;
    case 3: // SHORT
        return 2;
    case 4: // LONG
    case 9: // SLONG
        return 4;
    case 5:  // RATIONAL
    case 10: // SRATIONAL
        return 8;
    default:
        return 0;
    }
}

std::optional<QByteArray> fieldBytes(const QByteArray& storage, quint64 entryOffset, quint16 type,
                                     quint32 count, ByteOrder order, quint64 offsetBase)
{
    const quint64 unit = tiffTypeSize(type);
    if (unit == 0 || count == 0 || quint64(count) > maximumJpegMetadataBytes / unit) {
        return std::nullopt;
    }
    const quint64 byteCount = unit * quint64(count);
    if (byteCount > maximumJpegMetadataBytes) {
        return std::nullopt;
    }

    quint64 valueOffset = entryOffset + 8;
    if (byteCount > 4) {
        const auto relative = read32(storage, entryOffset + 8, order);
        if (!relative || quint64(*relative) > std::numeric_limits<quint64>::max() - offsetBase) {
            return std::nullopt;
        }
        valueOffset = offsetBase + quint64(*relative);
    }
    if (valueOffset > quint64(storage.size()) ||
        byteCount > quint64(storage.size()) - valueOffset ||
        byteCount > quint64(std::numeric_limits<qsizetype>::max())) {
        return std::nullopt;
    }
    return storage.mid(qsizetype(valueOffset), qsizetype(byteCount));
}

std::optional<QByteArray> findIfdField(const QByteArray& storage, quint64 ifdOffset,
                                       quint16 wantedTag, ByteOrder order, quint64 offsetBase,
                                       quint16* foundType = nullptr)
{
    const auto count = read16(storage, ifdOffset, order);
    if (!count || *count > maximumTiffEntries) {
        return std::nullopt;
    }
    const quint64 entriesStart = ifdOffset + 2;
    const quint64 entriesBytes = quint64(*count) * 12;
    if (entriesStart > quint64(storage.size()) ||
        entriesBytes > quint64(storage.size()) - entriesStart) {
        return std::nullopt;
    }

    for (quint16 index = 0; index < *count; ++index) {
        const quint64 entry = entriesStart + quint64(index) * 12;
        const auto tag = read16(storage, entry, order);
        const auto type = read16(storage, entry + 2, order);
        const auto itemCount = read32(storage, entry + 4, order);
        if (!tag || !type || !itemCount) {
            return std::nullopt;
        }
        if (*tag != wantedTag) {
            continue;
        }
        if (foundType) {
            *foundType = *type;
        }
        return fieldBytes(storage, entry, *type, *itemCount, order, offsetBase);
    }
    return QByteArray{};
}

TextProbe appleIdentifierFromMakerNote(const QByteArray& maker)
{
    TextProbe result;
    if (maker.size() < 16 || !maker.startsWith(QByteArray(appleSignature, 10))) {
        return result;
    }

    ByteOrder order;
    if (maker.mid(12, 2) == QByteArrayLiteral("MM")) {
        order = ByteOrder::Big;
    } else if (maker.mid(12, 2) == QByteArrayLiteral("II")) {
        order = ByteOrder::Little;
    } else {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Apple MakerNote has an invalid byte-order marker");
        return result;
    }

    const auto count = read16(maker, 14, order);
    if (!count || *count > maximumAppleEntries) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Apple MakerNote directory is truncated or too large");
        return result;
    }
    const quint64 entriesStart = 16;
    const quint64 entriesBytes = quint64(*count) * 12;
    if (entriesStart > quint64(maker.size()) ||
        entriesBytes > quint64(maker.size()) - entriesStart) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Apple MakerNote directory exceeds its metadata block");
        return result;
    }

    for (quint16 index = 0; index < *count; ++index) {
        const quint64 entry = entriesStart + quint64(index) * 12;
        const auto tag = read16(maker, entry, order);
        const auto type = read16(maker, entry + 2, order);
        const auto itemCount = read32(maker, entry + 4, order);
        if (!tag || !type || !itemCount) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("Apple MakerNote entry is truncated");
            return result;
        }
        if (*tag != 0x0011) {
            continue;
        }
        if ((*type != 2 && *type != 1 && *type != 7) || *itemCount == 0 ||
            *itemCount > maximumContentIdentifierBytes) {
            result.state = TextProbe::State::Invalid;
            result.error =
                QStringLiteral("Apple content identifier has an invalid field type or size");
            return result;
        }
        const auto valueBytes = fieldBytes(maker, entry, *type, *itemCount, order, 0);
        if (!valueBytes) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("Apple content identifier exceeds the MakerNote block");
            return result;
        }
        QByteArray value = *valueBytes;
        while (!value.isEmpty() && value.endsWith('\0')) {
            value.chop(1);
        }
        if (value.isEmpty() || value.size() > qsizetype(maximumContentIdentifierBytes) ||
            std::any_of(value.cbegin(), value.cend(), [](char ch) {
                const uchar byte = uchar(ch);
                return byte < 0x20 || byte > 0x7e;
            })) {
            result.state = TextProbe::State::Invalid;
            result.error =
                QStringLiteral("Apple content identifier is not a bounded printable string");
            return result;
        }
        result.state = TextProbe::State::Found;
        result.value = QString::fromLatin1(value);
        return result;
    }
    return result;
}

TextProbe appleIdentifierFromExif(const QByteArray& exif)
{
    TextProbe result;
    if (exif.size() < 8) {
        return result;
    }
    ByteOrder order;
    if (exif.startsWith(QByteArrayLiteral("MM"))) {
        order = ByteOrder::Big;
    } else if (exif.startsWith(QByteArrayLiteral("II"))) {
        order = ByteOrder::Little;
    } else {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("EXIF TIFF header has an invalid byte order");
        return result;
    }
    const auto magic = read16(exif, 2, order);
    const auto firstIfd = read32(exif, 4, order);
    if (!magic || *magic != 42 || !firstIfd) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("EXIF TIFF header is invalid");
        return result;
    }

    quint16 pointerType = 0;
    const auto exifPointerBytes = findIfdField(exif, *firstIfd, 0x8769, order, 0, &pointerType);
    if (!exifPointerBytes) {
        return TextProbe{TextProbe::State::Invalid, {}, QStringLiteral("EXIF IFD0 is malformed")};
    }
    if (exifPointerBytes->isEmpty()) {
        return result;
    }
    if (pointerType != 4 || exifPointerBytes->size() != 4) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("EXIF SubIFD pointer has an invalid type");
        return result;
    }
    const auto exifIfd = read32(*exifPointerBytes, 0, order);
    if (!exifIfd) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("EXIF SubIFD pointer is truncated");
        return result;
    }

    quint16 makerType = 0;
    const auto maker = findIfdField(exif, *exifIfd, 0x927c, order, 0, &makerType);
    if (!maker) {
        return TextProbe{TextProbe::State::Invalid, {}, QStringLiteral("EXIF SubIFD is malformed")};
    }
    if (maker->isEmpty()) {
        return result;
    }
    if (makerType != 7) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("EXIF MakerNote has an unexpected type");
        return result;
    }
    return appleIdentifierFromMakerNote(*maker);
}

TextProbe jpegAppleIdentifier(const QString& path, const std::atomic_bool* cancelFlag)
{
    TextProbe result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Unable to open JPEG for Apple pairing metadata");
        return result;
    }
    char soi[2]{};
    if (!readExactAt(file, 0, soi, 2, cancelFlag)) {
        result.state =
            isCancelled(cancelFlag) ? TextProbe::State::Cancelled : TextProbe::State::Invalid;
        result.error = QStringLiteral("JPEG is truncated before its SOI marker");
        return result;
    }
    if (uchar(soi[0]) != 0xff || uchar(soi[1]) != 0xd8) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Apple pairing candidate is not a JPEG");
        return result;
    }

    quint64 position = 2;
    quint64 scanned = 2;
    for (int segment = 0; segment < maximumJpegSegments && scanned < maximumJpegMetadataBytes;
         ++segment) {
        if (isCancelled(cancelFlag)) {
            result.state = TextProbe::State::Cancelled;
            return result;
        }
        char markerPrefix = 0;
        if (!readExactAt(file, position, &markerPrefix, 1, cancelFlag)) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("JPEG metadata ended before SOS/EOI");
            return result;
        }
        ++position;
        ++scanned;
        if (uchar(markerPrefix) != 0xff) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("JPEG metadata contains bytes outside a marker segment");
            return result;
        }
        char markerByte = 0;
        do {
            if (!readExactAt(file, position, &markerByte, 1, cancelFlag)) {
                result.state = TextProbe::State::Invalid;
                result.error = QStringLiteral("JPEG marker is truncated");
                return result;
            }
            ++position;
            ++scanned;
        } while (uchar(markerByte) == 0xff && scanned < maximumJpegMetadataBytes);

        const uchar marker = uchar(markerByte);
        if (marker == 0xda || marker == 0xd9) {
            return result;
        }
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }

        char lengthBytes[2]{};
        if (!readExactAt(file, position, lengthBytes, 2, cancelFlag)) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("JPEG segment length is truncated");
            return result;
        }
        position += 2;
        scanned += 2;
        const quint16 declared =
            qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(lengthBytes));
        if (declared < 2) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("JPEG segment declares an invalid length");
            return result;
        }
        const quint64 payloadLength = quint64(declared) - 2;
        if (payloadLength >
            maximumJpegMetadataBytes - std::min(scanned, maximumJpegMetadataBytes)) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("JPEG metadata exceeds the 1 MiB Apple pairing limit");
            return result;
        }
        const qint64 size = file.size();
        if (size < 0 || position > quint64(size) || payloadLength > quint64(size) - position) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("JPEG metadata segment exceeds the source file");
            return result;
        }

        if (marker == 0xe1 && payloadLength >= 6) {
            QByteArray payload(qsizetype(payloadLength), Qt::Uninitialized);
            if (!readExactAt(file, position, payload.data(), payload.size(), cancelFlag)) {
                result.state = isCancelled(cancelFlag) ? TextProbe::State::Cancelled
                                                       : TextProbe::State::Invalid;
                result.error = QStringLiteral("JPEG APP1 metadata is truncated");
                return result;
            }
            if (payload.startsWith(QByteArray(exifSignature, 6))) {
                return appleIdentifierFromExif(payload.mid(6));
            }
        }
        position += payloadLength;
        scanned += payloadLength;
    }

    if (scanned >= maximumJpegMetadataBytes) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("JPEG metadata exceeds the 1 MiB Apple pairing limit");
    } else {
        result.state = TextProbe::State::Invalid;
        result.error =
            QStringLiteral("JPEG metadata exceeds the Apple pairing segment-count limit");
    }
    return result;
}

struct IsoBox {
    quint64 start = 0;
    quint64 payload = 0;
    quint64 end = 0;
    quint32 type = 0;
};

std::optional<IsoBox> readBox(QFile& file, quint64 start, quint64 parentEnd,
                              const std::atomic_bool* cancelFlag)
{
    if (start > parentEnd || parentEnd - start < 8 || isCancelled(cancelFlag)) {
        return std::nullopt;
    }
    std::array<char, 16> header{};
    if (!readExactAt(file, start, header.data(), 8, cancelFlag)) {
        return std::nullopt;
    }
    const auto* bytes = reinterpret_cast<const uchar*>(header.data());
    const quint32 shortSize = qFromBigEndian<quint32>(bytes);
    const quint32 type = qFromBigEndian<quint32>(bytes + 4);
    quint64 headerSize = 8;
    quint64 boxSize = shortSize;
    if (shortSize == 1) {
        if (parentEnd - start < 16 ||
            !readExactAt(file, start + 8, header.data() + 8, 8, cancelFlag)) {
            return std::nullopt;
        }
        boxSize = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(header.data() + 8));
        headerSize = 16;
    } else if (shortSize == 0) {
        boxSize = parentEnd - start;
    }
    if (boxSize < headerSize || boxSize > parentEnd - start) {
        return std::nullopt;
    }
    return IsoBox{start, start + headerSize, start + boxSize, type};
}

struct MetaBoxes {
    std::optional<IsoBox> keys;
    std::optional<IsoBox> ilst;
};

bool collectMetaBoxChildren(QFile& file, quint64 position, quint64 end, MetaBoxes* result,
                            const std::atomic_bool* cancelFlag)
{
    if (!result || position > end) {
        return false;
    }

    MetaBoxes parsed;
    int boxes = 0;
    while (position < end && boxes++ < maximumQuickTimeBoxes) {
        const auto box = readBox(file, position, end, cancelFlag);
        if (!box || box->end <= position) {
            return false;
        }
        if (box->type == fourCc('k', 'e', 'y', 's')) {
            parsed.keys = box;
        } else if (box->type == fourCc('i', 'l', 's', 't')) {
            parsed.ilst = box;
        }
        position = box->end;
    }
    if (position != end || boxes > maximumQuickTimeBoxes) {
        return false;
    }
    *result = parsed;
    return true;
}

bool isQuickTimeMdtaMeta(QFile& file, const IsoBox& meta, const std::atomic_bool* cancelFlag)
{
    if (meta.end - meta.payload < 20) {
        return false;
    }
    const auto handler = readBox(file, meta.payload, meta.end, cancelFlag);
    if (!handler || handler->type != fourCc('h', 'd', 'l', 'r') || handler->end <= meta.payload ||
        handler->end - handler->payload < 12) {
        return false;
    }

    std::array<char, 12> payload{};
    if (!readExactAt(file, handler->payload, payload.data(), payload.size(), cancelFlag)) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const uchar*>(payload.data());
    return qFromBigEndian<quint32>(bytes + 8) == fourCc('m', 'd', 't', 'a');
}

bool collectMetaBoxes(QFile& file, const IsoBox& meta, MetaBoxes* result,
                      const std::atomic_bool* cancelFlag)
{
    if (!result || meta.end - meta.payload < 4) {
        return false;
    }

    MetaBoxes fullBox;
    if (collectMetaBoxChildren(file, meta.payload + 4, meta.end, &fullBox, cancelFlag)) {
        *result = fullBox;
        return true;
    }
    if (isCancelled(cancelFlag) || !isQuickTimeMdtaMeta(file, meta, cancelFlag)) {
        return false;
    }

    MetaBoxes quickTime;
    if (!collectMetaBoxChildren(file, meta.payload, meta.end, &quickTime, cancelFlag) ||
        !quickTime.keys || !quickTime.ilst) {
        return false;
    }
    *result = quickTime;
    return true;
}

std::optional<MetaBoxes> findMovieMetadataBoxes(QFile& file, quint64 fileSize,
                                                const std::atomic_bool* cancelFlag)
{
    quint64 top = 0;
    int topBoxes = 0;
    while (top < fileSize && topBoxes++ < maximumQuickTimeBoxes) {
        const auto box = readBox(file, top, fileSize, cancelFlag);
        if (!box || box->end <= top) {
            return std::nullopt;
        }
        if (box->type == fourCc('m', 'o', 'o', 'v')) {
            quint64 childPos = box->payload;
            int childCount = 0;
            while (childPos < box->end && childCount++ < maximumQuickTimeBoxes) {
                const auto child = readBox(file, childPos, box->end, cancelFlag);
                if (!child || child->end <= childPos) {
                    return std::nullopt;
                }
                if (child->type == fourCc('m', 'e', 't', 'a')) {
                    MetaBoxes result;
                    if (!collectMetaBoxes(file, *child, &result, cancelFlag)) {
                        return std::nullopt;
                    }
                    if (result.keys && result.ilst) {
                        return result;
                    }
                } else if (child->type == fourCc('u', 'd', 't', 'a')) {
                    quint64 udtaPos = child->payload;
                    int udtaCount = 0;
                    while (udtaPos < child->end && udtaCount++ < maximumQuickTimeBoxes) {
                        const auto nested = readBox(file, udtaPos, child->end, cancelFlag);
                        if (!nested || nested->end <= udtaPos) {
                            return std::nullopt;
                        }
                        if (nested->type == fourCc('m', 'e', 't', 'a')) {
                            MetaBoxes result;
                            if (!collectMetaBoxes(file, *nested, &result, cancelFlag)) {
                                return std::nullopt;
                            }
                            if (result.keys && result.ilst) {
                                return result;
                            }
                        }
                        udtaPos = nested->end;
                    }
                    if (udtaPos != child->end) {
                        return std::nullopt;
                    }
                }
                childPos = child->end;
            }
            if (childPos != box->end) {
                return std::nullopt;
            }
        }
        top = box->end;
    }
    if (top != fileSize) {
        return std::nullopt;
    }
    return MetaBoxes{};
}

std::optional<quint32> contentIdentifierKeyIndex(QFile& file, const IsoBox& keys,
                                                 const std::atomic_bool* cancelFlag)
{
    if (keys.end - keys.payload < 8) {
        return std::nullopt;
    }
    std::array<char, 8> header{};
    if (!readExactAt(file, keys.payload, header.data(), 8, cancelFlag)) {
        return std::nullopt;
    }
    const quint32 count =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.data() + 4));
    if (count > maximumQuickTimeBoxes) {
        return std::nullopt;
    }
    if (count == 0) {
        return quint32(0);
    }
    quint64 position = keys.payload + 8;
    quint64 keyBytes = 0;
    for (quint32 index = 1; index <= count; ++index) {
        std::array<char, 8> entryHeader{};
        if (position > keys.end || keys.end - position < 8 ||
            !readExactAt(file, position, entryHeader.data(), 8, cancelFlag)) {
            return std::nullopt;
        }
        const auto* bytes = reinterpret_cast<const uchar*>(entryHeader.data());
        const quint32 size = qFromBigEndian<quint32>(bytes);
        const quint32 nameSpace = qFromBigEndian<quint32>(bytes + 4);
        if (size < 8 || quint64(size) > keys.end - position) {
            return std::nullopt;
        }
        const quint64 valueSize = quint64(size) - 8;
        if (valueSize > maximumQuickTimeKeyBytes - std::min(keyBytes, maximumQuickTimeKeyBytes)) {
            return std::nullopt;
        }
        keyBytes += valueSize;
        if (nameSpace == fourCc('m', 'd', 't', 'a') &&
            valueSize == sizeof(contentIdentifierKey) - 1) {
            QByteArray key(qsizetype(valueSize), Qt::Uninitialized);
            if (!readExactAt(file, position + 8, key.data(), key.size(), cancelFlag)) {
                return std::nullopt;
            }
            if (key == QByteArray(contentIdentifierKey, int(sizeof(contentIdentifierKey) - 1))) {
                return index;
            }
        }
        position += size;
    }
    return quint32(0);
}

TextProbe movieContentIdentifier(const QString& path, const std::atomic_bool* cancelFlag,
                                 ExternalFileIdentity* identityOut = nullptr)
{
    TextProbe result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Unable to open paired MOV candidate");
        return result;
    }
    QString identityError;
    const auto identityBefore = externalFileIdentity(file, &identityError);
    if (!identityBefore) {
        result.state = TextProbe::State::Invalid;
        result.error = identityError;
        return result;
    }
    const qint64 signedSize = file.size();
    if (signedSize < 8) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Paired MOV candidate is truncated");
        return result;
    }
    const auto meta = findMovieMetadataBoxes(file, quint64(signedSize), cancelFlag);
    if (isCancelled(cancelFlag)) {
        result.state = TextProbe::State::Cancelled;
        return result;
    }
    if (!meta) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Paired MOV contains malformed ISO-BMFF metadata boxes");
        return result;
    }
    if (!meta->keys || !meta->ilst) {
        return result;
    }

    const auto keyIndex = contentIdentifierKeyIndex(file, *meta->keys, cancelFlag);
    if (!keyIndex) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Paired MOV keys metadata is malformed");
        return result;
    }
    if (*keyIndex == 0) {
        return result;
    }

    quint64 position = meta->ilst->payload;
    int boxes = 0;
    while (position < meta->ilst->end && boxes++ < maximumQuickTimeBoxes) {
        const auto item = readBox(file, position, meta->ilst->end, cancelFlag);
        if (!item || item->end <= position) {
            result.state = TextProbe::State::Invalid;
            result.error = QStringLiteral("Paired MOV item metadata is malformed");
            return result;
        }
        if (item->type == *keyIndex) {
            quint64 childPos = item->payload;
            int childCount = 0;
            while (childPos < item->end && childCount++ < maximumQuickTimeBoxes) {
                const auto data = readBox(file, childPos, item->end, cancelFlag);
                if (!data || data->end <= childPos) {
                    result.state = TextProbe::State::Invalid;
                    result.error =
                        QStringLiteral("Paired MOV content identifier data is malformed");
                    return result;
                }
                if (data->type == fourCc('d', 'a', 't', 'a')) {
                    if (data->end - data->payload < 8 ||
                        data->end - data->payload - 8 > maximumContentIdentifierBytes) {
                        result.state = TextProbe::State::Invalid;
                        result.error =
                            QStringLiteral("Paired MOV content identifier has an invalid size");
                        return result;
                    }
                    std::array<char, 8> dataHeader{};
                    if (!readExactAt(file, data->payload, dataHeader.data(), dataHeader.size(),
                                     cancelFlag)) {
                        result.state = TextProbe::State::Invalid;
                        result.error = QStringLiteral(
                            "Paired MOV content identifier data header is truncated");
                        return result;
                    }
                    const quint32 valueType =
                        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(dataHeader.data()));
                    if (valueType != 1) {
                        result.state = TextProbe::State::Invalid;
                        result.error =
                            QStringLiteral("Paired MOV content identifier is not UTF-8 metadata");
                        return result;
                    }
                    const quint64 valueSize = data->end - data->payload - 8;
                    QByteArray value(qsizetype(valueSize), Qt::Uninitialized);
                    if (valueSize > 0 && !readExactAt(file, data->payload + 8, value.data(),
                                                      value.size(), cancelFlag)) {
                        result.state = TextProbe::State::Invalid;
                        result.error = QStringLiteral("Paired MOV content identifier is truncated");
                        return result;
                    }
                    while (!value.isEmpty() && value.endsWith('\0')) {
                        value.chop(1);
                    }
                    const QString decoded = QString::fromUtf8(value);
                    if (value.isEmpty() || decoded.toUtf8() != value) {
                        result.state = TextProbe::State::Invalid;
                        result.error =
                            QStringLiteral("Paired MOV content identifier is not valid UTF-8");
                        return result;
                    }
                    QString finalIdentityError;
                    const auto identityAfter = externalFileIdentity(file, &finalIdentityError);
                    if (!identityAfter) {
                        result.state = TextProbe::State::Invalid;
                        result.error = finalIdentityError;
                        return result;
                    }
                    if (*identityAfter != *identityBefore) {
                        result.state = TextProbe::State::Invalid;
                        result.error = QStringLiteral(
                            "Paired MOV changed while its pairing metadata was read");
                        return result;
                    }
                    if (identityOut) {
                        *identityOut = *identityAfter;
                    }
                    result.state = TextProbe::State::Found;
                    result.value = decoded;
                    return result;
                }
                childPos = data->end;
            }
            if (childPos != item->end) {
                result.state = TextProbe::State::Invalid;
                result.error = QStringLiteral("Paired MOV content identifier item is truncated");
                return result;
            }
            return result;
        }
        position = item->end;
    }
    if (position != meta->ilst->end) {
        result.state = TextProbe::State::Invalid;
        result.error = QStringLiteral("Paired MOV item list exceeds its metadata box");
    }
    return result;
}

std::optional<QString> sameBasenameMovie(const QFileInfo& still)
{
    const QDir directory = still.dir();
    const QString base = still.completeBaseName();
    for (const QString& extension : {QStringLiteral("MOV"), QStringLiteral("mov")}) {
        const QString candidate = directory.filePath(base + QLatin1Char('.') + extension);
        const QFileInfo movie(candidate);
        if (movie.exists() && movie.isFile()) {
            return movie.absoluteFilePath();
        }
    }
    return std::nullopt;
}

} // namespace

AppleLivePhotoIdentifierDetection appleLivePhotoIdentifierFromExifTiff(const QByteArray& tiff)
{
    const TextProbe probe = appleIdentifierFromExif(tiff);
    AppleLivePhotoIdentifierDetection result;
    if (probe.state == TextProbe::State::Found) {
        result.status = AppleLivePhotoIdentifierStatus::Valid;
        result.contentIdentifier = probe.value;
    } else if (probe.state == TextProbe::State::Invalid) {
        result.status = AppleLivePhotoIdentifierStatus::Invalid;
        result.error = probe.error;
    }
    return result;
}

AppleLivePhotoIdentifierDetection
appleLivePhotoIdentifierFromHeifExifBlock(const QByteArray& metadataBlock)
{
    AppleLivePhotoIdentifierDetection result;
    if (metadataBlock.isEmpty()) {
        return result;
    }
    if (metadataBlock.size() < 4) {
        result.status = AppleLivePhotoIdentifierStatus::Invalid;
        result.error = QStringLiteral("HEIF Exif metadata is truncated before its TIFF offset");
        return result;
    }
    const quint32 relativeOffset =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(metadataBlock.constData()));
    const quint64 tiffOffset = 4 + quint64(relativeOffset);
    if (tiffOffset > quint64(metadataBlock.size()) ||
        quint64(metadataBlock.size()) - tiffOffset < 8 ||
        tiffOffset > quint64(std::numeric_limits<qsizetype>::max())) {
        result.status = AppleLivePhotoIdentifierStatus::Invalid;
        result.error = QStringLiteral("HEIF Exif TIFF offset exceeds its metadata block");
        return result;
    }
    return appleLivePhotoIdentifierFromExifTiff(metadataBlock.mid(qsizetype(tiffOffset)));
}

bool hasAppleLivePhotoMovieCandidate(const QString& stillPath)
{
    const QFileInfo still(stillPath);
    return still.exists() && still.isFile() && sameBasenameMovie(still).has_value();
}

AppleLivePhotoPairDetection
detectAppleLivePhotoPairForIdentifier(const QString& stillPath, const QString& contentIdentifier,
                                      const std::atomic_bool* cancelFlag)
{
    AppleLivePhotoPairDetection result;
    if (isCancelled(cancelFlag)) {
        return result;
    }

    const QFileInfo still(stillPath);
    if (!still.exists() || !still.isFile()) {
        result.status = AppleLivePhotoPairStatus::Invalid;
        result.error = QStringLiteral("Apple Live Photo still path is not a file");
        return result;
    }
    const auto moviePath = sameBasenameMovie(still);
    if (!moviePath) {
        return result;
    }

    const QByteArray identifierBytes = contentIdentifier.toLatin1();
    if (contentIdentifier.isEmpty() ||
        identifierBytes.size() > qsizetype(maximumContentIdentifierBytes) ||
        QString::fromLatin1(identifierBytes) != contentIdentifier ||
        std::any_of(identifierBytes.cbegin(), identifierBytes.cend(), [](char ch) {
            const uchar byte = uchar(ch);
            return byte < 0x20 || byte > 0x7e;
        })) {
        result.status = AppleLivePhotoPairStatus::Invalid;
        result.error = QStringLiteral("Apple content identifier is not a bounded printable string");
        return result;
    }

    ExternalFileIdentity movieIdentity;
    const TextProbe movieIdentifier =
        movieContentIdentifier(*moviePath, cancelFlag, &movieIdentity);
    if (movieIdentifier.state == TextProbe::State::Cancelled) {
        return AppleLivePhotoPairDetection{};
    }
    if (movieIdentifier.state == TextProbe::State::Invalid) {
        result.status = AppleLivePhotoPairStatus::Invalid;
        result.error = movieIdentifier.error;
        return result;
    }
    if (movieIdentifier.state != TextProbe::State::Found ||
        movieIdentifier.value != contentIdentifier) {
        result.status = AppleLivePhotoPairStatus::NotPaired;
        return result;
    }

    result.status = AppleLivePhotoPairStatus::Valid;
    result.metadata = AppleLivePhotoPairMetadata{
        QUrl::fromLocalFile(*moviePath),
        contentIdentifier,
        movieIdentity,
    };
    return result;
}

AppleLivePhotoPairDetection detectAppleLivePhotoPair(const QString& stillPath,
                                                     const std::atomic_bool* cancelFlag)
{
    AppleLivePhotoPairDetection result;
    if (isCancelled(cancelFlag)) {
        return result;
    }

    const QFileInfo still(stillPath);
    if (!still.exists() || !still.isFile()) {
        result.status = AppleLivePhotoPairStatus::Invalid;
        result.error = QStringLiteral("Apple Live Photo still path is not a file");
        return result;
    }
    const auto moviePath = sameBasenameMovie(still);
    if (!moviePath) {
        return result;
    }

    const QString suffix = still.suffix().toLower();
    if (suffix != QStringLiteral("jpg") && suffix != QStringLiteral("jpeg")) {
        result.status = AppleLivePhotoPairStatus::NotPaired;
        return result;
    }

    const TextProbe stillIdentifier = jpegAppleIdentifier(still.absoluteFilePath(), cancelFlag);
    if (stillIdentifier.state == TextProbe::State::Cancelled) {
        return result;
    }
    if (stillIdentifier.state == TextProbe::State::Invalid) {
        result.status = AppleLivePhotoPairStatus::Invalid;
        result.error = stillIdentifier.error;
        return result;
    }
    if (stillIdentifier.state != TextProbe::State::Found) {
        result.status = AppleLivePhotoPairStatus::NotPaired;
        return result;
    }

    return detectAppleLivePhotoPairForIdentifier(still.absoluteFilePath(), stillIdentifier.value,
                                                 cancelFlag);
}

} // namespace Licasa
