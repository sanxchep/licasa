#include "app/application_settings.h"
#include "imaging/async_image_provider.h"
#include "imaging/image_resource_policy.h"
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
#include "imaging/fast_tiff_preview.h"
#include <tiffio.h>
#endif

#include <QFile>
#include <QImage>
#include <QQuickTextureFactory>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

class AsyncImageProviderTest final : public QObject {
    Q_OBJECT

  private slots:
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
    void largeGrayTiffPreviewStaysBoundedAndCancelable()
    {
        QTemporaryDir images;
        QVERIFY(images.isValid());
        const QString path = images.filePath(QStringLiteral("large-gray.tif"));
        const QByteArray encodedPath = QFile::encodeName(path);
        TIFF* tiff = TIFFOpen(encodedPath.constData(), "w");
        QVERIFY(tiff != nullptr);
        constexpr uint32_t width = 2048;
        constexpr uint32_t height = 1536;
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 3);
        TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        std::vector<quint16> row(width);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                row[x] = quint16(((x * 13 + y * 7) & 255) * 257);
            }
            QCOMPARE(TIFFWriteScanline(tiff, row.data(), y), 1);
        }
        TIFFClose(tiff);

        std::atomic_bool cancelled{false};
        const QImage preview =
            Licasa::readFastTiffPreview(path, QSize(1200, 900), 100000, 100000, &cancelled);
        QVERIFY(!preview.isNull());
        QCOMPARE(preview.format(), QImage::Format_Grayscale8);
        QVERIFY(preview.sizeInBytes() <= 100000);
        const int x = preview.width() / 2;
        const int y = preview.height() / 2;
        const int sourceX = int(quint64(x) * width / quint64(preview.width()));
        const int sourceY = int(quint64(y) * height / quint64(preview.height()));
        QCOMPARE(preview.pixelColor(x, y).red(), (sourceX * 13 + sourceY * 7) & 255);

        cancelled.store(true);
        QVERIFY(Licasa::readFastTiffPreview(path, QSize(1200, 900), 100000, 100000, &cancelled)
                    .isNull());
    }
#endif

    void nearbyPreviewPreparesWhileFullDecoderOwnsGate()
    {
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("neighbor.png"));
        QImage image(640, 360, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(path));
        const QUrl neighbor = QUrl::fromLocalFile(path);

        // Full development owns this gate, but a separately admitted small
        // neighbor can be ready before that long reader finishes.
        QMutexLocker fullDecodeGate(&policy.processingMutex());
        provider.prepareNearbyPreviews({neighbor}, QSize(640, 360), 384);
        QTRY_VERIFY_WITH_TIMEOUT(provider.hasNearbyPreview(neighbor), 1000);
        fullDecodeGate.unlock();
        provider.waitForPendingWork();
        QVERIFY(provider.nearbyPreviewBytes() <= 48 * 1024 * 1024);
    }

    void selectedPreviewDoesNotWaitForCancelledFullDecodeGate()
    {
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("next.png"));
        const QString stalePath = images.filePath(QStringLiteral("previous.png"));
        QImage image(640, 360, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(path));
        image.fill(Qt::red);
        QVERIFY(image.save(stalePath));
        const QString requestId = QStringLiteral("1/") +
                                  QString::fromLatin1(QUrl::toPercentEncoding(path)) +
                                  QStringLiteral("?licasa_stage=preview");
        const QString staleRequestId = QStringLiteral("1/") +
                                       QString::fromLatin1(QUrl::toPercentEncoding(stalePath)) +
                                       QStringLiteral("?licasa_stage=preview");

        // A previous full reader can keep this gate during native cancellation.
        // The bounded selected preview must still reach its first pixels.
        provider.releasePictureResources(true, QUrl::fromLocalFile(path));
        QMutexLocker oldDecodeGate(&policy.processingMutex());
        std::unique_ptr<QQuickImageResponse> staleResponse(
            provider.requestImageResponse(staleRequestId, QSize(640, 360)));
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(requestId, QSize(640, 360)));
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        const bool readyWithoutGate = finished.wait(1000);
        oldDecodeGate.unlock();
        provider.waitForPendingWork();

        QVERIFY(readyWithoutGate);
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY(texture != nullptr);
        QCOMPARE(texture->image().pixelColor(0, 0), QColor(Qt::green));
    }

    void navigationCancelsQueuedImageBeforeDecodingTheNextOne()
    {
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);

        const QString oldPath = images.filePath(QStringLiteral("old.png"));
        const QString nextPath = images.filePath(QStringLiteral("next.png"));
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(oldPath));
        image.fill(Qt::green);
        QVERIFY(image.save(nextPath));
        const auto requestId = [](const QString& path) {
            return QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path)) +
                   QStringLiteral("?licasa_stage=preview");
        };

        QSemaphore entered, release;
        provider.enqueueWork([&] {
            entered.release();
            release.acquire();
        });
        entered.acquire();
        std::unique_ptr<QQuickImageResponse> oldResponse(
            provider.requestImageResponse(requestId(oldPath), QSize(64, 64)));
        provider.releasePictureResources(true);
        std::unique_ptr<QQuickImageResponse> nextResponse(
            provider.requestImageResponse(requestId(nextPath), QSize(64, 64)));
        release.release();
        provider.waitForPendingWork();

        QVERIFY(oldResponse->textureFactory() == nullptr);
        std::unique_ptr<QQuickTextureFactory> nextTexture(nextResponse->textureFactory());
        QVERIFY(nextTexture != nullptr);
        QCOMPARE(nextTexture->image().pixelColor(0, 0), QColor(Qt::green));
    }

    void nearbyPreviewsStayWithinTheSelectedMemoryBudget()
    {
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        QList<QUrl> firstWindow;
        QList<QUrl> secondWindow;
        for (int index = 0; index < 11; ++index) {
            const QString path = images.filePath(QStringLiteral("photo-%1.png").arg(index));
            QImage image(800, 800, QImage::Format_ARGB32);
            image.fill(QColor::fromRgb(index * 12, 30, 90));
            QVERIFY(image.save(path));
            const QUrl url = QUrl::fromLocalFile(path);
            if (index < 10) {
                firstWindow.append(url);
            }
            if (index > 0) {
                secondWindow.append(url);
            }
        }

        provider.prepareNearbyPreviews(firstWindow, QSize(1200, 1200), 384);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewCount(), 10);
        QVERIFY(provider.nearbyPreviewBytes() <= 48 * 1024 * 1024);
        QVERIFY(provider.hasNearbyPreview(firstWindow.first()));

        const QString originalPath = firstWindow.first().toLocalFile();
        const QString replacementPath = images.filePath(QStringLiteral("replacement.png"));
        QVERIFY(QFile::copy(originalPath, replacementPath));
        QVERIFY(QFile::remove(originalPath));
        QVERIFY(QFile::rename(replacementPath, originalPath));
        QVERIFY(!provider.hasNearbyPreview(firstWindow.first()));

        provider.prepareNearbyPreviews(secondWindow, QSize(1200, 1200), 384);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewCount(), 10);
        QVERIFY(!provider.hasNearbyPreview(firstWindow.first()));
        QVERIFY(provider.hasNearbyPreview(secondWindow.last()));

        QImage changed(799, 800, QImage::Format_ARGB32);
        changed.fill(Qt::green);
        QVERIFY(changed.save(secondWindow.last().toLocalFile()));
        QVERIFY(!provider.hasNearbyPreview(secondWindow.last()));

        const QString hugePath = images.filePath(QStringLiteral("huge.png"));
        QImage huge(1600, 1600, QImage::Format_ARGB32);
        huge.fill(Qt::blue);
        QVERIFY(huge.save(hugePath));
        provider.prepareNearbyPreviews({QUrl::fromLocalFile(hugePath)}, QSize(1200, 1200), 384);
        provider.waitForPendingWork();
        QVERIFY(!provider.hasNearbyPreview(QUrl::fromLocalFile(hugePath)));
        QVERIFY(provider.nearbyPreviewBytes() <= 48 * 1024 * 1024);

        provider.prepareNearbyPreviews({secondWindow.last()}, QSize(400, 400), 384);
        provider.waitForPendingWork();
        QVERIFY(provider.hasNearbyPreview(secondWindow.last()));
        const QString requestId =
            QStringLiteral("1/") +
            QString::fromLatin1(QUrl::toPercentEncoding(secondWindow.last().toLocalFile())) +
            QStringLiteral("?licasa_stage=preview");
        const quint64 hitsBeforeResponse = provider.nearbyPreviewHitCount();
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(requestId, QSize(1200, 1200)));
        provider.waitForPendingWork();
        QVERIFY(response->errorString().isEmpty());
        QVERIFY(provider.nearbyPreviewHitCount() > hitsBeforeResponse);

        provider.releasePictureResources();
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewCount(), 0);
    }

    void destructionDrainsQueuedWorkOnTheWorker()
    {
        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        auto provider = std::make_unique<Licasa::AsyncImageProvider>(policy);
        QSemaphore entered, release;
        std::atomic_bool cleanupRan{false};
        std::atomic_bool cleanupRanOffMainThread{false};
        QThread* mainThread = QThread::currentThread();
        provider->enqueueWork([&] {
            entered.release();
            release.acquire();
        });
        entered.acquire();
        provider->enqueueWork([&] {
            cleanupRanOffMainThread = QThread::currentThread() != mainThread;
            cleanupRan = true;
        });
        // Keep cleanup queued when destruction begins, then let the active job
        // finish. Destruction must execute cleanup, not drop its closure.
        std::thread unblock([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release.release();
        });
        provider.reset();
        unblock.join();
        QVERIFY(cleanupRan.load());
        QVERIFY(cleanupRanOffMainThread.load());
    }
};

QTEST_MAIN(AsyncImageProviderTest)
#include "tst_async_image_provider.moc"
