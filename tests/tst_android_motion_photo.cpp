#include "tst_android_motion_photo.h"
#include "media/android_motion_photo.h"

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <atomic>
#include <limits>

using namespace Licasa;

namespace {

QByteArray ftypVideo()
{
    return QByteArray::fromHex("000000186674797069736f6d0000020069736f6d6d703431");
}

QByteArray widePrefixedVideo() { return QByteArray::fromHex("0000000877696465") + ftypVideo(); }

QByteArray mpvdHeader(quint64 videoLength)
{
    QByteArray header(8, '\0');
    qToBigEndian<quint32>(quint32(videoLength + 8), reinterpret_cast<uchar*>(header.data()));
    header.replace(4, 4, QByteArray("mpvd", 4));
    return header;
}

QByteArray sizedBox(const QByteArray& type, quint32 size)
{
    if (type.size() != 4 || size < 8) {
        return {};
    }
    QByteArray box(int(size), '\0');
    qToBigEndian<quint32>(size, reinterpret_cast<uchar*>(box.data()));
    box.replace(4, 4, type);
    return box;
}

QString writeFile(QTemporaryDir& directory, const QByteArray& bytes)
{
    const QString path = directory.filePath(QStringLiteral("asset.bin"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    file.close();
    return path;
}

QByteArray xmpFor(const QString& primaryMime, quint64 videoLength,
                  std::optional<quint64> primaryPadding = std::nullopt,
                  QString videoMime = QStringLiteral("video/mp4"), QString cameraExtra = {},
                  QString middleItems = {})
{
    QString padding;
    if (primaryPadding.has_value()) {
        padding = QStringLiteral(" Item:Padding=\"%1\"").arg(*primaryPadding);
    }

    return QStringLiteral(R"XMP(
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description xmlns:GCamera="http://ns.google.com/photos/1.0/camera/"
               xmlns:Container="http://ns.google.com/photos/1.0/container/"
               xmlns:Item="http://ns.google.com/photos/1.0/container/item/"
               GCamera:MotionPhoto="1"
               GCamera:MotionPhotoVersion="1"
               GCamera:MotionPhotoPresentationTimestampUs="123456" %1>
   <Container:Directory><rdf:Seq>
<rdf:li rdf:parseType="Resource"><Container:Item
  Item:Mime="%2" Item:Semantic="Primary" Item:Length="0"%3/></rdf:li>
%4
<rdf:li rdf:parseType="Resource"><Container:Item
  Item:Mime="%5" Item:Semantic="MotionPhoto" Item:Length="%6"/></rdf:li>
   </rdf:Seq></Container:Directory>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>)XMP")
        .arg(cameraExtra, primaryMime, padding, middleItems, videoMime,
             QString::number(videoLength))
        .toUtf8();
}

QByteArray legacyPixelXmp(quint64 videoLength, QString prefix = QStringLiteral("GCamera"),
                          std::optional<qint64> version = qint64(1), qint64 microVideo = 1,
                          qint64 presentationTimestampUs = 1331607)
{
    const QString versionAttribute =
        version.has_value()
            ? QStringLiteral(" %1:MicroVideoVersion=\"%2\"").arg(prefix, QString::number(*version))
            : QString();
    return QStringLiteral(R"XMP(
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description xmlns:%1="http://ns.google.com/photos/1.0/camera/"
      %1:MicroVideo="%2"%3
      %1:MicroVideoOffset="%4"
      %1:MicroVideoPresentationTimestampUs="%5"/>
 </rdf:RDF>
</x:xmpmeta>)XMP")
        .arg(prefix, QString::number(microVideo), versionAttribute, QString::number(videoLength),
             QString::number(presentationTimestampUs))
        .toUtf8();
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

struct SamsungSeftRecordFixture {
    quint16 type = 0;
    QByteArray content;
};

QByteArray samsungSeftJpeg(const QList<SamsungSeftRecordFixture>& records, quint32 version = 106)
{
    QByteArray bytes = QByteArray::fromHex("ffd8ffd9");
    struct RecordLocation {
        quint16 type = 0;
        quint32 start = 0;
        quint32 length = 0;
    };
    QList<RecordLocation> locations;
    for (const auto& record : records) {
        RecordLocation location;
        location.type = record.type;
        location.start = quint32(bytes.size());
        location.length = quint32(record.content.size());
        locations.push_back(location);
        bytes += record.content;
    }

    const quint32 sefhStart = quint32(bytes.size());
    QByteArray header("SEFH", 4);
    appendLe32(&header, version);
    appendLe32(&header, quint32(locations.size()));
    for (const auto& location : locations) {
        appendLe16(&header, 0);
        appendLe16(&header, location.type);
        appendLe32(&header, sefhStart - location.start);
        appendLe32(&header, location.length);
    }
    bytes += header;
    appendLe32(&bytes, quint32(header.size()));
    bytes += QByteArrayLiteral("SEFT");
    return bytes;
}

QByteArray samsungMotionContent(const QByteArray& video)
{
    return samsungSeftContent(0x0a30, QByteArrayLiteral("MotionPhoto_Data"), video);
}

QByteArray samsungStillMetadataContent()
{
    return samsungSeftContent(0x0a01, QByteArrayLiteral("Image_UTC_Data"),
                              QByteArrayLiteral("1459694074807"));
}

QByteArray withSecondaryMotionPadding(QByteArray xmp, quint64 padding)
{
    const QByteArray needle = QByteArrayLiteral("Item:Semantic=\"MotionPhoto\" Item:Length=\"");
    if (xmp.count(needle) != 1) {
        return {};
    }
    const QByteArray replacement =
        QByteArrayLiteral("Item:Semantic=\"MotionPhoto\" Item:Padding=\"") +
        QByteArray::number(padding) + QByteArrayLiteral("\" Item:Length=\"");
    xmp.replace(needle, replacement);
    return xmp;
}

void expectInvalid(const AndroidMotionPhotoDetection& result, QStringView messageFragment = {})
{
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Invalid);
    QVERIFY(!result.metadata.has_value());
    if (!messageFragment.isEmpty()) {
        QVERIFY2(result.error.contains(messageFragment, Qt::CaseInsensitive),
                 qPrintable(result.error));
    }
}

} // namespace

void AndroidMotionPhotoTest::validJpegUsesFinalItemLengthAndBoundedRange()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QByteArray primary(40, 'J');
    const QString path = writeFile(directory, primary + video);
    QVERIFY(!path.isEmpty());

    const auto result = detectAndroidMotionPhotoV1(
        path, xmpFor(QStringLiteral("image/jpeg"), quint64(video.size())));
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    const auto& metadata = *result.metadata;
    QCOMPARE(metadata.primaryMimeType, QStringLiteral("image/jpeg"));
    QCOMPARE(metadata.videoMimeType, QStringLiteral("video/mp4"));
    QCOMPARE(metadata.primaryImageLength, quint64(primary.size()));
    QCOMPARE(metadata.primaryPadding, quint64(0));
    QCOMPARE(metadata.videoRange.offset, quint64(primary.size()));
    QCOMPARE(metadata.videoRange.length, quint64(video.size()));
    QVERIFY(metadata.presentationTimestampUs.has_value());
    QCOMPARE(*metadata.presentationTimestampUs, qint64(123456));

    const QByteArray zeroSecondaryPadding =
        withSecondaryMotionPadding(xmpFor(QStringLiteral("image/jpeg"), quint64(video.size())), 0);
    QVERIFY(!zeroSecondaryPadding.isEmpty());
    const auto zeroPaddingResult = detectAndroidMotionPhotoV1(path, zeroSecondaryPadding);
    QCOMPARE(zeroPaddingResult.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(zeroPaddingResult.metadata.has_value());
    QCOMPARE(zeroPaddingResult.metadata->videoRange.offset, metadata.videoRange.offset);
    QCOMPARE(zeroPaddingResult.metadata->videoRange.length, metadata.videoRange.length);

    const QByteArray nonZeroSecondaryPadding =
        withSecondaryMotionPadding(xmpFor(QStringLiteral("image/jpeg"), quint64(video.size())), 1);
    QVERIFY(!nonZeroSecondaryPadding.isEmpty());
    expectInvalid(detectAndroidMotionPhotoV1(path, nonZeroSecondaryPadding),
                  QStringLiteral("secondary"));

    ByteRangeDevice range(path, metadata.videoRange.offset, metadata.videoRange.length);
    QVERIFY(range.open(QIODevice::ReadOnly));
    QCOMPARE(range.readAll(), video);
}

void AndroidMotionPhotoTest::quickTimeWidePreambleIsAccepted()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = widePrefixedVideo();
    const QString path = writeFile(directory, QByteArray(24, 'J') + video);
    QVERIFY(!path.isEmpty());

    const auto result =
        detectAndroidMotionPhotoV1(path, xmpFor(QStringLiteral("image/jpeg"), quint64(video.size()),
                                                std::nullopt, QStringLiteral("video/quicktime")));
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    QCOMPARE(result.metadata->videoMimeType, QStringLiteral("video/quicktime"));
    QCOMPARE(result.metadata->videoRange.offset, quint64(24));
}

void AndroidMotionPhotoTest::legacyPixelMicroVideoUsesBackwardOffsetAndBoundedRange()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    QByteArray primary(41, 'P');
    primary[0] = char(0xff);
    primary[1] = char(0xd8);
    const QString path = writeFile(directory, primary + video);
    QVERIFY(!path.isEmpty());

    const auto result = detectAndroidMotionPhotoV1(path, legacyPixelXmp(quint64(video.size())));
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    const auto& metadata = *result.metadata;
    QCOMPARE(metadata.primaryMimeType, QStringLiteral("image/jpeg"));
    QCOMPARE(metadata.videoMimeType, QStringLiteral("video/mp4"));
    QCOMPARE(metadata.primaryImageLength, quint64(primary.size()));
    QCOMPARE(metadata.primaryPadding, quint64(0));
    QCOMPARE(metadata.videoRange.offset, quint64(primary.size()));
    QCOMPARE(metadata.videoRange.length, quint64(video.size()));
    QVERIFY(metadata.presentationTimestampUs.has_value());
    QCOMPARE(*metadata.presentationTimestampUs, qint64(1331607));

    ByteRangeDevice range(path, metadata.videoRange.offset, metadata.videoRange.length);
    QVERIFY(range.open(QIODevice::ReadOnly));
    QCOMPARE(range.readAll(), video);
}

void AndroidMotionPhotoTest::legacyPixelCameraPrefixAndOptionalVersionAreAccepted()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    QByteArray primary(29, 'P');
    primary[0] = char(0xff);
    primary[1] = char(0xd8);
    const QString path = writeFile(directory, primary + video);
    QVERIFY(!path.isEmpty());

    const auto result = detectAndroidMotionPhotoV1(
        path, legacyPixelXmp(quint64(video.size()), QStringLiteral("Camera"), std::nullopt));
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    QCOMPARE(result.metadata->videoRange.offset, quint64(29));
    QCOMPARE(result.metadata->videoRange.length, quint64(video.size()));
}

void AndroidMotionPhotoTest::legacyPixelInvalidMetadataIsRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    QByteArray primary(40, 'P');
    primary[0] = char(0xff);
    primary[1] = char(0xd8);
    const QString path = writeFile(directory, primary + video);
    QVERIFY(!path.isEmpty());

    const auto zeroFlag = detectAndroidMotionPhotoV1(
        path, legacyPixelXmp(quint64(video.size()), QStringLiteral("GCamera"), qint64(1), 0));
    QCOMPARE(zeroFlag.status, AndroidMotionPhotoStatus::NotMotionPhoto);
    QVERIFY(!zeroFlag.metadata.has_value());

    const auto nonOneFlag = detectAndroidMotionPhotoV1(
        path, legacyPixelXmp(quint64(video.size()), QStringLiteral("GCamera"), qint64(1), 2));
    QCOMPARE(nonOneFlag.status, AndroidMotionPhotoStatus::NotMotionPhoto);
    QVERIFY(!nonOneFlag.metadata.has_value());

    expectInvalid(detectAndroidMotionPhotoV1(path, legacyPixelXmp(0)), QStringLiteral("zero"));
    expectInvalid(
        detectAndroidMotionPhotoV1(path, legacyPixelXmp(quint64(primary.size() + video.size()))),
        QStringLiteral("consumes"));
    expectInvalid(
        detectAndroidMotionPhotoV1(
            path, legacyPixelXmp(quint64(video.size()), QStringLiteral("GCamera"), qint64(2))),
        QStringLiteral("version"));
    expectInvalid(detectAndroidMotionPhotoV1(path, legacyPixelXmp(quint64(video.size()),
                                                                  QStringLiteral("GCamera"),
                                                                  qint64(1), 1, -2)),
                  QStringLiteral("below -1"));

    const QString nonJpegPath = writeFile(directory, QByteArray(40, 'P') + video);
    QVERIFY(!nonJpegPath.isEmpty());
    expectInvalid(detectAndroidMotionPhotoV1(nonJpegPath, legacyPixelXmp(quint64(video.size()))),
                  QStringLiteral("JPEG"));

    const QString stalePath = writeFile(directory, primary + QByteArray(video.size(), 'x'));
    QVERIFY(!stalePath.isEmpty());
    expectInvalid(detectAndroidMotionPhotoV1(stalePath, legacyPixelXmp(quint64(video.size()))),
                  QStringLiteral("ftyp"));
}

void AndroidMotionPhotoTest::modernDirectoryTakesPrecedenceOverLegacyOffset()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QString path = writeFile(directory, QByteArray(32, 'P') + video);
    QVERIFY(!path.isEmpty());

    const QByteArray xmp =
        xmpFor(QStringLiteral("image/jpeg"), quint64(video.size()), std::nullopt,
               QStringLiteral("video/mp4"),
               QStringLiteral("GCamera:MicroVideo=\"1\" "
                              "GCamera:MicroVideoVersion=\"1\" "
                              "GCamera:MicroVideoOffset=\"18446744073709551615\""));
    const auto result = detectAndroidMotionPhotoV1(path, xmp);
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    QCOMPARE(result.metadata->videoRange.offset, quint64(32));
    QCOMPARE(result.metadata->videoRange.length, quint64(video.size()));
}

void AndroidMotionPhotoTest::modernDeclarationDoesNotFallBackToLegacy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QString path = writeFile(directory, QByteArray(32, 'P') + video);
    QVERIFY(!path.isEmpty());

    QByteArray xmp = xmpFor(QStringLiteral("image/jpeg"), quint64(video.size()), std::nullopt,
                            QStringLiteral("video/mp4"),
                            QStringLiteral("GCamera:MicroVideo=\"1\" "
                                           "GCamera:MicroVideoVersion=\"1\" "
                                           "GCamera:MicroVideoOffset=\"%1\"")
                                .arg(video.size()));
    xmp.replace("GCamera:MotionPhotoVersion=\"1\"", "GCamera:MotionPhotoVersion=\"2\"");
    expectInvalid(detectAndroidMotionPhotoV1(path, xmp), QStringLiteral("version"));
}

void AndroidMotionPhotoTest::samsungSeftMotionPhotoUsesIndexedRecordRange()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QByteArray stillRecord = samsungStillMetadataContent();
    const QByteArray motionRecord = samsungMotionContent(video);
    const QByteArray bytes = samsungSeftJpeg({
        {0x0a01, stillRecord},
        {0x0a30, motionRecord},
    });
    const QString path = writeFile(directory, bytes);
    QVERIFY(!path.isEmpty());

    const auto result =
        detectSamsungMotionPhotoJpegSeft(path, legacyPixelXmp(quint64(video.size()) + 56));
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    const auto& metadata = *result.metadata;
    const quint64 expectedVideoOffset = quint64(4 + stillRecord.size() + 24);
    QCOMPARE(metadata.primaryMimeType, QStringLiteral("image/jpeg"));
    QCOMPARE(metadata.videoMimeType, QStringLiteral("video/mp4"));
    QCOMPARE(metadata.videoRange.offset, expectedVideoOffset);
    QCOMPARE(metadata.videoRange.length, quint64(video.size()));
    QVERIFY(metadata.presentationTimestampUs.has_value());
    QCOMPARE(*metadata.presentationTimestampUs, qint64(1331607));

    const quint64 legacyOffsetIncludingSeft = quint64(bytes.size()) - expectedVideoOffset;
    const auto googleCompatibilityPath =
        detectAndroidMotionPhotoV1(path, legacyPixelXmp(legacyOffsetIncludingSeft));
    QCOMPARE(googleCompatibilityPath.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(googleCompatibilityPath.metadata.has_value());
    QCOMPARE(googleCompatibilityPath.metadata->videoRange.offset, expectedVideoOffset);
    QCOMPARE(googleCompatibilityPath.metadata->videoRange.length, quint64(video.size()));

    ByteRangeDevice range(path, metadata.videoRange.offset, metadata.videoRange.length);
    QVERIFY(range.open(QIODevice::ReadOnly));
    QCOMPARE(range.readAll(), video);
}

void AndroidMotionPhotoTest::modernSamsungSeftCoLocatedRangeTrimsVendorDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QByteArray utcRecord = samsungStillMetadataContent();
    const QByteArray mccRecord =
        samsungSeftContent(0x0aa1, QByteArrayLiteral("MCC_Data"), QByteArrayLiteral("x"));
    const QByteArray captureRecord = samsungSeftContent(
        0x0c61, QByteArrayLiteral("Camera_Capture_Mode_Info"), QByteArrayLiteral("x"));
    const QByteArray motionRecord = samsungMotionContent(video);
    const QByteArray bytes = samsungSeftJpeg(
        {
            {0x0a01, utcRecord},
            {0x0aa1, mccRecord},
            {0x0c61, captureRecord},
            {0x0a30, motionRecord},
        },
        107);

    const quint64 expectedVideoOffset =
        quint64(4 + utcRecord.size() + mccRecord.size() + captureRecord.size() + 24);
    const quint64 modernDeclaredLength = quint64(bytes.size()) - expectedVideoOffset;
    QCOMPARE(modernDeclaredLength - quint64(video.size()), quint64(68));

    const QString path = writeFile(directory, bytes);
    QVERIFY(!path.isEmpty());
    const auto result = detectAndroidMotionPhotoV1(
        path, xmpFor(QStringLiteral("image/jpeg"), modernDeclaredLength));
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    QCOMPARE(result.metadata->videoRange.offset, expectedVideoOffset);
    QCOMPARE(result.metadata->videoRange.length, quint64(video.size()));
}

void AndroidMotionPhotoTest::modernSamsungSeftDifferentStartRemainsModern()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    QByteArray bytes = samsungSeftJpeg(
        {
            {0x0a30, samsungMotionContent(video)},
        },
        107);
    bytes.insert(4, video);

    const quint64 modernOffset = 4;
    const quint64 modernDeclaredLength = quint64(bytes.size()) - modernOffset;
    const QString path = writeFile(directory, bytes);
    QVERIFY(!path.isEmpty());

    const auto result = detectAndroidMotionPhotoV1(
        path, xmpFor(QStringLiteral("image/jpeg"), modernDeclaredLength));
    QCOMPARE(result.status, AndroidMotionPhotoStatus::Valid);
    QVERIFY(result.metadata.has_value());
    QCOMPARE(result.metadata->videoRange.offset, modernOffset);
    QCOMPARE(result.metadata->videoRange.length, modernDeclaredLength);
}

void AndroidMotionPhotoTest::samsungSeftWithoutMotionRecordStaysStill()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray bytes = samsungSeftJpeg({
        {0x0a01, samsungStillMetadataContent()},
    });
    const QString path = writeFile(directory, bytes);
    QVERIFY(!path.isEmpty());

    const auto result = detectSamsungMotionPhotoJpegSeft(path);
    QCOMPARE(result.status, AndroidMotionPhotoStatus::NotMotionPhoto);
    QVERIFY(!result.metadata.has_value());
    QVERIFY(result.error.isEmpty());
}

void AndroidMotionPhotoTest::samsungSeftMalformedIndexAndPayloadAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QByteArray motionRecord = samsungMotionContent(video);
    const QByteArray valid = samsungSeftJpeg({{0x0a30, motionRecord}});

    auto writeAndDetect = [&](const QByteArray& bytes) {
        const QString path = writeFile(directory, bytes);
        return detectSamsungMotionPhotoJpegSeft(path);
    };

    QByteArray duplicate = samsungSeftJpeg({
        {0x0a30, motionRecord},
        {0x0a30, motionRecord},
    });
    expectInvalid(writeAndDetect(duplicate), QStringLiteral("multiple"));

    QByteArray badOffset = valid;
    const quint32 headerLength = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(badOffset.constData() + badOffset.size() - 8));
    const qsizetype headerStart = badOffset.size() - 8 - qsizetype(headerLength);
    qToLittleEndian<quint32>(quint32(headerStart + 1),
                             reinterpret_cast<uchar*>(badOffset.data() + headerStart + 16));
    expectInvalid(writeAndDetect(badOffset), QStringLiteral("offset"));

    QByteArray badName = valid;
    const qsizetype nameOffset = badName.indexOf(QByteArrayLiteral("MotionPhoto_Data"));
    QVERIFY(nameOffset >= 0);
    badName[nameOffset] = 'X';
    expectInvalid(writeAndDetect(badName), QStringLiteral("content header"));

    QByteArray badVideo = valid;
    const qsizetype ftyp = badVideo.indexOf(QByteArrayLiteral("ftyp"));
    QVERIFY(ftyp >= 0);
    badVideo.replace(ftyp, 4, QByteArrayLiteral("nope"));
    expectInvalid(writeAndDetect(badVideo), QStringLiteral("ftyp"));

    QByteArray hugeCount = valid;
    qToLittleEndian<quint32>(4097, reinterpret_cast<uchar*>(hugeCount.data() + headerStart + 8));
    expectInvalid(writeAndDetect(hugeCount), QStringLiteral("record count"));

    QByteArray noSeft = QByteArray::fromHex("ffd8ffd9");
    const auto none = writeAndDetect(noSeft);
    QCOMPARE(none.status, AndroidMotionPhotoStatus::NotMotionPhoto);
}

void AndroidMotionPhotoTest::residualFlagWithoutActualVideoIsRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QString path = writeFile(directory, QByteArray(40, 'J') + QByteArray(video.size(), 'x'));
    QVERIFY(!path.isEmpty());

    expectInvalid(detectAndroidMotionPhotoV1(
                      path, xmpFor(QStringLiteral("image/jpeg"), quint64(video.size()))),
                  QStringLiteral("ftyp"));

    // Recognized ISO BMFF box types must still obey the bounded range.
    QByteArray oversizedFtyp = video;
    qToBigEndian<quint32>(quint32(video.size() + 1024),
                          reinterpret_cast<uchar*>(oversizedFtyp.data()));
    const QString oversizedPath = writeFile(directory, QByteArray(40, 'J') + oversizedFtyp);
    QVERIFY(!oversizedPath.isEmpty());
    expectInvalid(detectAndroidMotionPhotoV1(oversizedPath, xmpFor(QStringLiteral("image/jpeg"),
                                                                   quint64(oversizedFtyp.size()))),
                  QStringLiteral("bounded video range"));
}

void AndroidMotionPhotoTest::nonOneMotionPhotoFlagIsNotMotionPhoto()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QString path = writeFile(directory, QByteArray(40, 'J') + video);
    QVERIFY(!path.isEmpty());

    QByteArray xmp = xmpFor(QStringLiteral("image/jpeg"), quint64(video.size()));
    xmp.replace("GCamera:MotionPhoto=\"1\"", "GCamera:MotionPhoto=\"2\"");
    const auto result = detectAndroidMotionPhotoV1(path, xmp);
    QCOMPARE(result.status, AndroidMotionPhotoStatus::NotMotionPhoto);
    QVERIFY(!result.metadata.has_value());
    QVERIFY(result.error.isEmpty());
}

void AndroidMotionPhotoTest::directoryRulesAndLengthOverflowAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QString path = writeFile(directory, QByteArray(80, 'J') + video);
    QVERIFY(!path.isEmpty());

    const QString huge = QString::number(std::numeric_limits<quint64>::max());
    const QString middle = QStringLiteral(R"XMP(
<rdf:li rdf:parseType="Resource"><Container:Item
 Item:Mime="image/jpeg" Item:Semantic="GainMap" Item:Length="%1"/></rdf:li>)XMP")
                               .arg(huge);
    expectInvalid(detectAndroidMotionPhotoV1(path, xmpFor(QStringLiteral("image/jpeg"),
                                                          quint64(video.size()), std::nullopt,
                                                          QStringLiteral("video/mp4"), {}, middle)),
                  QStringLiteral("overflow"));

    QByteArray notLast = xmpFor(QStringLiteral("image/jpeg"), quint64(video.size()));
    notLast.replace("</rdf:Seq>", "<rdf:li rdf:parseType=\"Resource\"><Container:Item "
                                  "Item:Mime=\"image/jpeg\" Item:Semantic=\"GainMap\" "
                                  "Item:Length=\"0\"/></rdf:li></rdf:Seq>");
    expectInvalid(detectAndroidMotionPhotoV1(path, notLast), QStringLiteral("final"));
}

void AndroidMotionPhotoTest::heicAndAvifRequireEightByteMpvdHeader()
{
    const QByteArray video = ftypVideo();
    for (const QString& mime : {QStringLiteral("image/heic"), QStringLiteral("image/avif")}) {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray primary(48, 'I');
        const QString path =
            writeFile(directory, primary + mpvdHeader(quint64(video.size())) + video);
        QVERIFY(!path.isEmpty());

        const auto valid =
            detectAndroidMotionPhotoV1(path, xmpFor(mime, quint64(video.size()), quint64(8)));
        QCOMPARE(valid.status, AndroidMotionPhotoStatus::Valid);
        QVERIFY(valid.metadata.has_value());
        QCOMPARE(valid.metadata->primaryImageLength, quint64(primary.size()));
        QCOMPARE(valid.metadata->primaryPadding, quint64(8));
        QCOMPARE(valid.metadata->videoRange.offset, quint64(primary.size() + 8));

        const QByteArray zeroSecondaryPadding =
            withSecondaryMotionPadding(xmpFor(mime, quint64(video.size()), quint64(8)), 0);
        QVERIFY(!zeroSecondaryPadding.isEmpty());
        const auto zeroPaddingValid = detectAndroidMotionPhotoV1(path, zeroSecondaryPadding);
        QCOMPARE(zeroPaddingValid.status, AndroidMotionPhotoStatus::Valid);
        QVERIFY(zeroPaddingValid.metadata.has_value());
        QCOMPARE(zeroPaddingValid.metadata->videoRange.offset, valid.metadata->videoRange.offset);
        QCOMPARE(zeroPaddingValid.metadata->videoRange.length, valid.metadata->videoRange.length);

        const QByteArray nonZeroSecondaryPadding =
            withSecondaryMotionPadding(xmpFor(mime, quint64(video.size()), quint64(8)), 1);
        QVERIFY(!nonZeroSecondaryPadding.isEmpty());
        expectInvalid(detectAndroidMotionPhotoV1(path, nonZeroSecondaryPadding),
                      QStringLiteral("secondary"));

        expectInvalid(
            detectAndroidMotionPhotoV1(path, xmpFor(mime, quint64(video.size()), quint64(0))),
            QStringLiteral("Padding"));

        QByteArray broken = primary + QByteArray("\0\0\0\x20nope", 8) + video;
        const QString brokenPath = writeFile(directory, broken);
        QVERIFY(!brokenPath.isEmpty());
        expectInvalid(
            detectAndroidMotionPhotoV1(brokenPath, xmpFor(mime, quint64(video.size()), quint64(8))),
            QStringLiteral("mpvd"));

        // Samsung HEIC stores a top-level sefd metadata box after mpvd but
        // includes that sefd box in the MotionPhoto Item:Length. The adapter
        // validates the exact layout and trims playback to the mpvd payload.
        const QByteArray sefd = sizedBox(QByteArrayLiteral("sefd"), 24);
        QVERIFY(!sefd.isEmpty());
        const QByteArray samsungLayout = primary + mpvdHeader(quint64(video.size())) + video + sefd;
        const QString samsungPath = writeFile(directory, samsungLayout);
        QVERIFY(!samsungPath.isEmpty());
        const quint64 samsungDeclaredLength = quint64(video.size() + sefd.size());
        const QByteArray samsungXmp =
            withSecondaryMotionPadding(xmpFor(mime, samsungDeclaredLength, quint64(8)), 0);
        QVERIFY(!samsungXmp.isEmpty());

        if (mime == QStringLiteral("image/heic")) {
            const auto samsungValid = detectAndroidMotionPhotoV1(samsungPath, samsungXmp);
            QCOMPARE(samsungValid.status, AndroidMotionPhotoStatus::Valid);
            QVERIFY(samsungValid.metadata.has_value());
            QCOMPARE(samsungValid.metadata->videoRange.offset, quint64(primary.size() + 8));
            QCOMPARE(samsungValid.metadata->videoRange.length, quint64(video.size()));

            QByteArray wrongType = sefd;
            wrongType.replace(4, 4, QByteArrayLiteral("free"));
            const QString wrongTypePath = writeFile(
                directory, primary + mpvdHeader(quint64(video.size())) + video + wrongType);
            QVERIFY(!wrongTypePath.isEmpty());
            expectInvalid(detectAndroidMotionPhotoV1(wrongTypePath, samsungXmp),
                          QStringLiteral("sefd"));

            QByteArray wrongSize = sefd;
            qToBigEndian<quint32>(quint32(sefd.size() - 1),
                                  reinterpret_cast<uchar*>(wrongSize.data()));
            const QString wrongSizePath = writeFile(
                directory, primary + mpvdHeader(quint64(video.size())) + video + wrongSize);
            QVERIFY(!wrongSizePath.isEmpty());
            expectInvalid(detectAndroidMotionPhotoV1(wrongSizePath, samsungXmp),
                          QStringLiteral("sefd"));
        } else {
            // The Samsung exception is intentionally HEIC-only. AVIF remains
            // strict to the Android 1.0 mpvd-size rule.
            expectInvalid(detectAndroidMotionPhotoV1(samsungPath, samsungXmp),
                          QStringLiteral("mpvd"));
        }
    }
}

void AndroidMotionPhotoTest::oversizedOrEntityBearingXmpIsRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = writeFile(directory, QByteArray(64, 'x'));
    QVERIFY(!path.isEmpty());

    const QByteArray oversized(1024 * 1024 + 1, 'x');
    expectInvalid(detectAndroidMotionPhotoV1(path, oversized), QStringLiteral("1 MiB"));

    const QByteArray dtd = QByteArrayLiteral(
        "<!DOCTYPE x [<!ENTITY e \"boom\">]><x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "&e;</x:xmpmeta>");
    expectInvalid(detectAndroidMotionPhotoV1(path, dtd), QStringLiteral("DTD"));
}

void AndroidMotionPhotoTest::cancellationStopsVideoValidation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray video = ftypVideo();
    const QString path = writeFile(directory, QByteArray(40, 'J') + video);
    QVERIFY(!path.isEmpty());

    std::atomic_bool cancelled{true};
    expectInvalid(
        detectAndroidMotionPhotoV1(
            path, xmpFor(QStringLiteral("image/jpeg"), quint64(video.size())), &cancelled),
        QStringLiteral("cancelled"));
}

QTEST_MAIN(AndroidMotionPhotoTest)
