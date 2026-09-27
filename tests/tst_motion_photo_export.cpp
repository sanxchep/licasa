#include "export/motion_photo_export.h"

#include "io/external_file_identity.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Licasa;

namespace {

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

PhotoAssetInfo embeddedAsset(const QString& path, quint64 offset, quint64 length,
                             const QString& mime = QStringLiteral("video/mp4"))
{
    PhotoAssetInfo asset;
    asset.kind = PhotoAssetKind::MotionPhoto;
    asset.sourceUrl = QUrl::fromLocalFile(path);
    MotionComponent motion;
    motion.embedded = CheckedByteRange{offset, length};
    motion.mimeType = mime;
    asset.motion = motion;
    return asset;
}

PhotoAssetInfo externalAsset(const QString& stillPath, const QString& moviePath,
                             const ExternalFileIdentity& identity)
{
    PhotoAssetInfo asset;
    asset.kind = PhotoAssetKind::MotionPhoto;
    asset.sourceUrl = QUrl::fromLocalFile(stillPath);
    MotionComponent motion;
    motion.mimeType = QStringLiteral("video/quicktime");
    motion.externalVideoUrl = QUrl::fromLocalFile(moviePath);
    motion.externalVideoIdentity = identity;
    asset.motion = motion;
    return asset;
}

std::optional<ExternalFileIdentity> identityForPath(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return externalFileIdentity(file);
}

} // namespace

class MotionPhotoExportTest final : public QObject {
    Q_OBJECT

  private slots:
    void embeddedCopyContainsOnlyValidatedMotionRange()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("phone.jpg"));
        const QByteArray still = QByteArrayLiteral("JPEG-STILL-PREFIX-");
        const QByteArray video = QByteArrayLiteral("00000020ftypisomMOTION-BYTES-ONLY");
        const QByteArray trailer = QByteArrayLiteral("TRAILER-NOT-VIDEO");
        const QByteArray original = still + video + trailer;
        QVERIFY(writeBytes(sourcePath, original));

        const PhotoAssetInfo asset =
            embeddedAsset(sourcePath, quint64(still.size()), quint64(video.size()));
        const QString destinationPath = dir.filePath(QStringLiteral("motion.mp4"));
        const MotionPhotoExportResult result =
            copyMotionPhotoComponent(asset, QUrl::fromLocalFile(destinationPath));

        QVERIFY2(result.errorMessage.isEmpty(), qPrintable(result.errorMessage));
        QCOMPARE(result.destinationUrl, QUrl::fromLocalFile(destinationPath));
        QCOMPARE(readBytes(destinationPath), video);
        QCOMPARE(readBytes(sourcePath), original);
    }

    void externalCopyRequiresPinnedIdentity()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString stillPath = dir.filePath(QStringLiteral("IMG_0001.HEIC"));
        const QString moviePath = dir.filePath(QStringLiteral("IMG_0001.MOV"));
        const QByteArray still = QByteArrayLiteral("still");
        const QByteArray movie = QByteArrayLiteral("00000020ftypqt  PINNED-MOV-BYTES");
        QVERIFY(writeBytes(stillPath, still));
        QVERIFY(writeBytes(moviePath, movie));
        const auto identity = identityForPath(moviePath);
        QVERIFY(identity.has_value());

        const PhotoAssetInfo asset = externalAsset(stillPath, moviePath, *identity);
        const QString destinationPath = dir.filePath(QStringLiteral("export.mov"));
        const MotionPhotoExportResult result =
            copyMotionPhotoComponent(asset, QUrl::fromLocalFile(destinationPath));

        QVERIFY2(result.errorMessage.isEmpty(), qPrintable(result.errorMessage));
        QCOMPARE(readBytes(destinationPath), movie);
        QCOMPARE(readBytes(moviePath), movie);
    }

    void externalReplacementIsRejectedBeforeCopy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString stillPath = dir.filePath(QStringLiteral("IMG_0002.HEIC"));
        const QString moviePath = dir.filePath(QStringLiteral("IMG_0002.MOV"));
        QVERIFY(writeBytes(stillPath, QByteArrayLiteral("still")));
        QVERIFY(writeBytes(moviePath, QByteArrayLiteral("ORIGINAL-MOV")));
        const auto identity = identityForPath(moviePath);
        QVERIFY(identity.has_value());
        const PhotoAssetInfo asset = externalAsset(stillPath, moviePath, *identity);

        const QString movedPath = dir.filePath(QStringLiteral("old.mov"));
        QVERIFY(QFile::rename(moviePath, movedPath));
        QVERIFY(writeBytes(moviePath, QByteArrayLiteral("REPLACEMENT")));

        const QString destinationPath = dir.filePath(QStringLiteral("export.mov"));
        const MotionPhotoExportResult result =
            copyMotionPhotoComponent(asset, QUrl::fromLocalFile(destinationPath));

        QVERIFY(!result.errorMessage.isEmpty());
        QVERIFY(!QFileInfo::exists(destinationPath));
        QCOMPARE(readBytes(movedPath), QByteArrayLiteral("ORIGINAL-MOV"));
    }

    void externalDestinationCannotAliasPrimaryStill()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString stillPath = dir.filePath(QStringLiteral("IMG_0003.HEIC"));
        const QString moviePath = dir.filePath(QStringLiteral("IMG_0003.MOV"));
        const QString stillAliasPath = dir.filePath(QStringLiteral("still-alias.mov"));
        const QByteArray still = QByteArrayLiteral("PRIMARY-STILL-BYTES");
        const QByteArray movie = QByteArrayLiteral("PINNED-MOV-BYTES");
        QVERIFY(writeBytes(stillPath, still));
        QVERIFY(writeBytes(moviePath, movie));
        QVERIFY(QFile::link(stillPath, stillAliasPath));
        const auto identity = identityForPath(moviePath);
        QVERIFY(identity.has_value());

        const PhotoAssetInfo asset = externalAsset(stillPath, moviePath, *identity);
        const MotionPhotoExportResult result =
            copyMotionPhotoComponent(asset, QUrl::fromLocalFile(stillAliasPath));

        QVERIFY(!result.errorMessage.isEmpty());
        QCOMPARE(readBytes(stillPath), still);
        QCOMPARE(readBytes(moviePath), movie);
    }

    void destinationCannotAliasSource()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("source.mp4"));
        const QByteArray bytes = QByteArrayLiteral("MOTION-COMPONENT");
        QVERIFY(writeBytes(sourcePath, bytes));
        const PhotoAssetInfo asset = embeddedAsset(sourcePath, 0, quint64(bytes.size()));

        const MotionPhotoExportResult result =
            copyMotionPhotoComponent(asset, QUrl::fromLocalFile(sourcePath));
        QVERIFY(!result.errorMessage.isEmpty());
        QCOMPARE(readBytes(sourcePath), bytes);
    }

    void destinationExtensionMustMatchCopiedPayload()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("phone.jpg"));
        const QByteArray bytes = QByteArrayLiteral("MOTION-COMPONENT");
        QVERIFY(writeBytes(sourcePath, bytes));
        const PhotoAssetInfo asset = embeddedAsset(sourcePath, 0, quint64(bytes.size()));
        const QString destinationPath = dir.filePath(QStringLiteral("wrong.mov"));

        const MotionPhotoExportResult result =
            copyMotionPhotoComponent(asset, QUrl::fromLocalFile(destinationPath));
        QVERIFY(!result.errorMessage.isEmpty());
        QVERIFY(!QFileInfo::exists(destinationPath));
    }

    void serviceReprobesInsteadOfTrustingCachedRange()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("ordinary.jpg"));
        QVERIFY(writeBytes(sourcePath, QByteArray::fromHex("ffd8ffd9")));

        MotionPhotoExportService service;
        service.setAsset(embeddedAsset(sourcePath, 0, 4));
        QVERIFY(service.available());
        QCOMPARE(service.suggestedSuffix(), QStringLiteral("mp4"));

        QSignalSpy completed(&service, &MotionPhotoExportService::exportCompleted);
        QSignalSpy failed(&service, &MotionPhotoExportService::exportFailed);
        QSignalSpy busy(&service, &MotionPhotoExportService::busyChanged);

        const QString destinationPath = dir.filePath(QStringLiteral("must-not-exist.mp4"));
        QVERIFY(service.exportCopy(QUrl::fromLocalFile(destinationPath)));
        QVERIFY(service.busy());
        QVERIFY(failed.wait(5000));

        QCOMPARE(completed.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(busy.count() >= 2);
        QVERIFY(!service.busy());
        QVERIFY(!QFileInfo::exists(destinationPath));
    }
};

QTEST_GUILESS_MAIN(MotionPhotoExportTest)
#include "tst_motion_photo_export.moc"
