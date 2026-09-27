#include "tst_photo_asset_probe.h"
#include "imaging/codec_plugins.h"
#include "media/photo_asset_probe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <array>

using namespace Licasa;

namespace {

#if defined(LICASA_SAMSUNG_REAL_FIXTURE_1) && defined(LICASA_SAMSUNG_REAL_FIXTURE_2) &&            \
    defined(LICASA_SAMSUNG_REAL_FIXTURE_3)
struct SamsungRealFixtureExpectation {
    const char* path = nullptr;
    qint64 fileBytes = 0;
    quint64 videoOffset = 0;
    quint64 videoLength = 0;
};

std::array<SamsungRealFixtureExpectation, 3> samsungRealFixtureExpectations()
{
    return {{{LICASA_SAMSUNG_REAL_FIXTURE_1, 8013982, 3366251, 4647675},
             {LICASA_SAMSUNG_REAL_FIXTURE_2, 7221645, 2685814, 4535775},
             {LICASA_SAMSUNG_REAL_FIXTURE_3, 7311312, 2584924, 4726320}}};
}
#endif

#if defined(LICASA_XIAOMI_REAL_FIXTURE_MODERN) && defined(LICASA_XIAOMI_REAL_FIXTURE_LEGACY) &&    \
    defined(LICASA_XIAOMI_REAL_FIXTURE_GAINMAP)
struct XiaomiRealFixtureExpectation {
    const char* path = nullptr;
    qint64 fileBytes = 0;
    quint64 videoOffset = 0;
    quint64 videoLength = 0;
    qint64 presentationUs = 0;
};

std::array<XiaomiRealFixtureExpectation, 3> xiaomiRealFixtureExpectations()
{
    return {{{LICASA_XIAOMI_REAL_FIXTURE_MODERN, 3129678, 698144, 2431534, 1232129},
             {LICASA_XIAOMI_REAL_FIXTURE_LEGACY, 4332013, 2961546, 1370467, 1232308},
             {LICASA_XIAOMI_REAL_FIXTURE_GAINMAP, 8291290, 5123094, 3168196, 1530610}}};
}
#endif

QByteArray makeVideo()
{
    QByteArray video(24, '\0');
    qToBigEndian<quint32>(24, reinterpret_cast<uchar*>(video.data()));
    video.replace(4, 4, "ftyp");
    video.replace(8, 4, "mp42");
    return video;
}

QByteArray motionXmp(quint64 videoLength)
{
    return QStringLiteral(R"XMP(<x:xmpmeta xmlns:x="adobe:ns:meta/">
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
<rdf:Description xmlns:Camera="http://ns.google.com/photos/1.0/camera/"
 xmlns:Container="http://ns.google.com/photos/1.0/container/"
 xmlns:Item="http://ns.google.com/photos/1.0/container/item/"
 Camera:MotionPhoto="1" Camera:MotionPhotoVersion="1"
 Camera:MotionPhotoPresentationTimestampUs="123456">
<Container:Directory><rdf:Seq>
<rdf:li rdf:parseType="Resource"><Container:Item Item:Mime="image/jpeg" Item:Semantic="Primary"/></rdf:li>
<rdf:li rdf:parseType="Resource"><Container:Item Item:Mime="video/mp4" Item:Semantic="MotionPhoto" Item:Length="%1"/></rdf:li>
</rdf:Seq></Container:Directory></rdf:Description></rdf:RDF></x:xmpmeta>)XMP")
        .arg(videoLength)
        .toUtf8();
}

QByteArray legacyPixelXmp(quint64 videoLength)
{
    return QStringLiteral(R"XMP(<x:xmpmeta xmlns:x="adobe:ns:meta/">
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
<rdf:Description xmlns:GCamera="http://ns.google.com/photos/1.0/camera/"
 GCamera:MicroVideo="1" GCamera:MicroVideoVersion="1"
 GCamera:MicroVideoOffset="%1"
 GCamera:MicroVideoPresentationTimestampUs="1331607"/>
</rdf:RDF></x:xmpmeta>)XMP")
        .arg(videoLength)
        .toUtf8();
}

QByteArray app1(const QByteArray& packet)
{
    static constexpr char signature[] = "http://ns.adobe.com/xap/1.0/\0";
    QByteArray payload(signature, int(sizeof(signature) - 1));
    payload += packet;
    const quint16 segmentLength = quint16(payload.size() + 2);
    QByteArray result;
    result += char(0xff);
    result += char(0xe1);
    char length[2];
    qToBigEndian<quint16>(segmentLength, reinterpret_cast<uchar*>(length));
    result.append(length, 2);
    result += payload;
    return result;
}

QString write(const QTemporaryDir& directory, const QByteArray& bytes, const QString& name)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

QByteArray motionJpeg()
{
    const QByteArray video = makeVideo();
    QByteArray jpeg;
    jpeg += char(0xff);
    jpeg += char(0xd8);
    jpeg += app1(motionXmp(quint64(video.size())));
    jpeg += char(0xff);
    jpeg += char(0xd9);
    jpeg += video;
    return jpeg;
}

QByteArray legacyPixelMotionJpeg()
{
    const QByteArray video = makeVideo();
    QByteArray jpeg;
    jpeg += char(0xff);
    jpeg += char(0xd8);
    jpeg += app1(legacyPixelXmp(quint64(video.size())));
    jpeg += char(0xff);
    jpeg += char(0xd9);
    jpeg += video;
    return jpeg;
}

void appendLe16(QByteArray* bytes, quint16 value)
{
    char raw[2];
    qToLittleEndian<quint16>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 2);
}

void appendLe32(QByteArray* bytes, quint32 value)
{
    char raw[4];
    qToLittleEndian<quint32>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 4);
}

QByteArray samsungSeftContent(quint16 type, const QByteArray& name, const QByteArray& payload)
{
    QByteArray content;
    appendLe16(&content, 0);
    appendLe16(&content, type);
    appendLe32(&content, quint32(name.size()));
    content += name;
    content += payload;
    return content;
}

QByteArray samsungSeftJpeg(const QByteArray& xmpPacket, bool includeMotion)
{
    QByteArray bytes;
    bytes += char(0xff);
    bytes += char(0xd8);
    if (!xmpPacket.isEmpty()) {
        bytes += app1(xmpPacket);
    }
    bytes += char(0xff);
    bytes += char(0xd9);

    struct Record {
        quint16 type = 0;
        quint32 start = 0;
        quint32 length = 0;
    };
    QList<Record> records;

    const QByteArray utc = samsungSeftContent(0x0a01, QByteArrayLiteral("Image_UTC_Data"),
                                              QByteArrayLiteral("1459694074807"));
    records.push_back({0x0a01, quint32(bytes.size()), quint32(utc.size())});
    bytes += utc;

    if (includeMotion) {
        const QByteArray video = makeVideo();
        const QByteArray motion =
            samsungSeftContent(0x0a30, QByteArrayLiteral("MotionPhoto_Data"), video);
        records.push_back({0x0a30, quint32(bytes.size()), quint32(motion.size())});
        bytes += motion;
    }

    const quint32 headerStart = quint32(bytes.size());
    QByteArray header("SEFH", 4);
    appendLe32(&header, 106);
    appendLe32(&header, quint32(records.size()));
    for (const auto& record : records) {
        appendLe16(&header, 0);
        appendLe16(&header, record.type);
        appendLe32(&header, headerStart - record.start);
        appendLe32(&header, record.length);
    }
    bytes += header;
    appendLe32(&bytes, quint32(header.size()));
    bytes += QByteArrayLiteral("SEFT");
    return bytes;
}

quint64 samsungMotionVideoOffset(const QByteArray& bytes)
{
    const qsizetype name = bytes.indexOf(QByteArrayLiteral("MotionPhoto_Data"));
    return name >= 0 ? quint64(name + 16) : 0;
}

void appendBe16(QByteArray* bytes, quint16 value)
{
    char raw[2];
    qToBigEndian<quint16>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 2);
}

void appendBe32(QByteArray* bytes, quint32 value)
{
    char raw[4];
    qToBigEndian<quint32>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 4);
}

constexpr quint32 fourCc(char a, char b, char c, char d) noexcept
{
    return (quint32(quint8(a)) << 24) | (quint32(quint8(b)) << 16) | (quint32(quint8(c)) << 8) |
           quint32(quint8(d));
}

QByteArray isoBox(quint32 type, const QByteArray& payload)
{
    QByteArray bytes;
    appendBe32(&bytes, quint32(payload.size() + 8));
    appendBe32(&bytes, type);
    bytes += payload;
    return bytes;
}

QByteArray applePairJpeg(const QByteArray& identifier)
{
    QByteArray maker("Apple iOS\0", 10);
    maker += QByteArray::fromHex("00014d4d");
    appendBe16(&maker, 1);
    appendBe16(&maker, 0x0011);
    appendBe16(&maker, 2);
    appendBe32(&maker, quint32(identifier.size() + 1));
    appendBe32(&maker, 32);
    appendBe32(&maker, 0);
    maker += identifier;
    maker += '\0';

    QByteArray tiff("MM\0*", 4);
    appendBe32(&tiff, 8);
    appendBe16(&tiff, 1);
    appendBe16(&tiff, 0x8769);
    appendBe16(&tiff, 4);
    appendBe32(&tiff, 1);
    appendBe32(&tiff, 26);
    appendBe32(&tiff, 0);
    appendBe16(&tiff, 1);
    appendBe16(&tiff, 0x927c);
    appendBe16(&tiff, 7);
    appendBe32(&tiff, quint32(maker.size()));
    appendBe32(&tiff, 44);
    appendBe32(&tiff, 0);
    tiff += maker;

    QByteArray payload("Exif\0\0", 6);
    payload += tiff;
    QByteArray jpeg = QByteArray::fromHex("ffd8ffe1");
    appendBe16(&jpeg, quint16(payload.size() + 2));
    jpeg += payload;
    jpeg += QByteArray::fromHex("ffd9");
    return jpeg;
}

QByteArray applePairMov(const QByteArray& identifier)
{
    const QByteArray key = QByteArrayLiteral("com.apple.quicktime.content.identifier");
    QByteArray keyEntry;
    appendBe32(&keyEntry, quint32(key.size() + 8));
    appendBe32(&keyEntry, fourCc('m', 'd', 't', 'a'));
    keyEntry += key;
    QByteArray keysPayload(4, '\0');
    appendBe32(&keysPayload, 1);
    keysPayload += keyEntry;

    QByteArray dataPayload;
    appendBe32(&dataPayload, 1);
    appendBe32(&dataPayload, 0);
    dataPayload += identifier;
    const QByteArray data = isoBox(fourCc('d', 'a', 't', 'a'), dataPayload);
    const QByteArray item = isoBox(1, data);
    QByteArray metaPayload(4, '\0');
    metaPayload += isoBox(fourCc('k', 'e', 'y', 's'), keysPayload);
    metaPayload += isoBox(fourCc('i', 'l', 's', 't'), item);
    return isoBox(fourCc('m', 'o', 'o', 'v'), isoBox(fourCc('m', 'e', 't', 'a'), metaPayload));
}

} // namespace

void PhotoAssetProbeTest::initTestCase()
{
    // Production initializes the private image-plugin paths in main() before
    // PhotoAssetProbe can run. Mirror that boundary in this integration test
    // instead of accidentally exercising only the system image plugins.
    Licasa::initializeImagePlugins();
}

void PhotoAssetProbeTest::plainJpegHasNoMotion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        write(directory, QByteArray::fromHex("ffd8ffd9"), QStringLiteral("plain.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY(info.kind == PhotoAssetKind::Still);
    QVERIFY(!info.motion.has_value());
}

void PhotoAssetProbeTest::motionJpegIsDetectedFromStandardApp1Xmp()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray bytes = motionJpeg();
    const QString path = write(directory, bytes, QStringLiteral("sampleMP.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QCOMPARE(info.motion->mimeType, QStringLiteral("video/mp4"));
    QCOMPARE(info.motion->embedded.length, quint64(makeVideo().size()));
    QCOMPARE(info.motion->embedded.offset, quint64(bytes.size() - makeVideo().size()));
    QVERIFY(info.motion->presentationTimestampUs.has_value());
    QCOMPARE(*info.motion->presentationTimestampUs, qint64(123456));
}

void PhotoAssetProbeTest::legacyPixelMicroVideoJpegIsDetectedFromStandardApp1Xmp()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray bytes = legacyPixelMotionJpeg();
    const QString path = write(directory, bytes, QStringLiteral("MVIMG_legacy.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QCOMPARE(info.motion->mimeType, QStringLiteral("video/mp4"));
    QCOMPARE(info.motion->embedded.length, quint64(makeVideo().size()));
    QCOMPARE(info.motion->embedded.offset, quint64(bytes.size() - makeVideo().size()));
    QVERIFY(info.motion->presentationTimestampUs.has_value());
    QCOMPARE(*info.motion->presentationTimestampUs, qint64(1331607));
}

void PhotoAssetProbeTest::samsungSeftJpegWithoutXmpIsDetected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray bytes = samsungSeftJpeg({}, true);
    const quint64 expectedOffset = samsungMotionVideoOffset(bytes);
    QVERIFY(expectedOffset > 0);
    const QString path = write(directory, bytes, QStringLiteral("samsung-motion.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QCOMPARE(info.motion->mimeType, QStringLiteral("video/mp4"));
    QCOMPARE(info.motion->embedded.offset, expectedOffset);
    QCOMPARE(info.motion->embedded.length, quint64(makeVideo().size()));
    QVERIFY(!info.motion->presentationTimestampUs.has_value());
}

void PhotoAssetProbeTest::samsungSeftTrimsClassicMicroVideoFraming()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray provisional = samsungSeftJpeg({}, true);
    const quint64 videoOffset = samsungMotionVideoOffset(provisional);
    QVERIFY(videoOffset > 0);
    const quint64 samsungMicroVideoOffset = quint64(provisional.size()) - videoOffset;

    const QByteArray bytes = samsungSeftJpeg(legacyPixelXmp(samsungMicroVideoOffset), true);
    const quint64 expectedOffset = samsungMotionVideoOffset(bytes);
    QVERIFY(expectedOffset > 0);
    const QString path = write(directory, bytes, QStringLiteral("samsung-xmp-motion.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QCOMPARE(info.motion->embedded.offset, expectedOffset);
    QCOMPARE(info.motion->embedded.length, quint64(makeVideo().size()));
    QVERIFY(info.motion->presentationTimestampUs.has_value());
    QCOMPARE(*info.motion->presentationTimestampUs, qint64(1331607));
    QVERIFY(samsungMicroVideoOffset > info.motion->embedded.length);
}

void PhotoAssetProbeTest::samsungSeftCanRecoverMalformedModernMetadata()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray malformedModern = motionXmp(quint64(makeVideo().size()));
    QCOMPARE(malformedModern.count("Camera:MotionPhotoVersion=\"1\""), 1);
    malformedModern.replace("Camera:MotionPhotoVersion=\"1\"", "Camera:MotionPhotoVersion=\"2\"");
    const QByteArray bytes = samsungSeftJpeg(malformedModern, true);
    const quint64 expectedOffset = samsungMotionVideoOffset(bytes);
    const QString path = write(directory, bytes, QStringLiteral("samsung-modern-broken.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QCOMPARE(info.motion->embedded.offset, expectedOffset);
    QCOMPARE(info.motion->embedded.length, quint64(makeVideo().size()));
}

void PhotoAssetProbeTest::samsungSeftDoesNotOverrideExplicitMotionPhotoZero()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray explicitStill = motionXmp(quint64(makeVideo().size()));
    QCOMPARE(explicitStill.count("Camera:MotionPhoto=\"1\""), 1);
    explicitStill.replace("Camera:MotionPhoto=\"1\"", "Camera:MotionPhoto=\"0\"");
    const QByteArray bytes = samsungSeftJpeg(explicitStill, true);
    const QString path = write(directory, bytes, QStringLiteral("samsung-explicit-still.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY(info.kind == PhotoAssetKind::Still);
    QVERIFY(!info.motion.has_value());
}

void PhotoAssetProbeTest::samsungSeftWithoutMotionRecordStaysStill()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray bytes = samsungSeftJpeg({}, false);
    const QString path = write(directory, bytes, QStringLiteral("samsung-still.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY(info.kind == PhotoAssetKind::Still);
    QVERIFY(!info.motion.has_value());
    QVERIFY(info.metadataError.isEmpty());
}

#ifdef LICASA_LEGACY_PIXEL_REAL_FIXTURE
void PhotoAssetProbeTest::realLegacyPixelMvimgIsIntegrated()
{
    const QString path = QStringLiteral(LICASA_LEGACY_PIXEL_REAL_FIXTURE);
    QVERIFY2(QFileInfo::exists(path), qPrintable(path));
    QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("MVIMG_20191231_235723.jpg"));
    QCOMPARE(QFileInfo(path).size(), qint64(7117287));

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QVERIFY(info.motion->hasEmbeddedVideo());
    QVERIFY(!info.motion->hasExternalVideo());
    QCOMPARE(info.motion->mimeType, QStringLiteral("video/mp4"));
    QCOMPARE(info.motion->embedded.offset, quint64(5318017));
    QCOMPARE(info.motion->embedded.length, quint64(1799270));
    QVERIFY(info.motion->presentationTimestampUs.has_value());
    QCOMPARE(*info.motion->presentationTimestampUs, qint64(0));

    const QVariantMap map = photoAssetInfoToVariantMap(info);
    QCOMPARE(map.size(), 1);
    QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), true);
}
#endif

#if defined(LICASA_SAMSUNG_REAL_FIXTURE_1) && defined(LICASA_SAMSUNG_REAL_FIXTURE_2) &&            \
    defined(LICASA_SAMSUNG_REAL_FIXTURE_3)
void PhotoAssetProbeTest::realSamsungSeftFixturesAreIntegrated()
{
    for (const auto& expected : samsungRealFixtureExpectations()) {
        const QString path = QString::fromUtf8(expected.path);
        const QFileInfo fileInfo(path);
        QVERIFY2(fileInfo.exists(), qPrintable(path));
        QCOMPARE(fileInfo.size(), expected.fileBytes);

        const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
        QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
        QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
        QVERIFY(info.motion.has_value());
        QVERIFY(info.motion->hasEmbeddedVideo());
        QVERIFY(!info.motion->hasExternalVideo());
        QCOMPARE(info.motion->mimeType, QStringLiteral("video/mp4"));
        QCOMPARE(info.motion->embedded.offset, expected.videoOffset);
        QCOMPARE(info.motion->embedded.length, expected.videoLength);

        const QVariantMap map = photoAssetInfoToVariantMap(info);
        QCOMPARE(map.size(), 1);
        QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), true);
    }
}
#endif

void PhotoAssetProbeTest::scanStopsBeforeCompressedImageData()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = makeVideo();
    QByteArray bytes = QByteArray::fromHex("ffd8ffda0008") + QByteArray(6, '\0');
    bytes += app1(motionXmp(quint64(video.size()))); // fake marker after SOS: must never be scanned
    bytes += video;
    const QString path = write(directory, bytes, QStringLiteral("compressed.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY(info.kind == PhotoAssetKind::Still);
    QVERIFY(!info.motion.has_value());
}

void PhotoAssetProbeTest::malformedMetadataIsBoundedAndNonFatal()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray bytes = QByteArray::fromHex("ffd8ffe1ffff") + QByteArray(32, 'x');
    const QString path = write(directory, bytes, QStringLiteral("broken.jpg"));
    QVERIFY(!path.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY(info.kind == PhotoAssetKind::Still);
    QVERIFY(!info.motion.has_value());
    QVERIFY(!info.metadataError.isEmpty());
}

void PhotoAssetProbeTest::metadataScanLimitsAreEnforced()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QByteArray tooManySegments = QByteArray::fromHex("ffd8");
    for (int i = 0; i < 513; ++i) {
        tooManySegments += QByteArray::fromHex("ffe00002");
    }
    const QString countPath = write(directory, tooManySegments, QStringLiteral("too-many.jpg"));
    QVERIFY(!countPath.isEmpty());
    const PhotoAssetInfo countInfo = probePhotoAsset(QUrl::fromLocalFile(countPath));
    QVERIFY(countInfo.kind == PhotoAssetKind::Still);
    QVERIFY(countInfo.metadataError.contains(QStringLiteral("segment-count"), Qt::CaseInsensitive));

    QByteArray tooMuchMetadata = QByteArray::fromHex("ffd8");
    const QByteArray payload(65533, 'm');
    for (int i = 0; i < 17; ++i) {
        tooMuchMetadata += char(0xff);
        tooMuchMetadata += char(0xe0);
        tooMuchMetadata += char(0xff);
        tooMuchMetadata += char(0xff);
        tooMuchMetadata += payload;
    }
    const QString sizePath = write(directory, tooMuchMetadata, QStringLiteral("too-large.jpg"));
    QVERIFY(!sizePath.isEmpty());
    const PhotoAssetInfo sizeInfo = probePhotoAsset(QUrl::fromLocalFile(sizePath));
    QVERIFY(sizeInfo.kind == PhotoAssetKind::Still);
    QVERIFY(sizeInfo.metadataError.contains(QStringLiteral("1 MiB"), Qt::CaseInsensitive));
}

void PhotoAssetProbeTest::asynchronousProbeEmitsSimpleCapabilityState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = write(directory, motionJpeg(), QStringLiteral("asyncMP.jpeg"));
    QVERIFY(!path.isEmpty());

    PhotoAssetProbe probe;
    QSignalSpy spy(&probe, &PhotoAssetProbe::infoReady);
    QVERIFY(spy.isValid());
    probe.request(QUrl::fromLocalFile(path));
    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.size(), 1);
    const QList<QVariant> arguments = spy.takeFirst();
    QCOMPARE(arguments.at(0).toUrl(), QUrl::fromLocalFile(path));
    const QVariantMap map = arguments.at(1).toMap();
    QCOMPARE(map.size(), 1);
    QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), true);
    QVERIFY(!map.contains(QStringLiteral("motionVideoOffset")));
    QVERIFY(!map.contains(QStringLiteral("motionVideoLength")));
    QVERIFY(!map.contains(QStringLiteral("motionMimeType")));
}

void PhotoAssetProbeTest::rapidNavigationSuppressesStaleResult()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString motionPath = write(directory, motionJpeg(), QStringLiteral("firstMP.jpg"));
    const QString plainPath =
        write(directory, QByteArray::fromHex("ffd8ffd9"), QStringLiteral("second.jpg"));
    QVERIFY(!motionPath.isEmpty());
    QVERIFY(!plainPath.isEmpty());

    PhotoAssetProbe probe;
    QSignalSpy spy(&probe, &PhotoAssetProbe::infoReady);
    QVERIFY(spy.isValid());
    probe.request(QUrl::fromLocalFile(motionPath));
    probe.request(QUrl::fromLocalFile(plainPath));

    QVERIFY(spy.wait(1000));
    QTest::qWait(20);
    QCOMPARE(spy.size(), 1);
    const QList<QVariant> arguments = spy.takeFirst();
    QCOMPARE(arguments.at(0).toUrl(), QUrl::fromLocalFile(plainPath));
    const QVariantMap map = arguments.at(1).toMap();
    QCOMPARE(map.size(), 1);
    QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), false);
}

void PhotoAssetProbeTest::appleJpegPairIsIntegratedAsExternalMotion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray identifier("1B82C8F5-2640-4E98-B2D5-88A9A1F6307C");
    const QString still =
        write(directory, applePairJpeg(identifier), QStringLiteral("IMG_7000.JPG"));
    const QString movie =
        write(directory, applePairMov(identifier), QStringLiteral("IMG_7000.MOV"));
    QVERIFY(!still.isEmpty());
    QVERIFY(!movie.isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(still));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QVERIFY(!info.motion->hasEmbeddedVideo());
    QVERIFY(info.motion->hasExternalVideo());
    QCOMPARE(info.motion->externalVideoUrl, QUrl::fromLocalFile(movie));
    QVERIFY(info.motion->externalVideoIdentity.has_value());
    QCOMPARE(info.motion->externalVideoIdentity->size, quint64(QFileInfo(movie).size()));
    QCOMPARE(info.motion->mimeType, QStringLiteral("video/quicktime"));
}

void PhotoAssetProbeTest::appleJpegMismatchedPairStaysStill()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString still =
        write(directory, applePairJpeg("STILL-IDENTIFIER-0001"), QStringLiteral("IMG_7050.JPG"));
    QVERIFY(!still.isEmpty());
    QVERIFY(!write(directory, applePairMov("MOVIE-IDENTIFIER-0002"), QStringLiteral("IMG_7050.MOV"))
                 .isEmpty());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(still));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::Still);
    QVERIFY(!info.motion.has_value());
    const QVariantMap map = photoAssetInfoToVariantMap(info);
    QCOMPARE(map.size(), 1);
    QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), false);
}

void PhotoAssetProbeTest::asynchronousApplePairingEmitsCapability()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray identifier("047F9182-B906-4426-A5DC-C0BA8193DD41");
    const QString still =
        write(directory, applePairJpeg(identifier), QStringLiteral("IMG_7100.jpeg"));
    QVERIFY(!still.isEmpty());
    QVERIFY(!write(directory, applePairMov(identifier), QStringLiteral("IMG_7100.mov")).isEmpty());

    PhotoAssetProbe probe;
    QSignalSpy spy(&probe, &PhotoAssetProbe::infoReady);
    probe.request(QUrl::fromLocalFile(still));
    QVERIFY(spy.wait(1000));
    QCOMPARE(spy.size(), 1);
    const QVariantMap map = spy.takeFirst().at(1).toMap();
    QCOMPARE(map.size(), 1);
    QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), true);
}

#if defined(LICASA_APPLE_REAL_HEIC_FIXTURE) && defined(LICASA_APPLE_REAL_MOV_FIXTURE)
void PhotoAssetProbeTest::realAppleHeicMovPairIsIntegrated()
{
    const QString still = QStringLiteral(LICASA_APPLE_REAL_HEIC_FIXTURE);
    const QString movie = QStringLiteral(LICASA_APPLE_REAL_MOV_FIXTURE);
    QVERIFY2(QFileInfo::exists(still), qPrintable(still));
    QVERIFY2(QFileInfo::exists(movie), qPrintable(movie));
    QCOMPARE(QFileInfo(still).completeBaseName(), QFileInfo(movie).completeBaseName());

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(still));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QVERIFY(!info.motion->hasEmbeddedVideo());
    QVERIFY(info.motion->hasExternalVideo());
    QCOMPARE(info.motion->externalVideoUrl, QUrl::fromLocalFile(movie));
    QVERIFY(info.motion->externalVideoIdentity.has_value());
    QCOMPARE(info.motion->externalVideoIdentity->size, quint64(QFileInfo(movie).size()));
    QCOMPARE(info.motion->mimeType, QStringLiteral("video/quicktime"));

    const QVariantMap map = photoAssetInfoToVariantMap(info);
    QCOMPARE(map.size(), 1);
    QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), true);
}
#endif

#if defined(LICASA_XIAOMI_REAL_FIXTURE_MODERN) && defined(LICASA_XIAOMI_REAL_FIXTURE_LEGACY) &&    \
    defined(LICASA_XIAOMI_REAL_FIXTURE_GAINMAP)
void PhotoAssetProbeTest::realXiaomiMotionFixturesAreIntegrated()
{
    for (const auto& expected : xiaomiRealFixtureExpectations()) {
        const QString path = QString::fromUtf8(expected.path);
        const QFileInfo fileInfo(path);
        QVERIFY2(fileInfo.exists(), qPrintable(path));
        QCOMPARE(fileInfo.size(), expected.fileBytes);

        const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
        QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
        QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
        QVERIFY(info.motion.has_value());
        QVERIFY(info.motion->hasEmbeddedVideo());
        QVERIFY(!info.motion->hasExternalVideo());
        QCOMPARE(info.motion->mimeType, QStringLiteral("video/mp4"));
        QCOMPARE(info.motion->embedded.offset, expected.videoOffset);
        QCOMPARE(info.motion->embedded.length, expected.videoLength);
        QVERIFY(info.motion->presentationTimestampUs.has_value());
        QCOMPARE(*info.motion->presentationTimestampUs, expected.presentationUs);

        const QVariantMap map = photoAssetInfoToVariantMap(info);
        QCOMPARE(map.size(), 1);
        QCOMPARE(map.value(QStringLiteral("motionAvailable")).toBool(), true);
    }
}
#endif

#ifdef LICASA_HEIC_MOTION_FIXTURE
void PhotoAssetProbeTest::realHeicMotionPhotoBridge()
{
    const QString path = QStringLiteral(LICASA_HEIC_MOTION_FIXTURE);
    QVERIFY2(QFileInfo::exists(path), qPrintable(path));

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::MotionPhoto);
    QVERIFY(info.motion.has_value());
    QVERIFY(info.motion->embedded.length > 0);
    QCOMPARE(info.motion->mimeType, QStringLiteral("video/mp4"));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const qint64 signedSize = file.size();
    QVERIFY(signedSize > 0);

    const auto rangeEnd =
        checkedByteOffsetAdd(info.motion->embedded.offset, info.motion->embedded.length);
    QVERIFY(rangeEnd.has_value());
    QVERIFY(*rangeEnd < quint64(signedSize));

    const quint64 trailerLength = quint64(signedSize) - *rangeEnd;
    QCOMPARE(trailerLength, quint64(281));
    QVERIFY(file.seek(qint64(*rangeEnd)));
    const QByteArray sefdHeader = file.read(8);
    QCOMPARE(sefdHeader.size(), 8);
    QCOMPARE(sefdHeader.mid(4, 4), QByteArrayLiteral("sefd"));
    QCOMPARE(
        quint64(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(sefdHeader.constData()))),
        trailerLength);

    QVERIFY(file.seek(qint64(info.motion->embedded.offset)));
    const QByteArray videoHeader = file.read(8);
    QCOMPARE(videoHeader.size(), 8);
    QCOMPARE(videoHeader.mid(4, 4), QByteArrayLiteral("ftyp"));
}
#endif

#ifdef LICASA_AVIF_XMP_FIXTURE
void PhotoAssetProbeTest::realAvifXmpRemainsStill()
{
    const QString path = QStringLiteral(LICASA_AVIF_XMP_FIXTURE);
    QVERIFY2(QFileInfo::exists(path), qPrintable(path));

    const PhotoAssetInfo info = probePhotoAsset(QUrl::fromLocalFile(path));
    QVERIFY2(info.metadataError.isEmpty(), qPrintable(info.metadataError));
    QVERIFY(info.kind == PhotoAssetKind::Still);
    QVERIFY(!info.motion.has_value());
}
#endif

QTEST_GUILESS_MAIN(PhotoAssetProbeTest)
