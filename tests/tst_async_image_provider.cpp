#include "app/application_settings.h"
#include "imaging/async_image_provider.h"
#include "imaging/image_resource_policy.h"
#include "imaging/parallel_decode_pair.h"
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
#include "imaging/fast_tiff_preview.h"
#include <tiffio.h>
#endif
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
#include "imaging/fast_png_preview.h"
#include <png.h>
#endif
#ifdef LICASA_HAVE_FAST_WEBP_PREVIEW
#include "imaging/fast_webp_preview.h"
#endif

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickTextureFactory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
namespace {
bool writePlainLargePng(const QString& path, bool alpha)
{
    const QByteArray encoded = QFile::encodeName(path);
    FILE* file = std::fopen(encoded.constData(), "wb");
    if (!file) {
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info) {
        if (png) {
            png_destroy_write_struct(&png, nullptr);
        }
        std::fclose(file);
        return false;
    }
    std::vector<uchar> row(size_t(5000) * (alpha ? 4 : 3));
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        return false;
    }
    png_init_io(png, file);
    png_set_compression_level(png, 1);
    png_set_IHDR(png, info, 5000, 5000, 8, alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);
    const int channels = alpha ? 4 : 3;
    for (int x = 0; x < 5000; ++x) {
        row[size_t(x) * channels] = 30;
        row[size_t(x) * channels + 1] = 80;
        row[size_t(x) * channels + 2] = 140;
        if (alpha) {
            row[size_t(x) * channels + 3] = 255;
        }
    }
    for (int y = 0; y < 5000; ++y) {
        if (y == 2500) {
            row[size_t(2500) * channels] = 210;
            row[size_t(2500) * channels + 1] = 40;
            row[size_t(2500) * channels + 2] = 90;
            if (alpha) {
                row[size_t(2500) * channels + 3] = 120;
            }
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return std::fclose(file) == 0;
}
} // namespace
#endif

class AsyncImageProviderTest final : public QObject {
    Q_OBJECT

  private slots:

    void initTestCase() { qputenv("LICASA_TRACE_FAST_PNM", QByteArrayLiteral("1")); }

    void completionIsDeliveredOnResponseThread_data()
    {
        QTest::addColumn<bool>("cancelled");
        QTest::addColumn<bool>("missing");
        QTest::newRow("decoded") << false << false;
        QTest::newRow("cancelled") << true << false;
        QTest::newRow("missing") << false << true;
    }

    void completionIsDeliveredOnResponseThread()
    {
        QFETCH(bool, cancelled);
        QFETCH(bool, missing);
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("response.png"));
        if (!missing) {
            QImage image(64, 48, QImage::Format_RGB32);
            image.fill(Qt::green);
            QVERIFY(image.save(path));
        }

        // Hold the serial worker until connections and cancellation are ready.
        QSemaphore entered, release;
        provider.enqueueWork([&] {
            entered.release();
            release.acquire();
        });
        const bool workerEntered = entered.tryAcquire(1, 5000);
        std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path)), QSize()));
        const auto drain = qScopeGuard([&] {
            release.release();
            provider.waitForPendingWork();
        });
        QVERIFY(workerEntered);
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        std::atomic_bool wrongThread{false};
        QThread* owner = response->thread();
        QObject::connect(
            response.get(), &QQuickImageResponse::finished, response.get(),
            [&] { wrongThread.store(QThread::currentThread() != owner); }, Qt::DirectConnection);
        if (cancelled) {
            response->cancel();
        }
        release.release();
        provider.waitForPendingWork();
        // The owner is blocked here. Workers may finish decoding, but must not
        // publish to Qt while another thread can delete the response.
        QVERIFY(!wrongThread.load());
        QCOMPARE(finished.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(!wrongThread.load());
        QCOMPARE(response->errorString().isEmpty(), !missing);
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QCOMPARE(bool(texture), !cancelled && !missing);
    }

    void parallelPairsJoinEitherRequestOrder_data()
    {
        QTest::addColumn<bool>("previewFirst");
        QTest::newRow("preview-first") << true;
        QTest::newRow("full-first") << false;
    }

    void parallelPairsJoinEitherRequestOrder()
    {
        QFETCH(bool, previewFirst);
        using Registry = Licasa::ParallelDecodePairRegistry;
        Registry registry;
        const QByteArray identity(32, 'a');
        const auto firstRole = previewFirst ? Registry::Role::Preview : Registry::Role::Full;
        const auto secondRole = previewFirst ? Registry::Role::Full : Registry::Role::Preview;
        const auto first = registry.claim(QStringLiteral("pair"), identity, firstRole);
        const auto second = registry.claim(QStringLiteral("pair"), identity, secondRole);
        QVERIFY(first);
        QVERIFY(first == second);
    }

    void presentedPreviewRemainsAvailableToLateFullRequest()
    {
        using Registry = Licasa::ParallelDecodePairRegistry;
        Registry registry;
        const QByteArray identity(32, 'a');
        const auto preview =
            registry.claim(QStringLiteral("pair"), identity, Registry::Role::Preview);
        QVERIFY(preview);
        registry.markPresented(QStringLiteral("pair"));
        const auto full = registry.claim(QStringLiteral("pair"), identity, Registry::Role::Full);
        QVERIFY(full == preview);
        QVERIFY(full->previewPresented.load(std::memory_order_acquire));
    }

    void failedPreviewReleasesLateFullWithoutForgettingRole()
    {
        using Registry = Licasa::ParallelDecodePairRegistry;
        Registry registry;
        const QByteArray identity(32, 'a');
        const auto preview =
            registry.claim(QStringLiteral("pair"), identity, Registry::Role::Preview);
        QVERIFY(preview);
        // Cancellation and decoder failure both guarantee that no frame arrives.
        preview->finishPreview(true);
        const auto full = registry.claim(QStringLiteral("pair"), identity, Registry::Role::Full);
        QVERIFY(full == preview);
        QVERIFY(full->previewPresented.load(std::memory_order_acquire));
        QVERIFY(full->previewFinished);
        QVERIFY(!registry.claim(QStringLiteral("pair"), identity, Registry::Role::Preview));
    }

    void duplicateOrMismatchedRequestCannotReplaceLivePair()
    {
        using Registry = Licasa::ParallelDecodePairRegistry;
        Registry registry;
        const QByteArray identity(32, 'a');
        const auto preview =
            registry.claim(QStringLiteral("pair"), identity, Registry::Role::Preview);
        QVERIFY(preview);
        QVERIFY(!registry.claim(QStringLiteral("pair"), identity, Registry::Role::Preview));
        QVERIFY(!registry.claim(QStringLiteral("pair"), QByteArray(32, 'b'), Registry::Role::Full));
        const auto full = registry.claim(QStringLiteral("pair"), identity, Registry::Role::Full);
        QVERIFY(full == preview);
        QVERIFY(!registry.claim(QStringLiteral("pair"), identity, Registry::Role::Full));
        registry.markPresented(QStringLiteral("pair"));
        QVERIFY(full->previewPresented.load(std::memory_order_acquire));
        registry.markPresented(QStringLiteral("unknown"));
        const auto future =
            registry.claim(QStringLiteral("unknown"), identity, Registry::Role::Full);
        QVERIFY(future);
        QVERIFY(!future->previewPresented.load(std::memory_order_acquire));
    }

    void pairRegistryIsBoundedWithoutEvictingUnmatchedCounterpart()
    {
        using Registry = Licasa::ParallelDecodePairRegistry;
        Registry registry;
        const QByteArray identity(32, 'a');
        std::shared_ptr<Licasa::ParallelDecodePair> first;
        for (qsizetype index = 0; index < Registry::maximumPairs; ++index) {
            const auto pair =
                registry.claim(QString::number(index), identity, Registry::Role::Preview);
            QVERIFY(pair);
            if (index == 0) {
                first = pair;
            }
        }
        QVERIFY(!registry.claim(QStringLiteral("overflow"), identity, Registry::Role::Full));
        QVERIFY(registry.claim(QStringLiteral("0"), identity, Registry::Role::Full) == first);
        registry.clear();
        registry.markPresented(QStringLiteral("0"));
        QVERIFY(!first->previewPresented.load(std::memory_order_acquire));
        const auto next = registry.claim(QStringLiteral("next"), identity, Registry::Role::Preview);
        QVERIFY(next);
        first->finishPreview(true);
        QVERIFY(first->previewFinished);
        QVERIFY(!next->previewPresented.load(std::memory_order_acquire));
        QVERIFY(registry.claim(QStringLiteral("next"), identity, Registry::Role::Full) == next);
        QVERIFY(!registry.claim(QString(Registry::maximumIdLength + 1, 'x'), identity,
                                Registry::Role::Full));
        QVERIFY(!registry.claim(QStringLiteral("bad-fingerprint"), QByteArray("short"),
                                Registry::Role::Full));
    }

    void concurrentRoleClaimsShareOnePair()
    {
        using Registry = Licasa::ParallelDecodePairRegistry;
        for (int attempt = 0; attempt < 16; ++attempt) {
            Registry registry;
            const QByteArray identity(32, 'a');
            QSemaphore begin;
            std::shared_ptr<Licasa::ParallelDecodePair> preview;
            std::shared_ptr<Licasa::ParallelDecodePair> full;
            std::thread previewThread([&] {
                begin.acquire();
                preview = registry.claim(QStringLiteral("pair"), identity, Registry::Role::Preview);
            });
            std::thread fullThread([&] {
                begin.acquire();
                full = registry.claim(QStringLiteral("pair"), identity, Registry::Role::Full);
            });
            begin.release(2);
            previewThread.join();
            fullThread.join();
            QVERIFY(preview);
            QVERIFY(preview == full);
        }
    }

    void providerPairingIsIndependentOfRequestOrder_data()
    {
        QTest::addColumn<bool>("previewFirst");
        QTest::addColumn<int>("collision");
        QTest::newRow("preview-first") << true << 0;
        QTest::newRow("full-first") << false << 0;
        QTest::newRow("same-id-other-image") << true << 1;
        QTest::newRow("same-id-other-render-settings") << true << 2;
    }

    void providerPairingIsIndependentOfRequestOrder()
    {
        QFETCH(bool, previewFirst);
        QFETCH(int, collision);
        // The phase trace flag is cached on first decode. Run this integration
        // in a fresh child so it does not alter tracing for the rest of the suite.
        if (!qEnvironmentVariableIsSet("LICASA_PAIR_ORDER_TEST_CHILD")) {
            QProcess child;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("LICASA_PAIR_ORDER_TEST_CHILD"), QStringLiteral("1"));
            environment.insert(QStringLiteral("LICASA_TRACE_DECODE_PHASES"), QStringLiteral("1"));
            child.setProcessEnvironment(environment);
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("providerPairingIsIndependentOfRequestOrder:") +
                             QString::fromLatin1(QTest::currentDataTag()),
                         QStringLiteral("-platform"), QStringLiteral("offscreen")});
            const bool finished = child.waitForFinished(15000);
            if (!finished) {
                child.kill();
                child.waitForFinished(5000);
            }
            const QByteArray output = child.readAll();
            QVERIFY2(finished, output.constData());
            QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                     output.constData());
            return;
        }
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("pair.png"));
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(path));
        const QString base =
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path));
        QString fullBase = base;
        if (collision == 1) {
            const QString otherPath = images.filePath(QStringLiteral("other.png"));
            image.fill(Qt::blue);
            QVERIFY(image.save(otherPath));
            fullBase =
                QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(otherPath));
        }
        QTest::ignoreMessage(QtInfoMsg, QRegularExpression(QStringLiteral(
                                            "^\\[Licasa\\] Decode phases: parallel-preview ")));
        QTest::ignoreMessage(
            QtInfoMsg,
            QRegularExpression(
                collision == 0 ? QStringLiteral("^\\[Licasa\\] Decode phases: speculative-full ")
                               : QStringLiteral("^\\[Licasa\\] Decode phases: full ")));
        const auto request = [&](bool preview) {
            return std::unique_ptr<QQuickImageResponse>(provider.requestImageResponse(
                (preview ? base : fullBase) +
                    (preview ? QStringLiteral("?licasa_stage=preview&licasa_parallel=preview")
                             : QStringLiteral("?licasa_stage=full&licasa_parallel=full")) +
                    QStringLiteral("&licasa_pair=order-test") +
                    ((!preview && collision == 2) ? QStringLiteral("&cm=false") : QString()),
                preview ? QSize(16, 12) : QSize()));
        };
        QMutexLocker gate(&policy.processingMutex());
        auto first = request(previewFirst);
        auto second = request(!previewFirst);
        gate.unlock();
        provider.waitForPendingWork();
        QQuickImageResponse* preview = previewFirst ? first.get() : second.get();
        QQuickImageResponse* full = previewFirst ? second.get() : first.get();
        QVERIFY(preview->errorString().isEmpty());
        QVERIFY(full->errorString().isEmpty());
        std::unique_ptr<QQuickTextureFactory> previewTexture(preview->textureFactory());
        std::unique_ptr<QQuickTextureFactory> fullTexture(full->textureFactory());
        QVERIFY(previewTexture);
        QVERIFY(fullTexture);
        QCOMPARE(previewTexture->image().size(), QSize(16, 12));
        QCOMPARE(fullTexture->image().size(), QSize(64, 48));
        QCOMPARE(previewTexture->image().pixelColor(8, 6), QColor(Qt::green));
        QCOMPARE(fullTexture->image().pixelColor(32, 24),
                 collision == 1 ? QColor(Qt::blue) : QColor(Qt::green));
    }

#ifdef LICASA_HAVE_FAST_WEBP_PREVIEW
    void simpleWebpPreviewUsesBoundedBufferAndRejectsCancellation()
    {
        QTemporaryDir images;
        QVERIFY(images.isValid());
        const QString path = images.filePath(QStringLiteral("large.webp"));
        QImage source(2400, 1600, QImage::Format_RGB32);
        QVERIFY(!source.isNull());
        source.fill(QColor(35, 80, 140));
        QImageWriter writer(path, "webp");
        writer.setQuality(95);
        QVERIFY2(writer.write(source), qPrintable(writer.errorString()));

        std::atomic_bool cancelled{false};
        const QImage preview =
            Licasa::readFastWebpPreview(path, QSize(600, 400), 4'000'000, 1'000'000, &cancelled);
        QCOMPARE(preview.size(), QSize(600, 400));
        QVERIFY(preview.sizeInBytes() <= 1'000'000);
        const QColor pixel = preview.pixelColor(300, 200);
        QVERIFY(qAbs(pixel.red() - 35) <= 5);
        QVERIFY(qAbs(pixel.green() - 80) <= 5);
        QVERIFY(qAbs(pixel.blue() - 140) <= 5);
        QVERIFY(Licasa::readFastWebpPreview(path, QSize(600, 400), 3'000'000, 1'000'000, &cancelled)
                    .isNull());
        cancelled.store(true);
        QVERIFY(Licasa::readFastWebpPreview(path, QSize(600, 400), 4'000'000, 1'000'000, &cancelled)
                    .isNull());
    }
#endif
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
    void deflateRgbTiffPreviewKeepsPixelsAndBudget()
    {
        QTemporaryDir images;
        QVERIFY(images.isValid());
        constexpr uint32_t width = 1024;
        constexpr uint32_t height = 768;
        for (const uint16_t samples : {uint16_t(3), uint16_t(4)}) {
            const QString path = images.filePath(QStringLiteral("rgb-%1.tif").arg(samples));
            TIFF* tiff = TIFFOpen(QFile::encodeName(path).constData(), "w");
            QVERIFY(tiff != nullptr);
            TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
            TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
            TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, samples);
            TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
            if (samples == 3) {
                TIFFSetField(tiff, TIFFTAG_PREDICTOR, 2);
            }
            TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 3);
            TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
            if (samples == 4) {
                uint16_t extra = EXTRASAMPLE_UNASSALPHA;
                TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, &extra);
            }
            std::vector<uchar> row(size_t(width) * samples);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    row[size_t(x) * samples] = uchar(x % 256);
                    row[size_t(x) * samples + 1] = uchar(y % 256);
                    row[size_t(x) * samples + 2] = uchar((x + y) % 256);
                    if (samples == 4) {
                        row[size_t(x) * samples + 3] = uchar((x * 3 + y) % 256);
                    }
                }
                QCOMPARE(TIFFWriteScanline(tiff, row.data(), y), 1);
            }
            TIFFClose(tiff);

            std::atomic_bool cancelled{false};
            const QImage preview =
                Licasa::readFastTiffPreview(path, QSize(320, 240), 80000, 320000, &cancelled);
            QVERIFY(!preview.isNull());
            QCOMPARE(preview.size(), QSize(320, 240));
            QVERIFY(preview.sizeInBytes() <= 320000);
            for (const QPoint point : {QPoint(0, 0), QPoint(159, 119), QPoint(319, 239)}) {
                const int sourceX = int(quint64(point.x()) * width / quint64(preview.width()));
                const int sourceY = int(quint64(point.y()) * height / quint64(preview.height()));
                const QColor color = preview.pixelColor(point);
                QCOMPARE(color.red(), sourceX % 256);
                QCOMPARE(color.green(), sourceY % 256);
                QCOMPARE(color.blue(), (sourceX + sourceY) % 256);
                QCOMPARE(color.alpha(), samples == 4 ? (sourceX * 3 + sourceY) % 256 : 255);
            }
            cancelled.store(true);
            QVERIFY(Licasa::readFastTiffPreview(path, QSize(320, 240), 80000, 320000, &cancelled)
                        .isNull());
        }
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

    void largeWebpNearbyPreviewRespectsDistanceAndMemory()
    {
        if (!QImageWriter::supportedImageFormats().contains("webp")) {
            QSKIP("WebP image writer unavailable");
        }
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        const QString firstPath = images.filePath(QStringLiteral("neighbor-0.webp"));
        QImage source(3840, 2160, QImage::Format_RGB32);
        source.fill(QColor(30, 80, 140));
        QVERIFY(source.save(firstPath, "WEBP", 80));
        source = {};
        QList<QUrl> urls{QUrl::fromLocalFile(firstPath)};
        for (int index = 1; index < 4; ++index) {
            const QString path = images.filePath(QStringLiteral("neighbor-%1.webp").arg(index));
            QVERIFY(QFile::copy(firstPath, path));
            urls.append(QUrl::fromLocalFile(path));
        }

        const QSize target(1200, 675);
        Licasa::AsyncImageProvider minimumBudget(policy);
        minimumBudget.prepareNearbyPreviews(urls, target, 384);
        minimumBudget.waitForPendingWork();
        QVERIFY(!minimumBudget.hasNearbyPreview(urls.first()));

        Licasa::AsyncImageProvider provider(policy);
        provider.prepareNearbyPreviews(urls, target, 8192);
        provider.waitForPendingWork();
        for (int index = 0; index < 3; ++index) {
            QVERIFY(provider.hasNearbyPreview(urls.at(index)));
        }
        QVERIFY(!provider.hasNearbyPreview(urls.at(3)));
        QVERIFY(provider.nearbyPreviewBytes() <= 96 * 1024 * 1024);

        const quint64 hitsBefore = provider.nearbyPreviewHitCount();
        const QString request = QStringLiteral("1/") +
                                QString::fromLatin1(QUrl::toPercentEncoding(firstPath)) +
                                QStringLiteral("?licasa_stage=preview");
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(request, target));
        provider.waitForPendingWork();
        QVERIFY2(response->errorString().isEmpty(), qPrintable(response->errorString()));
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY(texture);
        QCOMPARE(texture->textureSize(), target);
        QVERIFY(provider.nearbyPreviewHitCount() > hitsBefore);
    }

    void ordinaryPnmPreviewPreservesFullDetail_data()
    {
        QTest::addColumn<bool>("binary");
        QTest::addColumn<QString>("suffix");
        QTest::newRow("p6-ppm") << true << QStringLiteral("ppm");
        QTest::newRow("p6-pnm") << true << QStringLiteral("pnm");
        QTest::newRow("p3-fallback") << false << QStringLiteral("ppm");
    }

    void ordinaryPnmPreviewPreservesFullDetail()
    {
        QFETCH(bool, binary);
        QFETCH(QString, suffix);
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("ordinary.") + suffix);
        const QSize sourceSize(21, 15);
        const QSize target(7, 5);
        QImage source(sourceSize, QImage::Format_RGB32);
        QByteArray bytes = binary ? QByteArray("P6\n21 15\n255\n") : QByteArray("P3\n21 15\n255\n");
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                const QColor color((x * 17 + y * 31) & 255, (x * 37 + y * 13) & 255,
                                   (x * 7 + y * 43) & 255);
                source.setPixelColor(x, y, color);
                for (const int channel : {color.red(), color.green(), color.blue()}) {
                    if (binary) {
                        bytes.append(char(channel));
                    } else {
                        bytes += QByteArray::number(channel) + ' ';
                    }
                }
            }
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();

        QImage expected;
        if (binary) {
            // Each target pixel covers exactly nine source pixels. Sum the
            // original RGB values directly, independently of helper spans and
            // row accumulators, to establish the displayed area-filter result.
            expected = QImage(target, QImage::Format_RGB32);
            for (int y = 0; y < target.height(); ++y) {
                for (int x = 0; x < target.width(); ++x) {
                    int red = 0;
                    int green = 0;
                    int blue = 0;
                    for (int dy = 0; dy < 3; ++dy) {
                        for (int dx = 0; dx < 3; ++dx) {
                            const QColor color = source.pixelColor(x * 3 + dx, y * 3 + dy);
                            red += color.red();
                            green += color.green();
                            blue += color.blue();
                        }
                    }
                    expected.setPixelColor(x, y,
                                           QColor((red + 4) / 9, (green + 4) / 9, (blue + 4) / 9));
                }
            }
        } else {
            QImageReader fallback(path);
            fallback.setAutoTransform(true);
            fallback.setScaledSize(target);
            expected = fallback.read();
            QVERIFY(!expected.isNull());
        }
        const QString base =
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path));
        if (!expected.colorSpace().isValid()) {
            expected.setColorSpace(QColorSpace(QColorSpace::SRgb));
        }
        if (binary) {
            QTest::ignoreMessage(
                QtInfoMsg, QRegularExpression(QStringLiteral("^\\[Licasa\\] Fast PNM preview:")));
        }
        std::unique_ptr<QQuickImageResponse> preview(provider.requestImageResponse(
            base + QStringLiteral("?licasa_stage=preview&cm=0"), target));
        provider.waitForPendingWork();
        QVERIFY2(preview->errorString().isEmpty(), qPrintable(preview->errorString()));
        std::unique_ptr<QQuickTextureFactory> previewTexture(preview->textureFactory());
        QVERIFY(previewTexture);
        QCOMPARE(previewTexture->image(),
                 expected.convertToFormat(previewTexture->image().format()));
        QVERIFY(!provider.hasDecodedFullBase(QUrl::fromLocalFile(path)));

        QImageReader fullReader(path);
        fullReader.setAutoTransform(true);
        QImage fullReference = fullReader.read();
        QVERIFY(!fullReference.isNull());
        if (!fullReference.colorSpace().isValid()) {
            fullReference.setColorSpace(QColorSpace(QColorSpace::SRgb));
        }
        std::unique_ptr<QQuickImageResponse> full(provider.requestImageResponse(
            base + QStringLiteral("?licasa_stage=full&cm=0"), QSize()));
        provider.waitForPendingWork();
        QVERIFY2(full->errorString().isEmpty(), qPrintable(full->errorString()));
        std::unique_ptr<QQuickTextureFactory> fullTexture(full->textureFactory());
        QVERIFY(fullTexture);
        QCOMPARE(fullTexture->image(),
                 fullReference.convertToFormat(fullTexture->image().format()));
        QCOMPARE(fullTexture->image().size(), sourceSize);
    }

    void largePngPreviewReusesItsDecodedFullBase()
    {
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        preferences.setMaximumImageMegapixels(40);
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("large.png"));
        const QUrl url = QUrl::fromLocalFile(path);
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
        QVERIFY(writePlainLargePng(path, true));
        QVERIFY(Licasa::fastPngBaseEligible(path, policy.maximumImagePixels()));
        std::atomic_bool cancelled{false};
        const auto direct = Licasa::readFastPngPreview(
            path, QSize(1000, 1000), policy.maximumImagePixels(), 8 * 1024 * 1024, &cancelled);
        QCOMPARE(direct.fullBase.format(), QImage::Format_ARGB32);
        QCOMPARE(direct.fullBase.pixelColor(2500, 2500), QColor(210, 40, 90, 120));
        QVERIFY(direct.fullBase.colorSpace() == QColorSpace(QColorSpace::SRgb));
#else
        QImage source(5000, 5000, QImage::Format_ARGB32);
        QVERIFY(!source.isNull());
        source.fill(QColor(30, 80, 140));
        source.setPixelColor(2500, 2500, QColor(210, 40, 90, 120));
        QVERIFY(source.save(path));
        source = {};
#endif

        const QString base =
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path));
        QVERIFY(!provider.hasDecodedFullBase(url));
        std::unique_ptr<QQuickImageResponse> preview(provider.requestImageResponse(
            base + QStringLiteral("?licasa_stage=preview"), QSize(1200, 1200)));
        provider.waitForPendingWork();
        QVERIFY(preview->errorString().isEmpty());
        std::unique_ptr<QQuickTextureFactory> previewTexture(preview->textureFactory());
        QVERIFY(previewTexture != nullptr);
        QCOMPARE(previewTexture->image().size(), QSize(1200, 1200));
        QVERIFY(provider.hasDecodedFullBase(url));
        const quint64 hitsBeforeFull = provider.decodedBaseCacheHitCount();

        std::unique_ptr<QQuickImageResponse> full(
            provider.requestImageResponse(base + QStringLiteral("?licasa_stage=full"), QSize()));
        provider.waitForPendingWork();
        QVERIFY(full->errorString().isEmpty());
        std::unique_ptr<QQuickTextureFactory> fullTexture(full->textureFactory());
        QVERIFY(fullTexture != nullptr);
        QCOMPARE(fullTexture->image().size(), QSize(5000, 5000));
        const QColor displayed = fullTexture->image().pixelColor(2500, 2500);
        QCOMPARE(displayed.red(), 210);
        QCOMPARE(displayed.green(), 40);
        QVERIFY(std::abs(displayed.blue() - 90) <= 1);
        QCOMPARE(displayed.alpha(), 120);
        QVERIFY(provider.decodedBaseCacheHitCount() > hitsBeforeFull);
        provider.releasePictureResources();
        provider.waitForPendingWork();
        QVERIFY(!provider.hasDecodedFullBase(url));
    }

#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
    void truncatedFastPngReleasesPartialRasters()
    {
        QTemporaryDir images;
        QVERIFY(images.isValid());
        const QString path = images.filePath(QStringLiteral("truncated.png"));
        QVERIFY(writePlainLargePng(path, false));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(file.size() / 2));
        file.close();
        std::atomic_bool cancelled{false};
        // Exercise errors after raster allocation in both reader modes. Leak
        // detection must see every partial full raster and scratch row released.
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto full = Licasa::readFastPngPreview(path, QSize(128, 128), 40000000,
                                                         8 * 1024 * 1024, &cancelled);
            QVERIFY(full.preview.isNull());
            QVERIFY(full.fullBase.isNull());
            QVERIFY(Licasa::readFastPngNearbyPreview(path, QSize(128, 128), 40000000,
                                                     8 * 1024 * 1024, &cancelled)
                        .isNull());
        }
    }

    void largePngGetsBoundedNearbyPreview()
    {
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        preferences.setMaximumImageMegapixels(40);
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("neighbor.png"));
        QVERIFY(writePlainLargePng(path, false));
        const QUrl url = QUrl::fromLocalFile(path);

        provider.prepareNearbyPreviews({url}, QSize(1000, 1000), 384);
        QTRY_VERIFY_WITH_TIMEOUT(provider.hasNearbyPreview(url), 5000);
        QVERIFY(provider.nearbyPreviewBytes() <= 5 * 1024 * 1024);
        provider.releasePictureResources();
        provider.waitForPendingWork();
    }

    void fastPngRgbPreviewRespectsPixelsAndCancellation()
    {
        QTemporaryDir images;
        QVERIFY(images.isValid());
        const QString path = images.filePath(QStringLiteral("rgb.png"));
        QVERIFY(writePlainLargePng(path, false));
        QVERIFY(Licasa::fastPngBaseEligible(path, 40000000));
        std::atomic_bool cancelled{false};
        const auto decoded = Licasa::readFastPngPreview(path, QSize(1000, 1000), 40000000,
                                                        8 * 1024 * 1024, &cancelled);
        QCOMPARE(decoded.preview.size(), QSize(1000, 1000));
        QCOMPARE(decoded.fullBase.size(), QSize(5000, 5000));
        QCOMPARE(decoded.preview.format(), QImage::Format_RGB32);
        QCOMPARE(decoded.fullBase.format(), QImage::Format_RGB32);
        QCOMPARE(decoded.preview.pixelColor(500, 500), QColor(210, 40, 90));
        QCOMPARE(decoded.fullBase.pixelColor(2500, 2500), QColor(210, 40, 90));
        const QImage nearby = Licasa::readFastPngNearbyPreview(path, QSize(1000, 1000), 40000000,
                                                               8 * 1024 * 1024, &cancelled);
        QCOMPARE(nearby.size(), QSize(1000, 1000));
        QCOMPARE(nearby.pixelColor(500, 500), QColor(210, 40, 90));
        cancelled.store(true);
        QVERIFY(Licasa::readFastPngNearbyPreview(path, QSize(1000, 1000), 40000000, 8 * 1024 * 1024,
                                                 &cancelled)
                    .isNull());
        QVERIFY(Licasa::readFastPngPreview(path, QSize(1000, 1000), 40000000, 8 * 1024 * 1024,
                                           &cancelled)
                    .preview.isNull());
        cancelled.store(false);
        QVERIFY(Licasa::readFastPngPreview(path, QSize(1000, 1000), 40000000, 1000, &cancelled)
                    .preview.isNull());
        QFile truncated(path);
        QVERIFY(truncated.open(QIODevice::ReadWrite));
        QVERIFY(truncated.resize(truncated.size() / 2));
        truncated.close();
        QVERIFY(Licasa::readFastPngPreview(path, QSize(1000, 1000), 40000000, 8 * 1024 * 1024,
                                           &cancelled)
                    .preview.isNull());
    }
#endif

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

    void nearbyPreviewsKeepTinySourcesAtNativeSize_data()
    {
        QTest::addColumn<int>("extent");
        QTest::newRow("one-pixel") << 1;
        QTest::newRow("two-pixel") << 2;
    }

    void nearbyPreviewsKeepTinySourcesAtNativeSize()
    {
        QFETCH(int, extent);
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("tiny-neighbor.png"));
        QImage source(extent, extent, QImage::Format_RGB32);
        source.setColorSpace(QColorSpace::SRgb);
        for (int y = 0; y < extent; ++y) {
            for (int x = 0; x < extent; ++x) {
                source.setPixel(x, y, qRgb(20 + x * 35, 40 + y * 45, 70 + x + y));
            }
        }
        QVERIFY(source.save(path));

        const QSize requested(1280, 960);
        provider.prepareNearbyPreviews({QUrl::fromLocalFile(path)}, requested, 384);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewCount(), 1);
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(extent * extent * 4));
        const quint64 hitsBefore = provider.nearbyPreviewHitCount();
        const QString id = QStringLiteral("1/") +
                           QString::fromLatin1(QUrl::toPercentEncoding(path)) +
                           QStringLiteral("?licasa_stage=preview");
        std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(id, requested));
        provider.waitForPendingWork();
        QVERIFY(response->errorString().isEmpty());
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY(texture);
        QCOMPARE(texture->image().size(), source.size());
        for (int y = 0; y < extent; ++y) {
            for (int x = 0; x < extent; ++x) {
                QCOMPARE(texture->image().pixelColor(x, y), source.pixelColor(x, y));
            }
        }
        QVERIFY(provider.nearbyPreviewHitCount() > hitsBefore);
    }

    void nearbyPreviewsFollowViewportSizeChanges()
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
        QImage source(256, 256, QImage::Format_RGB32);
        source.fill(Qt::green);
        QVERIFY(source.save(path));
        const QUrl url = QUrl::fromLocalFile(path);

        provider.prepareNearbyPreviews({url}, QSize(32, 32), 384);
        provider.waitForPendingWork();
        QVERIFY(provider.hasNearbyPreview(url));
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(32 * 32 * 4));

        // A larger viewport must rebuild the same neighbor, rather than reuse
        // a smaller cached raster just because its file identity still matches.
        provider.prepareNearbyPreviews({url}, QSize(128, 128), 384);
        provider.waitForPendingWork();
        const QString id = QStringLiteral("1/") +
                           QString::fromLatin1(QUrl::toPercentEncoding(path)) +
                           QStringLiteral("?licasa_stage=preview");
        const auto rasterFor = [&](const QSize& requested) {
            std::unique_ptr<QQuickImageResponse> response(
                provider.requestImageResponse(id, requested));
            provider.waitForPendingWork();
            if (!response->errorString().isEmpty()) {
                return QImage{};
            }
            std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
            return texture ? texture->image() : QImage{};
        };
        const QImage grown = rasterFor(QSize(128, 128));
        QCOMPARE(grown.size(), QSize(128, 128));
        QCOMPARE(grown.pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(128 * 128 * 4));

        // A smaller viewport can reuse the prepared raster without another
        // decode. Partial growth still fits its actual preparation size.
        provider.prepareNearbyPreviews({url}, QSize(16, 16), 384);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(128 * 128 * 4));
        QCOMPARE(rasterFor(QSize(16, 16)).size(), QSize(128, 128));
        provider.prepareNearbyPreviews({url}, QSize(96, 96), 384);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(128 * 128 * 4));
        QCOMPARE(rasterFor(QSize(96, 96)).size(), QSize(128, 128));

        provider.prepareNearbyPreviews({url}, QSize(192, 192), 384);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(192 * 192 * 4));
        const QImage regrown = rasterFor(QSize(192, 192));
        QCOMPARE(regrown.size(), QSize(192, 192));
        QCOMPARE(regrown.pixelColor(0, 0), QColor(Qt::green));

        provider.prepareNearbyPreviews({url}, QSize(192, 192), 0);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewCount(), 0);
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(0));
        provider.prepareNearbyPreviews({url}, QSize(32, 32), 384);
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(32 * 32 * 4));
        provider.releasePictureResources();
        provider.waitForPendingWork();
        QCOMPARE(provider.nearbyPreviewCount(), 0);
        QCOMPARE(provider.nearbyPreviewBytes(), qsizetype(0));
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
