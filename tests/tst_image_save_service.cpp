#include "app/app_constants.h"
#include "app/application_settings.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/format_support.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_probe.h"
#include "imaging/image_resource_policy.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QQuickTextureFactory>
#include <QRandomGenerator>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>

class ImageSaveServiceTest final : public QObject {
    Q_OBJECT

  private slots:
    void sharpenMatchesOriginalPixelStencil()
    {
        QRandomGenerator random(0x51a4u);
        for (const QSize size :
             {QSize(1, 7), QSize(7, 2), QSize(3, 3), QSize(4, 5), QSize(37, 29)}) {
            QImage raster(size, QImage::Format_ARGB32);
            for (int y = 0; y < raster.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(raster.scanLine(y));
                for (int x = 0; x < raster.width(); ++x) {
                    row[x] = random.generate();
                }
            }
            for (const auto format : {QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied,
                                      QImage::Format_RGB888, QImage::Format_RGBA64}) {
                const QImage source = raster.convertToFormat(format);
                for (const double amount : {0.01, 0.5, 1.0}) {
                    QImage actual = source;
                    actual.setColorSpace(QColorSpace::SRgb);
                    actual.setDevicePixelRatio(1.5);
                    actual.setText(QStringLiteral("caption"), QStringLiteral("preserved"));
                    const QImage original = actual.copy();
                    QImage expected = original;
                    if (size.width() >= 3 && size.height() >= 3) {
                        const QImage pixels = original.convertToFormat(QImage::Format_ARGB32);
                        expected = pixels.copy();
                        for (int y = 1; y < size.height() - 1; ++y) {
                            for (int x = 1; x < size.width() - 1; ++x) {
                                const QColor center = pixels.pixelColor(x, y);
                                const QColor neighbors[] = {
                                    pixels.pixelColor(x - 1, y), pixels.pixelColor(x + 1, y),
                                    pixels.pixelColor(x, y - 1), pixels.pixelColor(x, y + 1)};
                                const auto channel = [&](int component) {
                                    const auto value = [component](const QColor& color) {
                                        return component == 0   ? color.red()
                                               : component == 1 ? color.green()
                                                                : color.blue();
                                    };
                                    const int c = value(center);
                                    int detail = 4 * c;
                                    for (const auto& neighbor : neighbors) {
                                        detail -= value(neighbor);
                                    }
                                    return int(std::clamp(std::round(c + detail * (amount * 0.48)),
                                                          0.0, 255.0));
                                };
                                expected.setPixel(
                                    x, y,
                                    qRgba(channel(0), channel(1), channel(2), center.alpha()));
                            }
                        }
                    }
                    Licasa::ImageEditParameters parameters;
                    parameters.sharpen = amount;
                    QVERIFY(Licasa::applyImageEdits(actual, parameters));
                    QCOMPARE(actual, expected);
                    QCOMPARE(actual.colorSpace(), original.colorSpace());
                    QCOMPARE(actual.devicePixelRatio(), original.devicePixelRatio());
                    QCOMPARE(actual.text(QStringLiteral("caption")),
                             original.text(QStringLiteral("caption")));
                    QCOMPARE(source, raster.convertToFormat(format));
                }
            }
        }
    }

    void sharpenReusesOwnedRasterAndDetachesSharedRaster()
    {
        QImage owned(257, 129, QImage::Format_ARGB32);
        owned.fill(qRgba(50, 100, 150, 200));
        const uchar* storage = owned.constBits();
        Licasa::ImageEditParameters parameters;
        parameters.sharpen = 0.7;
        QVERIFY(Licasa::applyImageEdits(owned, parameters));
        QCOMPARE(owned.constBits(), storage);

        const QImage shared = owned;
        const QImage unchanged = shared.copy();
        QVERIFY(Licasa::applyImageEdits(owned, parameters));
        QVERIFY(owned.constBits() != shared.constBits());
        QCOMPARE(shared, unchanged);
    }

    void initTestCase()
    {
        QVERIFY(settingsDirectory_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("LicasaTests"));
        QCoreApplication::setApplicationName(QStringLiteral("ImageResourcePolicyTests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
        QSettings().clear();

        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        resourcePolicy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);
    }

    void configurableImageLimitPersistsAndUpdatesDecoderBudget()
    {
        QVERIFY(!qEnvironmentVariableIsSet("QT_IMAGEIO_MAXALLOC"));
        QCOMPARE(preferences_->maximumImageMegapixels(),
                 Licasa::Constants::defaultImageLimitMegapixels);
        QCOMPARE(resourcePolicy_->maximumImageMegapixels(),
                 Licasa::Constants::defaultImageLimitMegapixels);
        QCOMPARE(preferences_->maximumImageMemoryMiB(), Licasa::Constants::defaultImageMemoryMiB);
        QVERIFY(resourcePolicy_->allows(QSize(10000, 10000)));
        QVERIFY(!resourcePolicy_->allows(QSize(30000, 20000)));

        QSignalSpy changed(preferences_.get(),
                           &Licasa::ViewerPreferences::maximumImageMegapixelsChanged);
        preferences_->setMaximumImageMegapixels(250);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(resourcePolicy_->maximumImageMegapixels(), 250);
        QCOMPARE(QImageReader::allocationLimit(), resourcePolicy_->decoderAllocationLimitMiB());
        QVERIFY(resourcePolicy_->allows(QSize(20000, 10000)));

        Licasa::ViewerPreferences restoredPreferences;
        QCOMPARE(restoredPreferences.maximumImageMegapixels(), 250);

        preferences_->setMaximumImageMemoryMiB(8192);
        QCOMPARE(preferences_->maximumImageMegapixels(),
                 Licasa::Constants::defaultImageLimitMegapixels);
        QCOMPARE(resourcePolicy_->maximumImageMegapixels(),
                 Licasa::Constants::defaultImageLimitMegapixels);

        preferences_->setMaximumImageMegapixels(-1);
        QCOMPARE(preferences_->maximumImageMegapixels(),
                 Licasa::Constants::minimumImageLimitMegapixels);
        QVERIFY(!resourcePolicy_->allows(QSize(7680, 4320)));
        QCOMPARE(QImageReader::allocationLimit(), 191);

        preferences_->setMaximumImageMegapixels(std::numeric_limits<int>::max());
        QCOMPARE(preferences_->maximumImageMegapixels(),
                 Licasa::Constants::maximumImageLimitMegapixels);
        QCOMPARE(QImageReader::allocationLimit(), resourcePolicy_->decoderAllocationLimitMiB());

        preferences_->setMaximumImageMegapixels(Licasa::Constants::defaultImageLimitMegapixels);
        QCOMPARE(QImageReader::allocationLimit(), resourcePolicy_->decoderAllocationLimitMiB());
    }

    void policyUpdatesStayResponsiveDuringActiveWork()
    {
        QSemaphore admitted;
        QSemaphore release;
        const int startingLimit = resourcePolicy_->maximumImageMegapixels();
        const int startingDecoderCap = resourcePolicy_->decoderAllocationLimitMiB();
        int snapshot = 0;
        int capDuringOperation = 0;
        std::thread worker([&] {
            QMutexLocker lock(&resourcePolicy_->processingMutex());
            snapshot = resourcePolicy_->prepareForProcessing();
            admitted.release();
            // A timeout avoids hanging the test if a future setter regresses
            // to blocking on this worker from the GUI thread.
            release.tryAcquire(1, 2000);
            capDuringOperation = QImageReader::allocationLimit();
        });
        admitted.acquire();
        QElapsedTimer timer;
        timer.start();
        preferences_->setMaximumImageMegapixels(25);
        const qint64 settingMs = timer.elapsed();
        const bool newAdmission = resourcePolicy_->allows(QSize(7680, 4320));
        bool guiHeartbeat = false;
        QTimer::singleShot(0, this, [&] { guiHeartbeat = true; });
        QTest::qWait(30);
        release.release();
        worker.join();
        QVERIFY2(settingMs < 250, "Changing the setting blocked the GUI on active work");
        QVERIFY(guiHeartbeat);
        QVERIFY(!newAdmission);
        QCOMPARE(snapshot, startingLimit);
        QCOMPARE(capDuringOperation, startingDecoderCap);
        QTRY_COMPARE(QImageReader::allocationLimit(), 191);
        preferences_->setMaximumImageMegapixels(Licasa::Constants::defaultImageLimitMegapixels);
    }

    void decodeSaveAndCancellationShareTheProcessingGate()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("source.png"));
        QImage source(1920, 1080, QImage::Format_RGB32);
        source.fill(Qt::darkCyan);
        QVERIFY(source.save(path));
        Licasa::AsyncImageProvider provider(*resourcePolicy_);
        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy saved(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);

        QMutexLocker gate(&resourcePolicy_->processingMutex());
        const QString requestId =
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path));
        std::unique_ptr<QQuickImageResponse> preview(
            provider.requestImageResponse(requestId, QSize(640, 480)));
        std::unique_ptr<QQuickImageResponse> stale(
            provider.requestImageResponse(requestId, QSize(640, 480)));
        QSignalSpy ready(preview.get(), &QQuickImageResponse::finished);
        QSignalSpy cancelled(stale.get(), &QQuickImageResponse::finished);
        const bool accepted = service.save(
            QUrl::fromLocalFile(path),
            QUrl::fromLocalFile(directory.filePath(QStringLiteral("copy.png"))), {}, {});
        stale->cancel();
        QTest::qWait(30);
        const bool held = ready.isEmpty() && saved.isEmpty();
        gate.unlock();
        QVERIFY(accepted);
        QVERIFY(held);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 5000);
        QVERIFY(failed.isEmpty());
        QVERIFY(preview->errorString().isEmpty());
        std::unique_ptr<QQuickTextureFactory> staleTexture(stale->textureFactory());
        QVERIFY(!staleTexture);
    }

    void cropSizeEstimateMatchesSavedBytes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("source.png"));
        QImage source(257, 193, QImage::Format_RGB32);
        QRandomGenerator random(0x9123u);
        for (int y = 0; y < source.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                row[x] = qRgb(random.bounded(256), random.bounded(256), random.bounded(256));
            }
        }
        QVERIFY(source.save(sourcePath));

        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy estimates(&service, &Licasa::ImageSaveService::sizeEstimateReady);
        QSignalSpy saved(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failures(&service, &Licasa::ImageSaveService::saveFailed);
        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
        const auto formats = QImageWriter::supportedImageFormats();
        int requestId = 0;
        for (const QByteArray& format : {QByteArrayLiteral("original"), QByteArrayLiteral("png"),
                                         QByteArrayLiteral("jpeg"), QByteArrayLiteral("webp")}) {
            if (format != QByteArrayLiteral("original") && !formats.contains(format)) {
                continue;
            }
            for (const qreal cropX : {0.12, 0.37}) {
                const QVariantMap edits{
                    {QStringLiteral("cropX"), cropX},    {QStringLiteral("cropY"), 0.19},
                    {QStringLiteral("cropWidth"), 0.48}, {QStringLiteral("cropHeight"), 0.61},
                    {QStringLiteral("quarterTurns"), 1}, {QStringLiteral("exposure"), 0.15}};
                const QVariantMap exportValues{{QStringLiteral("format"), format},
                                               {QStringLiteral("scale"), 0.75},
                                               {QStringLiteral("quality"), cropX < 0.2 ? 77 : 92}};
                service.estimateSize(sourceUrl, ++requestId, edits, exportValues);
                QTRY_COMPARE_WITH_TIMEOUT(estimates.count(), 1, 15000);
                const QList<QVariant> estimate = estimates.takeFirst();
                QCOMPARE(estimate.at(1).toInt(), requestId);
                QVERIFY(estimate.at(3).toString().isEmpty());
                const qint64 estimatedBytes = estimate.at(2).toLongLong();
                QVERIFY(estimatedBytes > 0);

                const QString destination =
                    directory.filePath(QStringLiteral("result-%1-%2.%3")
                                           .arg(QString::fromLatin1(format))
                                           .arg(cropX)
                                           .arg(format == QByteArrayLiteral("original")
                                                    ? QStringLiteral("png")
                                                    : QString::fromLatin1(format)));
                QVERIFY(
                    service.save(sourceUrl, QUrl::fromLocalFile(destination), edits, exportValues));
                QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
                QVERIFY(failures.isEmpty());
                const QString savedPath = saved.takeFirst().at(1).toUrl().toLocalFile();
                QCOMPARE(QFileInfo(savedPath).size(), estimatedBytes);
            }
        }
        QVERIFY(requestId >= 6);
    }

    void cropSizeEstimateKeepsLatestQueuedCrop()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("source.png"));
        QImage source(96, 64, QImage::Format_RGB32);
        source.fill(Qt::darkMagenta);
        QVERIFY(source.save(sourcePath));
        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);

        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy estimates(&service, &Licasa::ImageSaveService::sizeEstimateReady);
        QMutexLocker gate(&resourcePolicy_->processingMutex());
        service.estimateSize(sourceUrl, 1, {{QStringLiteral("cropWidth"), 0.9}}, {});
        service.estimateSize(sourceUrl, 2, {{QStringLiteral("cropWidth"), 0.7}}, {});
        service.estimateSize(sourceUrl, 3, {{QStringLiteral("cropWidth"), 0.5}}, {});
        gate.unlock();
        QTRY_COMPARE_WITH_TIMEOUT(estimates.count(), 2, 15000);
        QCOMPARE(estimates.at(0).at(1).toInt(), 1);
        QCOMPARE(estimates.at(1).at(1).toInt(), 3);
        QVERIFY(estimates.at(0).at(2).toLongLong() > 0);
        QVERIFY(estimates.at(1).at(2).toLongLong() > 0);
    }

    void cropSizeEstimateUsesSuggestedFormatForOtherSources()
    {
        if (!QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("bmp"))) {
            QSKIP("BMP writer is unavailable");
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("source.bmp"));
        QImage source(113, 71, QImage::Format_RGB32);
        source.fill(Qt::cyan);
        QVERIFY(source.save(sourcePath));
        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
        const QVariantMap edits{{QStringLiteral("cropX"), 0.1},
                                {QStringLiteral("cropWidth"), 0.65}};
        const QVariantMap exportValues{{QStringLiteral("format"), QStringLiteral("original")}};

        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy estimates(&service, &Licasa::ImageSaveService::sizeEstimateReady);
        QSignalSpy saved(&service, &Licasa::ImageSaveService::saveCompleted);
        service.estimateSize(sourceUrl, 1, edits, exportValues);
        QTRY_COMPARE_WITH_TIMEOUT(estimates.count(), 1, 15000);
        const qint64 estimatedBytes = estimates.first().at(2).toLongLong();
        QVERIFY(estimatedBytes > 0);
        QVERIFY(service.save(sourceUrl,
                             QUrl::fromLocalFile(directory.filePath(QStringLiteral("edited.png"))),
                             edits, exportValues));
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
        QCOMPARE(QFileInfo(saved.first().at(1).toUrl().toLocalFile()).size(), estimatedBytes);
    }

    void detectsAnimatedAndStaticImages()
    {
        const QString sourceRoot = QString::fromUtf8(LICASA_SOURCE_DIR);
        const QUrl animated = QUrl::fromLocalFile(
            sourceRoot +
            QStringLiteral("/tests/test-assets/animated/gif/standard-transparent-320x240.gif"));
        const QUrl still = QUrl::fromLocalFile(
            sourceRoot + QStringLiteral("/tests/test-assets/dimensions/odd-photo-997x613.png"));

        Licasa::ImageProbe probe(*resourcePolicy_);
        QVERIFY(probe.inspect(animated).value("animated").toBool());
        QVERIFY(!probe.inspect(still).value("animated").toBool());
        QCOMPARE(probe.inspect(animated).value("size").toSize(), QSize(320, 240));
        QCOMPARE(probe.inspect(still).value("size").toSize(), QSize(997, 613));
        QCOMPARE(probe.inspect(animated).value("fileSize").toLongLong(),
                 QFileInfo(animated.toLocalFile()).size());
        QCOMPARE(probe.inspect(still).value("fileSize").toLongLong(),
                 QFileInfo(still.toLocalFile()).size());
        QVERIFY(probe.inspect(QUrl("https://example.org/image.png")).isEmpty());
        QVERIFY(probe.inspect(QUrl::fromLocalFile("/missing-licasa-image")).isEmpty());
    }

    void previewAndSaveParametersUseTheSamePipeline()
    {
        QImage source(12, 8, QImage::Format_ARGB32);
        source.fill(QColor(80, 120, 180, 137));

        const QString query = QStringLiteral("b=0.15&c=0.10&hi=-0.20&sh=0.12&s=0.08&vib=0.18&"
                                             "w=0.05&t=-0.03&blur=0.04&sharp=0.20&vig=0.10&"
                                             "r=1&fh=1&fv=0&cx=0.25&cy=0.25&cw=0.5&ch=0.5");
        const QVariantMap values{
            {QStringLiteral("exposure"), 0.15},       {QStringLiteral("contrast"), 0.10},
            {QStringLiteral("highlights"), -0.20},    {QStringLiteral("shadows"), 0.12},
            {QStringLiteral("saturation"), 0.08},     {QStringLiteral("vibrance"), 0.18},
            {QStringLiteral("warmth"), 0.05},         {QStringLiteral("tint"), -0.03},
            {QStringLiteral("soften"), 0.04},         {QStringLiteral("sharpen"), 0.20},
            {QStringLiteral("vignette"), 0.10},       {QStringLiteral("quarterTurns"), 1},
            {QStringLiteral("flipHorizontal"), true}, {QStringLiteral("flipVertical"), false},
            {QStringLiteral("cropX"), 0.25},          {QStringLiteral("cropY"), 0.25},
            {QStringLiteral("cropWidth"), 0.5},       {QStringLiteral("cropHeight"), 0.5},
        };

        QImage preview = source;
        QImage saved = source;
        Licasa::applyImageEdits(preview, Licasa::editParametersFromQuery(query));
        Licasa::applyImageEdits(saved, Licasa::editParametersFromMap(values));

        QCOMPARE(preview, saved);
        QCOMPARE(preview.size(), QSize(4, 6));
        QCOMPARE(preview.pixelColor(1, 1).alpha(), 137);
    }

    void normalizesUntrustedEditAndExportValues()
    {
        const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
        const qreal infinity = std::numeric_limits<qreal>::infinity();
        const QVariantMap edits{
            {QStringLiteral("exposure"), infinity},   {QStringLiteral("contrast"), 4.0},
            {QStringLiteral("highlights"), -4.0},     {QStringLiteral("soften"), -2.0},
            {QStringLiteral("sharpen"), infinity},    {QStringLiteral("vignette"), 5.0},
            {QStringLiteral("quarterTurns"), -5},     {QStringLiteral("cropX"), 0.9},
            {QStringLiteral("cropY"), nan},           {QStringLiteral("cropWidth"), 0.8},
            {QStringLiteral("cropHeight"), infinity},
        };

        const Licasa::ImageEditParameters parameters = Licasa::editParametersFromMap(edits);
        QCOMPARE(parameters.exposure, 0.0);
        QCOMPARE(parameters.contrast, 1.0);
        QCOMPARE(parameters.highlights, -1.0);
        QCOMPARE(parameters.soften, 0.0);
        QCOMPARE(parameters.sharpen, 0.0);
        QCOMPARE(parameters.vignette, 1.0);
        QCOMPARE(parameters.quarterTurns, 3);
        QCOMPARE(parameters.cropX, 0.9);
        QCOMPARE(parameters.cropY, 0.0);
        QVERIFY(qAbs(parameters.cropWidth - 0.1) < 0.000001);
        QCOMPARE(parameters.cropHeight, 1.0);

        const Licasa::ImageExportOptions nonFiniteExport = Licasa::exportOptionsFromMap({
            {QStringLiteral("scale"), infinity},
            {QStringLiteral("quality"), -20},
            {QStringLiteral("format"), QStringLiteral("GIF")},
        });
        QCOMPARE(nonFiniteExport.scale, 1.0);
        QCOMPARE(nonFiniteExport.quality, 1);
        QVERIFY(nonFiniteExport.format.isEmpty());

        const Licasa::ImageExportOptions boundedExport = Licasa::exportOptionsFromMap({
            {QStringLiteral("scale"), 40.0},
            {QStringLiteral("quality"), 400},
            {QStringLiteral("format"), QStringLiteral("JPG")},
        });
        QCOMPARE(boundedExport.scale, 2.0);
        QCOMPARE(boundedExport.quality, 100);
        QCOMPARE(boundedExport.format, QByteArrayLiteral("jpeg"));
    }

    void handlesNonFiniteDirectEditParameters()
    {
        QImage image(64, 48, QImage::Format_ARGB32);
        image.fill(QColor(20, 80, 160, 170));

        const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
        const qreal infinity = std::numeric_limits<qreal>::infinity();
        Licasa::ImageEditParameters parameters;
        parameters.exposure = nan;
        parameters.contrast = infinity;
        parameters.highlights = -infinity;
        parameters.soften = nan;
        parameters.sharpen = infinity;
        parameters.vignette = nan;
        parameters.cropX = infinity;
        parameters.cropY = nan;
        parameters.cropWidth = -infinity;
        parameters.cropHeight = infinity;

        QVERIFY(Licasa::applyImageEdits(image, parameters));
        QVERIFY(!image.isNull());
        QVERIFY(image.width() > 0);
        QVERIFY(image.height() > 0);
    }

    void honorsPreCancelledEditsWithoutMutation()
    {
        QImage image(24, 18, QImage::Format_ARGB32);
        image.fill(Qt::darkMagenta);
        const QImage original = image;
        const std::atomic_bool cancelled = true;

        Licasa::ImageEditParameters parameters;
        parameters.exposure = 0.8;
        QVERIFY(!Licasa::applyImageEdits(image, parameters, &cancelled));
        QCOMPARE(image, original);
    }

    void randomizedEditInputsRemainBounded()
    {
        QImage source(80, 60, QImage::Format_ARGB32);
        source.fill(QColor(90, 130, 210, 151));
        QRandomGenerator random(0x51CA5A);

        const auto randomValue = [&random]() { return random.generateDouble() * 8.0 - 4.0; };

        for (int iteration = 0; iteration < 192; ++iteration) {
            Licasa::ImageEditParameters parameters;
            parameters.exposure = randomValue();
            parameters.contrast = randomValue();
            parameters.highlights = randomValue();
            parameters.shadows = randomValue();
            parameters.saturation = randomValue();
            parameters.vibrance = randomValue();
            parameters.warmth = randomValue();
            parameters.tint = randomValue();
            parameters.soften = randomValue();
            parameters.sharpen = randomValue();
            parameters.vignette = randomValue();
            parameters.quarterTurns = static_cast<int>(random.bounded(2001)) - 1000;
            parameters.flipHorizontal = random.bounded(2) != 0;
            parameters.flipVertical = random.bounded(2) != 0;
            parameters.cropX = randomValue();
            parameters.cropY = randomValue();
            parameters.cropWidth = randomValue();
            parameters.cropHeight = randomValue();
            if (iteration % 17 == 0) {
                parameters.exposure = std::numeric_limits<qreal>::quiet_NaN();
                parameters.cropWidth = std::numeric_limits<qreal>::infinity();
            }

            QImage result = source;
            QVERIFY2(Licasa::applyImageEdits(result, parameters),
                     qPrintable(QStringLiteral("edit iteration %1 failed").arg(iteration)));
            QVERIFY(!result.isNull());
            QVERIFY(result.width() > 0);
            QVERIFY(result.height() > 0);
            QVERIFY(static_cast<qint64>(result.width()) * result.height() <=
                    static_cast<qint64>(source.width()) * source.height());
        }
    }

    void malformedCorpusTerminatesSafely()
    {
        const QString sourceRoot = QString::fromUtf8(LICASA_SOURCE_DIR);
        QDirIterator files(sourceRoot + QStringLiteral("/tests/test-assets/broken"), QDir::Files,
                           QDirIterator::NoIteratorFlags);
        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());

        Licasa::ImageProbe probe(*resourcePolicy_);
        Licasa::FormatSupport formats;
        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy completed(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);
        int fixtureCount = 0;

        while (files.hasNext()) {
            const QString sourcePath = files.next();
            const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
            probe.inspect(sourceUrl).value("size").toSize();
            probe.inspect(sourceUrl).value("animated").toBool();
            formats.canOpen(sourceUrl);

            const QString destinationPath =
                outputDirectory.filePath(QStringLiteral("output-%1.png").arg(fixtureCount));
            QVERIFY(service.save(sourceUrl, QUrl::fromLocalFile(destinationPath), {}, {}));
            ++fixtureCount;
            QTRY_COMPARE_WITH_TIMEOUT(completed.count() + failed.count(), 1, 5000);
            if (completed.count() == 1) {
                const QString actualPath = completed.first().at(1).toUrl().toLocalFile();
                QVERIFY(!QImage(actualPath).isNull());
            } else {
                QVERIFY(!QFileInfo::exists(destinationPath));
            }
            QVERIFY(!service.busy());
            completed.clear();
            failed.clear();
        }

        QVERIFY(fixtureCount >= 8);
        QVERIFY(!service.busy());
    }

    void rejectsOversizedImageBeforeRasterAllocation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("oversized.svg"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray payload = QByteArrayLiteral(
            "<svg xmlns='http://www.w3.org/2000/svg' width='100000' height='100000' "
            "viewBox='0 0 100000 100000'><rect width='100000' height='100000'/></svg>");
        QCOMPARE(sourceFile.write(payload), payload.size());
        sourceFile.close();

        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
        Licasa::FormatSupport formats;
        if (!formats.canOpen(sourceUrl)) {
            QSKIP("The Qt SVG image plugin is unavailable.");
        }

        Licasa::ImageProbe probe(*resourcePolicy_);
        QCOMPARE(probe.inspect(sourceUrl).value("size").toSize(), QSize(100000, 100000));
        QVERIFY(!probe.inspect(sourceUrl).value("withinBudget").toBool());

        const QString destinationPath = directory.filePath(QStringLiteral("output.png"));
        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy completed(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);
        QVERIFY(service.save(sourceUrl, QUrl::fromLocalFile(destinationPath), {}, {}));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QCOMPARE(completed.count(), 0);
        QVERIFY(failed.first().at(1).toString().contains(QStringLiteral("full-resolution limit"),
                                                         Qt::CaseInsensitive));
        QVERIFY(!QFileInfo::exists(destinationPath));
    }

    void keepsRepositoryEightKFixtureSupported()
    {
        constexpr qint64 eightKPixels = 7680LL * 4320LL;
        static_assert(eightKPixels <=
                      static_cast<qint64>(Licasa::Constants::defaultImageLimitMegapixels) *
                          1'000'000);

        const QString path = QString::fromUtf8(LICASA_SOURCE_DIR) +
                             QStringLiteral("/tests/test-assets/dimensions/uhd-8k-7680x4320.png");
        const QUrl url = QUrl::fromLocalFile(path);
        Licasa::ImageProbe probe(*resourcePolicy_);
        Licasa::FormatSupport formats;
        QCOMPARE(probe.inspect(url).value("size").toSize(), QSize(7680, 4320));
        QVERIFY(probe.inspect(url).value("withinBudget").toBool());
        QVERIFY(formats.canOpen(url));
    }

    void oversizedPreviewRequiresNativeScaledDecode()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("oversized.bmp"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // 6000 x 6000 RGB BMP header, without pixel data. The application must
        // reject admission before asking a non-scaling decoder to rasterize.
        const QByteArray header = QByteArray::fromHex(
            "424d36f36f0600000000360000002800000070170000701700000100180000000000"
            "00f36f0600000000000000000000000000000000");
        QCOMPARE(file.write(header), header.size());
        file.close();
        QImageReader reader(path);
        QCOMPARE(reader.size(), QSize(6000, 6000));
        QVERIFY(!reader.supportsOption(QImageIOHandler::ScaledSize));
        preferences_->setMaximumImageMegapixels(25);
        Licasa::AsyncImageProvider provider(*resourcePolicy_);
        std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path)),
            QSize(640, 480)));
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(response->errorString().contains(QStringLiteral("bounded preview")));
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY(!texture);
        preferences_->setMaximumImageMegapixels(100);
    }

    void previewsImageAboveFullResolutionLimitAtBoundedSize()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("huge-graph.svg"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray payload = QByteArrayLiteral(
            "<svg xmlns='http://www.w3.org/2000/svg' width='100000' height='100000' "
            "viewBox='0 0 100000 100000'><rect width='100000' height='100000' "
            "fill='#336699'/></svg>");
        QCOMPARE(sourceFile.write(payload), payload.size());
        sourceFile.close();

        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
        Licasa::FormatSupport formats;
        if (!formats.canOpen(sourceUrl)) {
            QSKIP("The Qt SVG image plugin is unavailable.");
        }

        Licasa::ImageProbe probe(*resourcePolicy_);
        QVERIFY(!probe.inspect(sourceUrl).value("withinBudget").toBool());

        const QString requestId =
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(sourcePath));
        Licasa::AsyncImageProvider provider(*resourcePolicy_);
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(requestId, QSize(640, 480)));
        QVERIFY(response);
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY2(response->errorString().isEmpty(), qPrintable(response->errorString()));

        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY(texture);
        QCOMPARE(texture->textureSize(), QSize(480, 480));

        preferences_->setMaximumImageMegapixels(Licasa::Constants::minimumImageLimitMegapixels);
        std::unique_ptr<QQuickImageResponse> oversizedPreview(
            provider.requestImageResponse(requestId, QSize(6000, 6000)));
        QVERIFY(oversizedPreview);
        QSignalSpy oversizedFinished(oversizedPreview.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(oversizedFinished.count(), 1, 5000);
        QVERIFY2(oversizedPreview->errorString().isEmpty(),
                 qPrintable(oversizedPreview->errorString()));

        std::unique_ptr<QQuickTextureFactory> oversizedTexture(oversizedPreview->textureFactory());
        QVERIFY(oversizedTexture);
        const QSize boundedSize = oversizedTexture->textureSize();
        QVERIFY(static_cast<quint64>(boundedSize.width()) *
                    static_cast<quint64>(boundedSize.height()) <=
                static_cast<quint64>(Licasa::Constants::minimumImageLimitMegapixels) * 1'000'000);

        preferences_->setMaximumImageMegapixels(Licasa::Constants::defaultImageLimitMegapixels);
    }

    void overwritesOriginalAtomicallyWithRotation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString sourcePath = directory.filePath(QStringLiteral("source.png"));
        QImage source(2, 3, QImage::Format_ARGB32);
        source.fill(Qt::transparent);
        source.setPixelColor(0, 0, Qt::red);
        source.setPixelColor(1, 0, Qt::green);
        source.setPixelColor(0, 1, Qt::blue);
        source.setPixelColor(1, 1, Qt::yellow);
        source.setPixelColor(0, 2, Qt::cyan);
        source.setPixelColor(1, 2, Qt::magenta);
        QVERIFY(source.save(sourcePath));

        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy completed(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);
        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);

        const QVariantMap edits{{QStringLiteral("quarterTurns"), 1}};
        QVERIFY(service.save(sourceUrl, sourceUrl, edits, {}));
        QVERIFY(service.busy());
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!service.busy());

        const QImage saved(sourcePath);
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.size(), QSize(3, 2));
        QCOMPARE(completed.first().at(1).toUrl(), sourceUrl);
    }

    void appendsAWritableExtensionForSaveAs()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString sourcePath = directory.filePath(QStringLiteral("source.png"));
        QImage source(4, 2, QImage::Format_RGB32);
        source.fill(Qt::darkCyan);
        QVERIFY(source.save(sourcePath));

        const QString requestedPath = directory.filePath(QStringLiteral("copy"));
        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy completed(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);

        QVERIFY(service.save(QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(requestedPath),
                             {}, {}));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);

        const QUrl actualDestination = completed.first().at(1).toUrl();
        QVERIFY(actualDestination.toLocalFile().endsWith(QStringLiteral("copy.png")));
        QVERIFY(!QImage(actualDestination.toLocalFile()).isNull());
    }

    void cropsScalesAndForcesRequestedFormat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString sourcePath = directory.filePath(QStringLiteral("source.png"));
        QImage source(100, 80, QImage::Format_ARGB32);
        source.fill(Qt::cyan);
        QVERIFY(source.save(sourcePath));

        const QString requestedPath = directory.filePath(QStringLiteral("export.png"));
        const QVariantMap edits{
            {QStringLiteral("cropX"), 0.25},    {QStringLiteral("cropY"), 0.25},
            {QStringLiteral("cropWidth"), 0.5}, {QStringLiteral("cropHeight"), 0.5},
            {QStringLiteral("sharpen"), 0.2},
        };
        const QVariantMap exportOptions{
            {QStringLiteral("scale"), 0.5},
            {QStringLiteral("quality"), 82},
            {QStringLiteral("format"), QStringLiteral("jpeg")},
        };

        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy completed(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);
        QVERIFY(service.save(QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(requestedPath),
                             edits, exportOptions));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);

        const QString destination = completed.first().at(1).toUrl().toLocalFile();
        QVERIFY(destination.endsWith(QStringLiteral("export.jpg")));
        const QImage saved(destination);
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.size(), QSize(25, 20));
    }

    void savesOwnedMotionFrameAsIndependentCopy()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString sourcePath = directory.filePath(QStringLiteral("motion-source.jpg"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray sentinel = QByteArrayLiteral("original-motion-photo-sentinel");
        QCOMPARE(source.write(sentinel), sentinel.size());
        source.close();

        QImage image(24, 12, QImage::Format_RGBA8888);
        image.fill(QColor(20, 80, 160, 220));
        image.setColorSpace(QColorSpace::SRgb);

        Licasa::MotionPhotoFrameRaster frame;
        frame.rgba8888 =
            QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
        frame.iccProfile = image.colorSpace().iccProfile();
        frame.size = image.size();
        frame.bytesPerLine = image.bytesPerLine();
        frame.timestampUs = 42000;
        QVERIFY(frame.isValid());

        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy ordinaryCompleted(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy ordinaryFailed(&service, &Licasa::ImageSaveService::saveFailed);
        QSignalSpy frameCompleted(&service, &Licasa::ImageSaveService::frameSaveCompleted);
        QSignalSpy frameFailed(&service, &Licasa::ImageSaveService::frameSaveFailed);

        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
        const QString requestedPath = directory.filePath(QStringLiteral("selected-frame"));
        QVERIFY(service.saveFrameCopy(
            sourceUrl, QUrl::fromLocalFile(requestedPath), std::move(frame),
            {{QStringLiteral("format"), QStringLiteral("png")}, {QStringLiteral("scale"), 0.5}}));
        QVERIFY(service.busy());
        QTRY_COMPARE_WITH_TIMEOUT(frameCompleted.count(), 1, 5000);
        QCOMPARE(frameFailed.count(), 0);
        QCOMPARE(ordinaryCompleted.count(), 0);
        QCOMPARE(ordinaryFailed.count(), 0);
        QVERIFY(!service.busy());

        const QString destination = frameCompleted.first().at(1).toUrl().toLocalFile();
        QVERIFY(destination.endsWith(QStringLiteral("selected-frame.png")));
        const QImage saved(destination);
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.size(), QSize(12, 6));

        QFile original(sourcePath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), sentinel);
    }
    void exportDestinationAndRasterContract_data()
    {
        QTest::addColumn<bool>("frameExport");
        QTest::addColumn<QString>("sourceSuffix");
        QTest::addColumn<QString>("requestedName");
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("expectedName");
        QTest::addColumn<qreal>("scale");
        for (bool frame : {false, true}) {
            const QByteArray prefix = frame ? "frame-" : "edited-";
            QTest::newRow((prefix + "fallback").constData())
                << frame << "jpg" << "copy" << "" << (frame ? "copy.png" : "copy.jpeg") << qreal(1);
            QTest::newRow((prefix + "forced-jpeg").constData())
                << frame << "png" << "copy.png" << "jpeg" << "copy.jpg" << qreal(.5);
            QTest::newRow((prefix + "jpeg-alias").constData())
                << frame << "png" << "copy.JPEG" << "jpg" << "copy.JPEG" << qreal(1);
            QTest::newRow((prefix + "explicit-png").constData())
                << frame << "png" << "copy" << "png" << "copy.png" << qreal(2);
            QTest::newRow((prefix + "inferred-png").constData())
                << frame << "png" << "copy.png" << "" << "copy.png" << qreal(.5);
            QTest::newRow((prefix + "unsupported-suffix").constData())
                << frame << "png" << "copy.unsupported" << "" << "" << qreal(1);
            QTest::newRow((prefix + "missing-directory").constData())
                << frame << "png" << "missing/copy.png" << "png" << "" << qreal(1);
        }
    }

    void exportDestinationAndRasterContract()
    {
        QFETCH(bool, frameExport);
        QFETCH(QString, sourceSuffix);
        QFETCH(QString, requestedName);
        QFETCH(QString, format);
        QFETCH(QString, expectedName);
        QFETCH(qreal, scale);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath("source." + sourceSuffix);
        QImage source(24, 16, QImage::Format_RGBA8888);
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                source.setPixelColor(x, y, QColor(x * 10, y * 15, 100, 40 + x * 8));
            }
        }
        source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        QVERIFY(source.save(sourcePath));
        QFile original(sourcePath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray sourceBytes = original.readAll();
        original.close();
        // Supply both entry points with the same decoded pixels, even for JPEG.
        source = QImage(sourcePath).convertToFormat(QImage::Format_RGBA8888);
        QVERIFY(!source.isNull());

        const QString requestedPath = directory.filePath(requestedName);
        const QByteArray sentinel("existing destination must survive rejection");
        if (expectedName.isEmpty() && !requestedName.contains('/')) {
            QFile existing(requestedPath);
            QVERIFY(existing.open(QIODevice::WriteOnly));
            QCOMPARE(existing.write(sentinel), sentinel.size());
        }
        Licasa::ImageSaveService service(*resourcePolicy_);
        QSignalSpy completed(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);
        QSignalSpy frameCompleted(&service, &Licasa::ImageSaveService::frameSaveCompleted);
        QSignalSpy frameFailed(&service, &Licasa::ImageSaveService::frameSaveFailed);
        const QVariantMap options{{"format", format}, {"scale", scale}, {"quality", 82}};
        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
        const QUrl destinationUrl = QUrl::fromLocalFile(requestedPath);
        if (frameExport) {
            Licasa::MotionPhotoFrameRaster frame;
            frame.size = source.size();
            frame.bytesPerLine = source.bytesPerLine();
            frame.rgba8888 =
                QByteArray(reinterpret_cast<const char*>(source.constBits()), source.sizeInBytes());
            frame.iccProfile = source.colorSpace().iccProfile();
            QVERIFY(service.saveFrameCopy(sourceUrl, destinationUrl, std::move(frame), options));
        } else {
            QVERIFY(service.save(sourceUrl, destinationUrl, {}, options));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
        const QSignalSpy& success = frameExport ? frameCompleted : completed;
        const QSignalSpy& failure = frameExport ? frameFailed : failed;
        QCOMPARE((frameExport ? completed : frameCompleted).count(), 0);
        QCOMPARE((frameExport ? failed : frameFailed).count(), 0);
        if (expectedName.isEmpty()) {
            QCOMPARE(success.count(), 0);
            QCOMPARE(failure.count(), 1);
            QVERIFY(!failure.first().at(1).toString().isEmpty());
            if (!requestedName.contains('/')) {
                QFile existing(requestedPath);
                QVERIFY(existing.open(QIODevice::ReadOnly));
                QCOMPARE(existing.readAll(), sentinel);
            } else {
                QVERIFY(!QFileInfo::exists(requestedPath));
            }
        } else {
            QCOMPARE(failure.count(), 0);
            QCOMPARE(success.count(), 1);
            QCOMPARE(success.first().at(1).toUrl(),
                     QUrl::fromLocalFile(directory.filePath(expectedName)));
            const QImage saved(directory.filePath(expectedName));
            const QSize expectedSize(qRound(source.width() * scale),
                                     qRound(source.height() * scale));
            QCOMPARE(saved.size(), expectedSize);
            if (expectedName.endsWith(".png")) {
                const QImage expected =
                    source.scaled(expectedSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                QCOMPARE(saved.convertToFormat(QImage::Format_RGBA8888),
                         expected.convertToFormat(QImage::Format_RGBA8888));
                QCOMPARE(saved.colorSpace(), source.colorSpace());
            }
        }
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), sourceBytes);
    }

  private:
    QTemporaryDir settingsDirectory_;
    std::unique_ptr<Licasa::ViewerPreferences> preferences_;
    std::unique_ptr<Licasa::ImageResourcePolicy> resourcePolicy_;
};

int main(int argc, char** argv)
{
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    QGuiApplication application(argc, argv);
    Licasa::initializeImagePlugins();
    ImageSaveServiceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_image_save_service.moc"
