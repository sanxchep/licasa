#include "media/live_photo_pairing.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <atomic>

using namespace Licasa;

namespace {

void append16(QByteArray* bytes, quint16 value)
{
    char raw[2];
    qToBigEndian<quint16>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 2);
}

void append32(QByteArray* bytes, quint32 value)
{
    char raw[4];
    qToBigEndian<quint32>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 4);
}

QByteArray box(quint32 type, const QByteArray& payload)
{
    QByteArray result;
    append32(&result, quint32(payload.size() + 8));
    append32(&result, type);
    result += payload;
    return result;
}

constexpr quint32 fourCc(char a, char b, char c, char d) noexcept
{
    return (quint32(quint8(a)) << 24) | (quint32(quint8(b)) << 16) | (quint32(quint8(c)) << 8) |
           quint32(quint8(d));
}

QByteArray appleMakerNote(const QByteArray& identifier)
{
    QByteArray maker("Apple iOS\0", 10);
    maker += QByteArray::fromHex("00014d4d"); // Apple header + MM marker; IFD starts at 14.
    append16(&maker, 1);
    append16(&maker, 0x0011);
    append16(&maker, 2); // ASCII
    append32(&maker, quint32(identifier.size() + 1));
    append32(&maker, 32); // relative to MakerNote start
    append32(&maker, 0);  // next IFD
    maker += identifier;
    maker += '\0';
    return maker;
}

QByteArray tiffWithAppleIdentifier(const QByteArray& identifier)
{
    const QByteArray maker = appleMakerNote(identifier);
    QByteArray tiff("MM\0*", 4);
    append32(&tiff, 8); // IFD0
    append16(&tiff, 1);
    append16(&tiff, 0x8769); // ExifIFDPointer
    append16(&tiff, 4);      // LONG
    append32(&tiff, 1);
    append32(&tiff, 26);
    append32(&tiff, 0);
    append16(&tiff, 1);
    append16(&tiff, 0x927c); // MakerNote
    append16(&tiff, 7);      // UNDEFINED
    append32(&tiff, quint32(maker.size()));
    append32(&tiff, 44);
    append32(&tiff, 0);
    tiff += maker;
    return tiff;
}

QByteArray jpegWithAppleIdentifier(const QByteArray& identifier)
{
    QByteArray payload("Exif\0\0", 6);
    payload += tiffWithAppleIdentifier(identifier);
    if (payload.size() + 2 > 0xffff) {
        return {};
    }

    QByteArray jpeg = QByteArray::fromHex("ffd8ffe1");
    append16(&jpeg, quint16(payload.size() + 2));
    jpeg += payload;
    jpeg += QByteArray::fromHex("ffd9");
    return jpeg;
}

QByteArray movieWithContentIdentifier(const QByteArray& identifier, bool includeIdentifier = true)
{
    const QByteArray key = includeIdentifier
                               ? QByteArrayLiteral("com.apple.quicktime.content.identifier")
                               : QByteArrayLiteral("com.example.other");

    QByteArray keyEntry;
    append32(&keyEntry, quint32(key.size() + 8));
    append32(&keyEntry, fourCc('m', 'd', 't', 'a'));
    keyEntry += key;

    QByteArray keysPayload(4, '\0'); // FullBox
    append32(&keysPayload, 1);
    keysPayload += keyEntry;

    QByteArray dataPayload;
    append32(&dataPayload, 1); // UTF-8 metadata type
    append32(&dataPayload, 0); // locale
    dataPayload += identifier;
    const QByteArray data = box(fourCc('d', 'a', 't', 'a'), dataPayload);
    const QByteArray item = box(1, data); // one-based key index

    QByteArray metaPayload(4, '\0'); // FullBox
    metaPayload += box(fourCc('k', 'e', 'y', 's'), keysPayload);
    metaPayload += box(fourCc('i', 'l', 's', 't'), item);
    return box(fourCc('m', 'o', 'o', 'v'), box(fourCc('m', 'e', 't', 'a'), metaPayload));
}

QByteArray quickTimeMovieWithContentIdentifier(const QByteArray& identifier,
                                               quint32 handlerType = fourCc('m', 'd', 't', 'a'),
                                               const QByteArray& trailingBytes = {})
{
    const QByteArray key = QByteArrayLiteral("com.apple.quicktime.content.identifier");

    QByteArray keyEntry;
    append32(&keyEntry, quint32(key.size() + 8));
    append32(&keyEntry, fourCc('m', 'd', 't', 'a'));
    keyEntry += key;

    QByteArray keysPayload(4, '\0'); // keys FullBox version + flags
    append32(&keysPayload, 1);
    keysPayload += keyEntry;

    QByteArray dataPayload;
    append32(&dataPayload, 1); // UTF-8 metadata type
    append32(&dataPayload, 0); // locale
    dataPayload += identifier;
    const QByteArray data = box(fourCc('d', 'a', 't', 'a'), dataPayload);
    const QByteArray item = box(1, data);

    QByteArray handlerPayload(8, '\0'); // version/flags + pre_defined
    append32(&handlerPayload, handlerType);
    handlerPayload += QByteArray(14, '\0');

    QByteArray metaPayload; // QuickTime-style meta: no outer FullBox header
    metaPayload += box(fourCc('h', 'd', 'l', 'r'), handlerPayload);
    metaPayload += box(fourCc('k', 'e', 'y', 's'), keysPayload);
    metaPayload += box(fourCc('i', 'l', 's', 't'), item);
    metaPayload += trailingBytes;
    return box(fourCc('m', 'o', 'o', 'v'), box(fourCc('m', 'e', 't', 'a'), metaPayload));
}

QString write(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

} // namespace

class LivePhotoPairingTest final : public QObject {
    Q_OBJECT

  private slots:
    void matchingJpegAndMovAreValidated()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray identifier("8B86D93B-2ED8-4A70-927D-4C3287F920A1");
        const QString still =
            write(directory, QStringLiteral("IMG_1234.JPG"), jpegWithAppleIdentifier(identifier));
        const QString movie = write(directory, QStringLiteral("IMG_1234.MOV"),
                                    movieWithContentIdentifier(identifier));
        QVERIFY(!still.isEmpty());
        QVERIFY(!movie.isEmpty());
        QVERIFY(hasAppleLivePhotoMovieCandidate(still));

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Valid));
        QVERIFY(detection.metadata.has_value());
        QCOMPARE(detection.metadata->videoUrl, QUrl::fromLocalFile(movie));
        QCOMPARE(detection.metadata->contentIdentifier, QString::fromLatin1(identifier));
        QCOMPARE(detection.metadata->videoIdentity.size, quint64(QFileInfo(movie).size()));
        QVERIFY(detection.error.isEmpty());
    }

    void quickTimeMetaWithoutFullBoxIsAccepted()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray identifier("9079230C-42A6-48F4-A743-6B456E207646");
        const QString still =
            write(directory, QStringLiteral("IMG_QT.JPG"), jpegWithAppleIdentifier(identifier));
        const QString movie = write(directory, QStringLiteral("IMG_QT.MOV"),
                                    quickTimeMovieWithContentIdentifier(identifier));
        QVERIFY(!still.isEmpty());
        QVERIFY(!movie.isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Valid));
        QVERIFY(detection.metadata.has_value());
        QCOMPARE(detection.metadata->contentIdentifier, QString::fromLatin1(identifier));
        QCOMPARE(detection.metadata->videoUrl, QUrl::fromLocalFile(movie));
        QVERIFY(detection.error.isEmpty());
    }

    void quickTimeMetaWithoutFullBoxRequiresMdtaHandler()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray identifier("STRICT-QUICKTIME-META");
        const QString still =
            write(directory, QStringLiteral("IMG_QT_BAD.JPG"), jpegWithAppleIdentifier(identifier));
        QVERIFY(!still.isEmpty());
        QVERIFY(!write(directory, QStringLiteral("IMG_QT_BAD.MOV"),
                       quickTimeMovieWithContentIdentifier(identifier, fourCc('v', 'i', 'd', 'e')))
                     .isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Invalid));
        QVERIFY(!detection.metadata.has_value());
        QVERIFY(!detection.error.isEmpty());
    }

    void quickTimeMetaWithoutFullBoxRejectsTrailingGarbage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray identifier("STRICT-QUICKTIME-TRAILING");
        const QString still = write(directory, QStringLiteral("IMG_QT_TRAIL.JPG"),
                                    jpegWithAppleIdentifier(identifier));
        QVERIFY(!still.isEmpty());
        QVERIFY(!write(directory, QStringLiteral("IMG_QT_TRAIL.MOV"),
                       quickTimeMovieWithContentIdentifier(identifier, fourCc('m', 'd', 't', 'a'),
                                                           QByteArrayLiteral("x")))
                     .isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Invalid));
        QVERIFY(!detection.metadata.has_value());
        QVERIFY(!detection.error.isEmpty());
    }

    void sameBasenameIsOnlyACandidate()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString still =
            write(directory, QStringLiteral("IMG_2000.jpg"),
                  jpegWithAppleIdentifier("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
        QVERIFY(!still.isEmpty());
        QVERIFY(!write(directory, QStringLiteral("IMG_2000.mov"),
                       movieWithContentIdentifier("11111111-2222-3333-4444-555555555555"))
                     .isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::NotPaired));
        QVERIFY(!detection.metadata.has_value());
    }

    void movieWithoutAppleKeyIsNotPromoted()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray identifier("01234567-89AB-CDEF-0123-456789ABCDEF");
        const QString still =
            write(directory, QStringLiteral("IMG_3000.jpeg"), jpegWithAppleIdentifier(identifier));
        QVERIFY(!still.isEmpty());
        QVERIFY(!write(directory, QStringLiteral("IMG_3000.MOV"),
                       movieWithContentIdentifier(identifier, false))
                     .isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::NotPaired));
        QVERIFY(!detection.metadata.has_value());
    }

    void noSameBasenameMovieDoesNoPairWork()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString still =
            write(directory, QStringLiteral("IMG_4000.JPG"), jpegWithAppleIdentifier("A-B-C"));
        QVERIFY(!still.isEmpty());
        QVERIFY(!write(directory, QStringLiteral("OTHER.MOV"), movieWithContentIdentifier("A-B-C"))
                     .isEmpty());
        QVERIFY(!hasAppleLivePhotoMovieCandidate(still));

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::NoCandidate));
        QVERIFY(!detection.metadata.has_value());
    }

    void malformedMovieBoxFailsSafely()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString still =
            write(directory, QStringLiteral("IMG_5000.JPG"), jpegWithAppleIdentifier("SAFE-ID"));
        QVERIFY(!still.isEmpty());
        QByteArray movie;
        append32(&movie, 4096); // box exceeds actual EOF
        append32(&movie, fourCc('m', 'o', 'o', 'v'));
        movie += QByteArray(16, '\0');
        QVERIFY(!write(directory, QStringLiteral("IMG_5000.MOV"), movie).isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Invalid));
        QVERIFY(!detection.error.isEmpty());
    }

    void malformedMakerNoteOffsetFailsSafely()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QByteArray jpeg = jpegWithAppleIdentifier("OFFSET-ID");
        const qsizetype marker = jpeg.indexOf(QByteArrayLiteral("Apple iOS\0"));
        QVERIFY(marker >= 0);
        // Value offset field for the sole Apple IFD entry is +24 from maker start.
        qToBigEndian<quint32>(0xfffffff0u, reinterpret_cast<uchar*>(jpeg.data() + marker + 24));
        const QString still = write(directory, QStringLiteral("IMG_6000.JPG"), jpeg);
        QVERIFY(!still.isEmpty());
        QVERIFY(!write(directory, QStringLiteral("IMG_6000.MOV"),
                       movieWithContentIdentifier("OFFSET-ID"))
                     .isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Invalid));
        QVERIFY(!detection.error.isEmpty());
    }

    void cancellationReturnsWithoutPromotion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray identifier("CANCEL-ID");
        const QString still =
            write(directory, QStringLiteral("IMG_7000.JPG"), jpegWithAppleIdentifier(identifier));
        QVERIFY(!still.isEmpty());
        QVERIFY(!write(directory, QStringLiteral("IMG_7000.MOV"),
                       movieWithContentIdentifier(identifier))
                     .isEmpty());
        std::atomic_bool cancelled{true};

        const auto detection = detectAppleLivePhotoPair(still, &cancelled);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::NoCandidate));
        QVERIFY(!detection.metadata.has_value());
    }

    void heifExifOffsetPayloadIsParsed()
    {
        const QByteArray identifier("HEIC-APPLE-IDENTIFIER-1234");
        QByteArray metadata;
        append32(&metadata, 6); // bytes between offset field and TIFF header
        metadata += QByteArrayLiteral("Exif\0\0");
        metadata += tiffWithAppleIdentifier(identifier);

        const auto detection = appleLivePhotoIdentifierFromHeifExifBlock(metadata);
        QCOMPARE(int(detection.status), int(AppleLivePhotoIdentifierStatus::Valid));
        QCOMPARE(detection.contentIdentifier, QString::fromLatin1(identifier));
        QVERIFY(detection.error.isEmpty());

        QByteArray malformed = metadata;
        qToBigEndian<quint32>(quint32(metadata.size() + 100),
                              reinterpret_cast<uchar*>(malformed.data()));
        const auto rejected = appleLivePhotoIdentifierFromHeifExifBlock(malformed);
        QCOMPARE(int(rejected.status), int(AppleLivePhotoIdentifierStatus::Invalid));
        QVERIFY(!rejected.error.isEmpty());
    }

    void knownHeicIdentifierPairsWithMovie()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray identifier("D2B13633-0F08-44C9-8C04-29E12A4374B1");
        const QString still = write(directory, QStringLiteral("IMG_6000.HEIC"),
                                    QByteArrayLiteral("synthetic-heic-placeholder"));
        const QString movie = write(directory, QStringLiteral("IMG_6000.MOV"),
                                    movieWithContentIdentifier(identifier));
        QVERIFY(!still.isEmpty());
        QVERIFY(!movie.isEmpty());

        const auto detection =
            detectAppleLivePhotoPairForIdentifier(still, QString::fromLatin1(identifier));
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Valid));
        QVERIFY(detection.metadata.has_value());
        QCOMPARE(detection.metadata->videoUrl, QUrl::fromLocalFile(movie));
    }

    void heicConvenienceProbeRequiresBridgedIdentifier()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString still =
            write(directory, QStringLiteral("IMG_8000.HEIC"),
                  QByteArrayLiteral("heic-is-not-parsed-by-the-jpeg-convenience-probe"));
        QVERIFY(!still.isEmpty());
        QVERIFY(
            !write(directory, QStringLiteral("IMG_8000.MOV"), movieWithContentIdentifier("HEIC-ID"))
                 .isEmpty());

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::NotPaired));
        QVERIFY(!detection.metadata.has_value());
        QVERIFY(detection.error.isEmpty());
    }

    void generatedPlayableMovUsesRealMdtaLayout()
    {
#ifndef LICASA_APPLE_EXTERNAL_MOV_FIXTURE
        QSKIP("Configure the generated Apple external MOV fixture for qualification");
#else
        const QString fixture = QStringLiteral(LICASA_APPLE_EXTERNAL_MOV_FIXTURE);
        const QByteArray identifier(LICASA_APPLE_EXTERNAL_MOV_IDENTIFIER);
        QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString still =
            write(directory, QStringLiteral("IMG_9000.JPG"), jpegWithAppleIdentifier(identifier));
        const QString movie = directory.filePath(QStringLiteral("IMG_9000.MOV"));
        QVERIFY(!still.isEmpty());
        QVERIFY(QFile::copy(fixture, movie));

        const auto detection = detectAppleLivePhotoPair(still);
        QCOMPARE(int(detection.status), int(AppleLivePhotoPairStatus::Valid));
        QVERIFY(detection.metadata.has_value());
        QCOMPARE(detection.metadata->contentIdentifier, QString::fromLatin1(identifier));
        QCOMPARE(detection.metadata->videoUrl, QUrl::fromLocalFile(movie));
        QCOMPARE(detection.metadata->videoIdentity.size, quint64(QFileInfo(movie).size()));
        QVERIFY(detection.error.isEmpty());
#endif
    }
};

QTEST_MAIN(LivePhotoPairingTest)
#include "tst_live_photo_pairing.moc"
