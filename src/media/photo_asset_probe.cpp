#include "media/photo_asset_probe.h"
#include "imaging/image_decode_contract.h"
#include "media/live_photo_pairing.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace Licasa {
namespace {

constexpr quint64 maximumJpegMetadataScan = 1024 * 1024;
constexpr qsizetype maximumMotionXmpBytes = 1024 * 1024;
constexpr qsizetype maximumMotionXmpBase64Bytes = ((maximumMotionXmpBytes + 2) / 3) * 4;
constexpr qsizetype maximumAppleExifBytes = 1024 * 1024 + 4;
constexpr qsizetype maximumAppleExifBase64Bytes = ((maximumAppleExifBytes + 2) / 3) * 4;
constexpr int maximumJpegSegments = 512;
constexpr char standardXmpSignature[] = "http://ns.adobe.com/xap/1.0/\0";
constexpr qsizetype standardXmpSignatureSize = sizeof(standardXmpSignature) - 1;

struct XmpProbeResult {
    QByteArray packet;
    QString error;
};

struct PluginMetadataProbeResult {
    XmpProbeResult xmp;
    AppleLivePhotoIdentifierDetection appleIdentifier;
};

bool cancelled(const std::atomic_bool* flag)
{
    return flag && flag->load(std::memory_order_relaxed);
}

bool readExact(QFile* file, char* destination, qint64 bytes, const std::atomic_bool* cancelFlag)
{
    if (bytes < 0 || cancelled(cancelFlag)) {
        return false;
    }
    qint64 done = 0;
    while (done < bytes) {
        if (cancelled(cancelFlag)) {
            return false;
        }
        const qint64 got = file->read(destination + done, bytes - done);
        if (got <= 0) {
            return false;
        }
        done += got;
    }
    return true;
}

PluginMetadataProbeResult imagePluginMetadata(const QString& path, bool requestAppleExif,
                                              const std::atomic_bool* cancelFlag)
{
    namespace Contract = ImageDecodeContract;
    PluginMetadataProbeResult result;
    if (cancelled(cancelFlag)) {
        return result;
    }

    QImageReader reader(path);
    if (!reader.device()) {
        result.xmp.error = QStringLiteral("Image metadata probe could not open the source");
        return result;
    }
    reader.device()->setProperty(Contract::appleLivePhotoExifProbeProperty, requestAppleExif);
    // Carry only cancellation into the plugin. The metadata bridge performs no
    // raster decode; retaining Qt's existing allocation-derived pixel budget
    // avoids creating a second application resource policy for this probe.
    Contract::configure(reader, Contract::pixelBudget(reader.device()), cancelFlag);

    const QString bridgeError = reader.text(QStringLiteral("LicasaXmpError"));
    const QByteArray encoded = reader.text(QStringLiteral("LicasaXmpBase64")).toLatin1();
    if (cancelled(cancelFlag)) {
        return result;
    }
    if (!bridgeError.isEmpty()) {
        result.xmp.error = bridgeError;
    } else if (!encoded.isEmpty()) {
        if (encoded.size() > maximumMotionXmpBase64Bytes || encoded.size() % 4 != 0) {
            result.xmp.error = QStringLiteral("Image XMP bridge exceeds the 1 MiB probe limit");
        } else {
            const QByteArray decoded = QByteArray::fromBase64(encoded);
            // The native plugins emit canonical Base64. Round-tripping rejects
            // whitespace, ignored garbage, misplaced padding and non-canonical input.
            if (decoded.size() > maximumMotionXmpBytes || decoded.toBase64() != encoded) {
                result.xmp.error =
                    QStringLiteral("Image XMP bridge returned invalid Base64 metadata");
            } else {
                result.xmp.packet = decoded;
            }
        }
    }

    if (requestAppleExif) {
        const QString exifError = reader.text(QStringLiteral("LicasaHeifExifError"));
        const QByteArray exifEncoded =
            reader.text(QStringLiteral("LicasaHeifExifBase64")).toLatin1();
        if (cancelled(cancelFlag)) {
            return PluginMetadataProbeResult{};
        }
        if (!exifError.isEmpty()) {
            result.appleIdentifier.status = AppleLivePhotoIdentifierStatus::Invalid;
            result.appleIdentifier.error = exifError;
        } else if (!exifEncoded.isEmpty()) {
            if (exifEncoded.size() > maximumAppleExifBase64Bytes || exifEncoded.size() % 4 != 0) {
                result.appleIdentifier.status = AppleLivePhotoIdentifierStatus::Invalid;
                result.appleIdentifier.error =
                    QStringLiteral("HEIF Exif bridge exceeds the 1 MiB Apple metadata limit");
            } else {
                const QByteArray exif = QByteArray::fromBase64(exifEncoded);
                if (exif.size() > maximumAppleExifBytes || exif.toBase64() != exifEncoded) {
                    result.appleIdentifier.status = AppleLivePhotoIdentifierStatus::Invalid;
                    result.appleIdentifier.error =
                        QStringLiteral("HEIF Exif bridge returned invalid Base64 metadata");
                } else {
                    result.appleIdentifier = appleLivePhotoIdentifierFromHeifExifBlock(exif);
                }
            }
        }
    }
    return result;
}

XmpProbeResult jpegStandardXmp(const QString& path, const std::atomic_bool* cancelFlag)
{
    XmpProbeResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        result.error = QStringLiteral("Unable to open JPEG for bounded metadata probing");
        return result;
    }

    char soi[2]{};
    if (!readExact(&file, soi, 2, cancelFlag) || uchar(soi[0]) != 0xff || uchar(soi[1]) != 0xd8) {
        if (!cancelled(cancelFlag)) {
            result.error = QStringLiteral("JPEG metadata probe did not find an SOI marker");
        }
        return result;
    }

    quint64 scanned = 2;
    for (int segment = 0; segment < maximumJpegSegments && scanned < maximumJpegMetadataScan;
         ++segment) {
        if (cancelled(cancelFlag)) {
            return result;
        }

        char byte = 0;
        if (!readExact(&file, &byte, 1, cancelFlag)) {
            result.error = QStringLiteral("JPEG metadata ended before SOS/EOI");
            return result;
        }
        ++scanned;
        if (uchar(byte) != 0xff) {
            result.error = QStringLiteral("JPEG metadata contains bytes outside a marker segment");
            return result;
        }

        do {
            if (!readExact(&file, &byte, 1, cancelFlag)) {
                result.error = QStringLiteral("JPEG marker is truncated");
                return result;
            }
            ++scanned;
        } while (uchar(byte) == 0xff && scanned < maximumJpegMetadataScan);

        const uchar marker = uchar(byte);
        if (marker == 0xda || marker == 0xd9) {
            return result; // start-of-scan / end-of-image: metadata phase is over
        }
        if (marker == 0x00) {
            continue;
        }
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue; // standalone markers
        }

        char lengthBytes[2]{};
        if (!readExact(&file, lengthBytes, 2, cancelFlag)) {
            result.error = QStringLiteral("JPEG segment length is truncated");
            return result;
        }
        scanned += 2;
        const quint16 declaredLength =
            qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(lengthBytes));
        if (declaredLength < 2) {
            result.error = QStringLiteral("JPEG segment declares an invalid length");
            return result;
        }

        const quint64 payloadLength = quint64(declaredLength) - 2;
        if (payloadLength > maximumJpegMetadataScan - std::min(scanned, maximumJpegMetadataScan)) {
            result.error = QStringLiteral("JPEG metadata exceeds the 1 MiB probe limit");
            return result;
        }

        if (marker == 0xe1 && payloadLength >= quint64(standardXmpSignatureSize)) {
            QByteArray payload(qsizetype(payloadLength), Qt::Uninitialized);
            if (!readExact(&file, payload.data(), payload.size(), cancelFlag)) {
                if (!cancelled(cancelFlag)) {
                    result.error = QStringLiteral("JPEG APP1 XMP segment is truncated");
                }
                return result;
            }
            scanned += payloadLength;
            if (payload.size() >= standardXmpSignatureSize &&
                std::equal(standardXmpSignature, standardXmpSignature + standardXmpSignatureSize,
                           payload.constData())) {
                result.packet = payload.mid(standardXmpSignatureSize);
                return result;
            }
            continue;
        }

        const qint64 currentPosition = file.pos();
        const qint64 currentSize = file.size();
        const auto target = currentPosition >= 0
                                ? checkedByteOffsetAdd(quint64(currentPosition), payloadLength)
                                : std::nullopt;
        if (!target || currentSize < 0 || *target > quint64(currentSize) ||
            *target > quint64(std::numeric_limits<qint64>::max()) || !file.seek(qint64(*target))) {
            result.error = QStringLiteral("JPEG metadata segment exceeds the source file");
            return result;
        }
        scanned += payloadLength;
    }

    if (scanned >= maximumJpegMetadataScan) {
        result.error = QStringLiteral("JPEG metadata exceeds the 1 MiB probe limit");
    } else if (!cancelled(cancelFlag)) {
        result.error = QStringLiteral("JPEG metadata exceeds the segment-count limit");
    }
    return result;
}

} // namespace

PhotoAssetInfo probePhotoAsset(const QUrl& url, const std::atomic_bool* cancelFlag)
{
    PhotoAssetInfo info;
    info.sourceUrl = url;
    if (!url.isValid() || !url.isLocalFile() || cancelled(cancelFlag)) {
        return info;
    }

    const QFileInfo fileInfo(url.toLocalFile());
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return info;
    }

    const QString suffix = fileInfo.suffix().toLower();
    XmpProbeResult xmp;
    AppleLivePhotoIdentifierDetection appleIdentifier;
    const bool isJpeg = suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
    const bool isHeif = suffix == QStringLiteral("heic") || suffix == QStringLiteral("heif") ||
                        suffix == QStringLiteral("hif");
    const bool isAvif = suffix == QStringLiteral("avif");
    const bool appleMovieCandidate =
        (isJpeg || isHeif) && hasAppleLivePhotoMovieCandidate(fileInfo.absoluteFilePath());
    if (isJpeg) {
        // Keep the Stage-2-qualified Android JPEG XMP path unchanged. Apple
        // pairing is attempted separately and only after a same-basename MOV
        // candidate exists, so plain JPEGs pay only the bounded async stat path.
        xmp = jpegStandardXmp(fileInfo.absoluteFilePath(), cancelFlag);
    } else if (isHeif || isAvif) {
        const PluginMetadataProbeResult metadata = imagePluginMetadata(
            fileInfo.absoluteFilePath(), isHeif && appleMovieCandidate, cancelFlag);
        xmp = metadata.xmp;
        if (isHeif) {
            appleIdentifier = metadata.appleIdentifier;
        }
    } else {
        return info;
    }

    if (cancelled(cancelFlag)) {
        return info;
    }

    // Embedded Android Motion Photo metadata wins when it is valid. A malformed
    // modern declaration may still be followed by an independently valid,
    // structurally indexed Samsung SEFT MotionPhoto_Data record. An explicit
    // Android NotMotionPhoto result remains authoritative.
    bool trySamsungFallback = xmp.packet.isEmpty();
    if (!xmp.packet.isEmpty()) {
        const AndroidMotionPhotoDetection detection =
            detectAndroidMotionPhotoV1(fileInfo.absoluteFilePath(), xmp.packet, cancelFlag);
        if (cancelled(cancelFlag)) {
            return info;
        }
        if (detection.status == AndroidMotionPhotoStatus::Valid && detection.metadata) {
            const auto& metadata = *detection.metadata;
            info.kind = PhotoAssetKind::MotionPhoto;
            MotionComponent motion;
            motion.embedded = metadata.videoRange;
            motion.mimeType = metadata.videoMimeType;
            motion.presentationTimestampUs = metadata.presentationTimestampUs;
            info.motion = std::move(motion);
            return info;
        }
        if (detection.status == AndroidMotionPhotoStatus::Invalid) {
            info.metadataError = detection.error;
            trySamsungFallback = true;
        }
    } else if (!xmp.error.isEmpty()) {
        info.metadataError = xmp.error;
    }

    // Classic Samsung JPEG Motion Photos use an EOF-indexed SEF/SEFT trailer.
    // Read that vendor structure only when the Google path did not already make
    // an authoritative valid/not-motion decision. This recovers Samsung files
    // whose MicroVideoOffset includes SEF framing bytes and malformed modern
    // directories only when the independent SEFT index is itself valid.
    if (isJpeg && trySamsungFallback) {
        const AndroidMotionPhotoDetection samsung =
            detectSamsungMotionPhotoJpegSeft(fileInfo.absoluteFilePath(), xmp.packet, cancelFlag);
        if (cancelled(cancelFlag)) {
            return info;
        }
        if (samsung.status == AndroidMotionPhotoStatus::Valid && samsung.metadata) {
            const auto& metadata = *samsung.metadata;
            info.kind = PhotoAssetKind::MotionPhoto;
            info.metadataError.clear();
            MotionComponent motion;
            motion.embedded = metadata.videoRange;
            motion.mimeType = metadata.videoMimeType;
            motion.presentationTimestampUs = metadata.presentationTimestampUs;
            info.motion = std::move(motion);
            return info;
        }
        if (samsung.status == AndroidMotionPhotoStatus::Invalid && info.metadataError.isEmpty()) {
            info.metadataError = samsung.error;
        }
    }

    AppleLivePhotoPairDetection applePair;
    if (isJpeg && appleMovieCandidate) {
        applePair = detectAppleLivePhotoPair(fileInfo.absoluteFilePath(), cancelFlag);
    } else if (isHeif) {
        if (appleIdentifier.status == AppleLivePhotoIdentifierStatus::Valid) {
            applePair = detectAppleLivePhotoPairForIdentifier(
                fileInfo.absoluteFilePath(), appleIdentifier.contentIdentifier, cancelFlag);
        } else if (appleIdentifier.status == AppleLivePhotoIdentifierStatus::Invalid) {
            if (info.metadataError.isEmpty()) {
                info.metadataError = appleIdentifier.error;
            }
        }
    }
    if (cancelled(cancelFlag)) {
        return info;
    }
    if (applePair.status == AppleLivePhotoPairStatus::Invalid) {
        if (info.metadataError.isEmpty()) {
            info.metadataError = applePair.error;
        }
        return info;
    }
    if (applePair.status == AppleLivePhotoPairStatus::Valid && applePair.metadata) {
        info.kind = PhotoAssetKind::MotionPhoto;
        info.metadataError.clear();
        info.motion = MotionComponent{
            {},
            QStringLiteral("video/quicktime"),
            {},
            applePair.metadata->videoUrl,
            applePair.metadata->videoIdentity,
        };
    }
    return info;
}

QVariantMap photoAssetInfoToVariantMap(const PhotoAssetInfo& info)
{
    // QML only needs the capability bit at this stage. The validated byte
    // range, MIME and timestamp remain C++ data so scripting cannot turn a
    // metadata probe into an arbitrary file-range interface.
    return QVariantMap{
        {QStringLiteral("motionAvailable"),
         info.kind == PhotoAssetKind::MotionPhoto && info.motion.has_value()},
    };
}

PhotoAssetProbe::PhotoAssetProbe(QObject* parent) : QObject(parent)
{
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(0);
}

PhotoAssetProbe::~PhotoAssetProbe()
{
    cancel();
    pool_.waitForDone();
}

void PhotoAssetProbe::setResultHandler(ResultHandler handler)
{
    resultHandler_ = std::move(handler);
}

void PhotoAssetProbe::setInvalidationHandler(InvalidationHandler handler)
{
    invalidationHandler_ = std::move(handler);
}

void PhotoAssetProbe::request(const QUrl& url)
{
    cancel();
    const std::uint64_t generation = generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto flag = std::make_shared<std::atomic_bool>(false);
    cancellation_ = flag;

    pool_.start([this, url, flag, generation]() {
        const PhotoAssetInfo result = probePhotoAsset(url, flag.get());
        if (flag->load(std::memory_order_relaxed) ||
            generation_.load(std::memory_order_relaxed) != generation) {
            return;
        }
        const QVariantMap map = photoAssetInfoToVariantMap(result);
        QMetaObject::invokeMethod(
            this,
            [this, url, result, map, flag, generation]() {
                if (flag->load(std::memory_order_relaxed) ||
                    generation_.load(std::memory_order_relaxed) != generation) {
                    return;
                }
                if (resultHandler_) {
                    resultHandler_(url, result);
                }
                emit infoReady(url, map);
            },
            Qt::QueuedConnection);
    });
}

void PhotoAssetProbe::cancel()
{
    if (cancellation_) {
        cancellation_->store(true, std::memory_order_relaxed);
    }
    cancellation_.reset();
    generation_.fetch_add(1, std::memory_order_relaxed);
    pool_.clear();
    if (invalidationHandler_) {
        invalidationHandler_();
    }
}

} // namespace Licasa
