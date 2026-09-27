#include "app/application_settings.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_animation.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"

#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QQuickImageProvider>
#include <QQuickTextureFactory>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <libheif/heif.h>
#include <memory>

namespace Contract = Licasa::ImageDecodeContract;
namespace {
constexpr int pinnedSequenceLastFrame = 119; // SDWebImage fixture contains 120 samples.
QString fixture(const char* name)
{
    return QStringLiteral(LICASA_SOURCE_DIR "/tests/test-assets/modern/heif/") +
           QString::fromLatin1(name);
}
QByteArray contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
QString codecError(QImageReader& reader)
{
    return reader.device()->property(Contract::errorProperty).toString();
}

QString sequenceFixture()
{
#ifdef LICASA_HEIF_SEQUENCE_FIXTURE
    return QStringLiteral(LICASA_HEIF_SEQUENCE_FIXTURE);
#else
    return {};
#endif
}

QString requireSequenceFixture()
{
    const QString path = sequenceFixture();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return {};
    }
    return path;
}

struct AnimationViewer {
    Licasa::ViewerPreferences preferences;
    Licasa::ImageResourcePolicy policy{preferences};
    std::unique_ptr<Licasa::AsyncImageProvider> pool{new Licasa::AsyncImageProvider(policy)};
    std::unique_ptr<Licasa::ImageAnimationService> service{
        new Licasa::ImageAnimationService(policy, *pool)};
    std::unique_ptr<QQuickImageProvider> frames{service->createFrameProvider()};
    std::unique_ptr<QObject> owner{new QObject};
    Licasa::ImageAnimation* animation =
        qobject_cast<Licasa::ImageAnimation*>(service->createController(owner.get()));

    ~AnimationViewer() { owner.reset(); }

    QImage frame() const
    {
        QSize size;
        return frames->requestImage(animation->frameSource().path().mid(1), &size, {});
    }
};

// Independent reference: libheif's complete-image assembly/transform path.
// This only reads the small fixtures, never the 48 MP stress image.
QImage referenceDecode(const QString& path, bool icc = false)
{
    if (heif_init(nullptr).code != heif_error_Ok) {
        return {};
    }
    QImage result;
    auto* context = heif_context_alloc();
    heif_image_handle* handle = nullptr;
    heif_image* image = nullptr;
    auto* options = heif_decoding_options_alloc();
    options->strict_decoding = 1;
    options->num_codec_threads = 1;
    options->convert_hdr_to_8bit = 1;
    options->output_image_nclx_profile_passthrough = icc;
    heif_context_set_max_decoding_threads(context, 0);
    if (heif_context_read_from_file(context, path.toLocal8Bit().constData(), nullptr).code ==
            heif_error_Ok &&
        heif_context_get_primary_image_handle(context, &handle).code == heif_error_Ok &&
        heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
                          options)
                .code == heif_error_Ok) {
        int stride;
        const auto* plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
        result = QImage(plane, heif_image_handle_get_width(handle),
                        heif_image_handle_get_height(handle), stride, QImage::Format_RGBA8888)
                     .copy();
        result.setColorSpace(QColorSpace::SRgb);
        if (icc) {
            QByteArray profile(qsizetype(heif_image_handle_get_raw_color_profile_size(handle)),
                               Qt::Uninitialized);
            if (heif_image_handle_get_raw_color_profile(handle, profile.data()).code ==
                heif_error_Ok) {
                result.setColorSpace(QColorSpace::fromIccProfile(profile));
                result.convertToColorSpace(QColorSpace::SRgb);
            }
        }
    }
    if (image) {
        heif_image_release(image);
    }
    if (handle) {
        heif_image_handle_release(handle);
    }
    heif_decoding_options_free(options);
    heif_context_free(context);
    heif_deinit();
    return result;
}
} // namespace

class HeifPluginTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("LicasaTests");
        QCoreApplication::setApplicationName("HeifPluginTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        preferences_->setMaximumImageMegapixels(25);
        policy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);
    }

    void gridPreviewMatchesAcrossWorkerCounts()
    {
        const QByteArray previous = qgetenv("LICASA_HEIF_DECODE_WORKERS");
        const bool wasSet = qEnvironmentVariableIsSet("LICASA_HEIF_DECODE_WORKERS");
        const auto restore = qScopeGuard([&] {
            if (wasSet) {
                qputenv("LICASA_HEIF_DECODE_WORKERS", previous);
            } else {
                qunsetenv("LICASA_HEIF_DECODE_WORKERS");
            }
        });
        const auto decode = [&](const char* workers) {
            qputenv("LICASA_HEIF_DECODE_WORKERS", workers);
            QImageReader reader(fixture("grid-48mp.heic"));
            Contract::configure(reader, 100'000'000);
            reader.setScaledSize(QSize(1200, 900));
            return reader.read();
        };
        const QImage serial = decode("0");
        QVERIFY(!serial.isNull());
        const QImage parallel = decode("20");
        QVERIFY(!parallel.isNull());
        QCOMPARE(parallel, serial);
    }

    void pinnedLibheifVersion()
    {
        // The sequence preflight deliberately requires the security-maintenance
        // pin, not merely an ABI-compatible older libheif from another prefix.
        QCOMPARE(QString::fromLatin1(heif_get_version()), QStringLiteral("1.23.4"));
    }

    void sequenceMetadataAndFrames()
    {
        const QString path = requireSequenceFixture();
        if (path.isEmpty()) {
            QSKIP("LICASA_HEIF_SEQUENCE_FIXTURE is unavailable.");
        }

        QImageReader reader(path);
        Contract::configure(reader, policy_->maximumImagePixels());
        QVERIFY2(reader.canRead(), qPrintable(codecError(reader)));
        QVERIFY(reader.supportsAnimation());
        QVERIFY(reader.supportsOption(QImageIOHandler::Animation));
        const QSize native = reader.size();
        QVERIFY(native.width() > 0 && native.height() > 0);
        QVERIFY(Contract::allows(native, policy_->maximumImagePixels()));
        QCOMPARE(reader.imageCount(), 0); // libheif exposes no cheap sample-count API.
        QCOMPARE(reader.loopCount(), 0);  // The pinned fixture plays once.
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());

        QImage first;
        int firstDelay = 0;
        for (int frame = 0; frame < 3; ++frame) {
            const QImage image = reader.read();
            QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
            QCOMPARE(image.size(), native);
            QCOMPARE(reader.currentImageNumber(), frame);
            QVERIFY(reader.nextImageDelay() >= 10 && reader.nextImageDelay() <= 60000);
            QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                     quint64(native.width()) * quint64(native.height()));
            if (frame == 0) {
                first = image;
                firstDelay = reader.nextImageDelay();
            }
        }
        QVERIFY(firstDelay > 0);
        QVERIFY(!first.isNull());

        // Qualify the pinned fixture's final sample as well. Besides covering
        // long forward traversal, this gives the hostile truncation test below
        // a known frame that must exist in the complete container.
        QVERIFY(reader.jumpToImage(pinnedSequenceLastFrame));
        const QImage last = reader.read();
        QVERIFY2(!last.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(last.size(), native);
        QCOMPARE(reader.currentImageNumber(), pinnedSequenceLastFrame);
    }

    void sequenceBackwardSeekRebuildsDecoder()
    {
        const QString path = requireSequenceFixture();
        if (path.isEmpty()) {
            QSKIP("LICASA_HEIF_SEQUENCE_FIXTURE is unavailable.");
        }

        QImageReader reader(path);
        Contract::configure(reader, policy_->maximumImagePixels());
        const QImage first = reader.read();
        QVERIFY2(!first.isNull(), qPrintable(codecError(reader)));
        QVERIFY2(!reader.read().isNull(), qPrintable(codecError(reader)));
        QCOMPARE(reader.currentImageNumber(), 1);

        QVERIFY(reader.jumpToImage(3));
        const QImage fourth = reader.read();
        QVERIFY2(!fourth.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(reader.currentImageNumber(), 3);

        // libheif's sequence cursor has no public random-access sample API.
        // Backward access must rebuild the context and replay bounded frames.
        QVERIFY(reader.jumpToImage(0));
        const QImage firstAgain = reader.read();
        QVERIFY2(!firstAgain.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(reader.currentImageNumber(), 0);
        QCOMPARE(firstAgain, first);

        QVERIFY(reader.jumpToImage(2));
        QVERIFY2(!reader.read().isNull(), qPrintable(codecError(reader)));
        QCOMPARE(reader.currentImageNumber(), 2);
    }

    void sequencePolicyAndCancellation()
    {
        const QString path = requireSequenceFixture();
        if (path.isEmpty()) {
            QSKIP("LICASA_HEIF_SEQUENCE_FIXTURE is unavailable.");
        }

        QImageReader metadata(path);
        Contract::configure(metadata, policy_->maximumImagePixels());
        const QSize native = metadata.size();
        QVERIFY(!native.isEmpty());

        QImageReader limited(path);
        Contract::configure(limited, 1);
        QCOMPARE(limited.size(), native);
        QVERIFY(limited.read().isNull());
        QVERIFY(codecError(limited).contains("limit", Qt::CaseInsensitive));
        QVERIFY(!limited.device()->property("_licasaNativeRasterPixels").isValid());

        std::atomic_bool stop{true};
        QImageReader cancelled(path);
        Contract::configure(cancelled, policy_->maximumImagePixels(), &stop);
        QVERIFY(cancelled.read().isNull());
        QVERIFY(!cancelled.device()->property("_licasaNativeRasterPixels").isValid());
    }

    void malformedSequenceFailsSafely()
    {
        const QString path = requireSequenceFixture();
        if (path.isEmpty()) {
            QSKIP("LICASA_HEIF_SEQUENCE_FIXTURE is unavailable.");
        }
        const QByteArray valid = contents(path);
        QVERIFY(valid.size() > 1024);

        const qsizetype metadataLengths[] = {16, 64, 256, 1024};
        for (const qsizetype length : metadataLengths) {
            QByteArray truncated = valid.left(length);
            QBuffer buffer(&truncated);
            QVERIFY(buffer.open(QIODevice::ReadOnly));
            QImageReader reader(&buffer, "heics");
            Contract::configure(reader, policy_->maximumImagePixels());
            QVERIFY2(reader.read().isNull(),
                     qPrintable(QString("accepted HEIF sequence truncation at %1").arg(length)));
            QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());
        }

        // Half of this pinned file still contains a complete first sample, so
        // decoding only frame 0 is not a valid truncation check. Force traversal
        // to the known final sample; missing media data must fail safely before
        // a raster for that frame is published.
        QByteArray truncated = valid.left(valid.size() / 2);
        QBuffer buffer(&truncated);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "heics");
        Contract::configure(reader, policy_->maximumImagePixels());
        if (reader.jumpToImage(pinnedSequenceLastFrame)) {
            QVERIFY2(reader.read().isNull(),
                     qPrintable(QString("accepted half-truncated HEIF sequence through frame %1")
                                    .arg(pinnedSequenceLastFrame)));
        } else {
            // Metadata-level rejection is equally valid, but must not publish pixels.
            QVERIFY(reader.read().isNull());
        }
        QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());
    }

    void sequenceGenericAnimationIntegration()
    {
        const QString path = requireSequenceFixture();
        if (path.isEmpty()) {
            QSKIP("LICASA_HEIF_SEQUENCE_FIXTURE is unavailable.");
        }

        AnimationViewer viewer;
        QVERIFY(viewer.animation);
        viewer.animation->setSource(QUrl::fromLocalFile(path));
        viewer.animation->setPlaying(false);
        viewer.animation->setActive(true);

        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 0, 15000);
        QVERIFY2(viewer.animation->error().isEmpty(), qPrintable(viewer.animation->error()));
        const QImage first = viewer.frame();
        QVERIFY(!first.isNull());

        viewer.animation->seekFrame(3);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 3, 15000);
        QVERIFY2(viewer.animation->error().isEmpty(), qPrintable(viewer.animation->error()));
        QVERIFY(!viewer.frame().isNull());

        viewer.animation->seekFrame(0);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 0, 15000);
        QVERIFY2(viewer.animation->error().isEmpty(), qPrintable(viewer.animation->error()));
        QCOMPARE(viewer.frame(), first);
        viewer.animation->setActive(false);
    }

#ifdef LICASA_HEIC_MOTION_FIXTURE
    void xmpMetadataBridgeIsMetadataOnly()
    {
        QImageReader reader(QStringLiteral(LICASA_HEIC_MOTION_FIXTURE));
        Contract::configure(reader, policy_->maximumImagePixels());
        QVERIFY(reader.device());
        reader.device()->setProperty(Contract::appleLivePhotoExifProbeProperty, true);
        QVERIFY2(reader.canRead(), qPrintable(codecError(reader)));
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
        const QString bridgeError = reader.text(QStringLiteral("LicasaXmpError"));
        QVERIFY2(bridgeError.isEmpty(), qPrintable(bridgeError));
        const QByteArray encoded = reader.text(QStringLiteral("LicasaXmpBase64")).toLatin1();
        QVERIFY(!encoded.isEmpty());
        const QByteArray xmp = QByteArray::fromBase64(encoded);
        QCOMPARE(xmp.toBase64(), encoded);
        QVERIFY(xmp.size() <= 1024 * 1024);
        QVERIFY(xmp.contains("MotionPhoto"));
        const QString exifError = reader.text(QStringLiteral("LicasaHeifExifError"));
        QVERIFY2(exifError.isEmpty(), qPrintable(exifError));
        const QByteArray exifEncoded =
            reader.text(QStringLiteral("LicasaHeifExifBase64")).toLatin1();
        if (!exifEncoded.isEmpty()) {
            const QByteArray exif = QByteArray::fromBase64(exifEncoded);
            QCOMPARE(exif.toBase64(), exifEncoded);
            QVERIFY(exif.size() >= 4);
            QVERIFY(exif.size() <= 1024 * 1024 + 4);
        }
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
    }
#endif

    void metadataDoesNotRasterize()
    {
        QImageReader reader(fixture("grid-48mp.heic"));
        Contract::configure(reader, policy_->maximumImagePixels());
        QVERIFY(reader.canRead());
        QCOMPARE(reader.size(), QSize(8000, 6000));
        QCOMPARE(reader.imageCount(), 1);
        QVERIFY(!reader.supportsAnimation());
        QVERIFY(reader.supportsOption(QImageIOHandler::ScaledSize));
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
        // It is safe to report dimensions even when the primary exceeds the
        // selected admission limit. Full decode must still fail before pixels.
        QVERIFY(reader.read().isNull());
        QVERIFY(codecError(reader).contains("limit"));
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
    }

    void oversizedGridUsesSmallTiles()
    {
        QImageReader reader(fixture("grid-48mp.heic"));
        Contract::configure(reader, policy_->maximumImagePixels());
        reader.setScaledSize(QSize(1200, 900));
        const QImage image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(image.size(), QSize(1200, 900));
        QVERIFY(reader.text("PreviewPath").contains("streamed grid tiles"));
        QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                 quint64(512 * 512));
        for (int y = 5; y < image.height(); y += 31) {
            for (int x = 5; x < image.width(); x += 31) {
                const auto color = image.pixelColor(x, y);
                QVERIFY(qAbs(color.red() - (20 + x * 210 / 1200)) < 9);
                QVERIFY(qAbs(color.green() - (20 + y * 210 / 900)) < 9);
                QCOMPARE(color.alpha(), 255);
            }
        }
    }

    void embeddedPreviewAvoidsPrimaryRaster()
    {
        QImageReader reader(fixture("grid-48mp-preview.heic"));
        Contract::configure(reader, policy_->maximumImagePixels());
        reader.setScaledSize(QSize(1200, 900));
        const auto image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(image.size(), QSize(1200, 900));
        QVERIFY(reader.text("PreviewPath").contains("embedded thumbnail"));
        QCOMPARE(reader.device()->property(Contract::embeddedPreviewSizeProperty).toSize(),
                 QSize(1600, 1200));
        QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                 quint64(1600 * 1200));
    }

    void singleCodedImageUsesAuthoritativePixelLimit()
    {
        QImageReader preview(fixture("single-48mp.heic"));
        Contract::configure(preview, policy_->maximumImagePixels());
        QCOMPARE(preview.size(), QSize(8000, 6000));
        preview.setScaledSize(QSize(1200, 900));
        QVERIFY(preview.read().isNull());
        QVERIFY(codecError(preview).contains("limit"));
        QVERIFY(!preview.device()->property("_licasaNativeRasterPixels").isValid());
        QImageReader full(fixture("single-48mp.heic"));
        Contract::configure(full, policy_->maximumImagePixels());
        QVERIFY(full.read().isNull()); // 48 MP still exceeds the selected 25 MP.
        QVERIFY(!full.device()->property("_licasaNativeRasterPixels").isValid());

        // User's explicit preference: decode automatically when the full
        // working raster fits the selected policy, even without a fast path.
        preferences_->setMaximumImageMegapixels(100);
        {
            QImageReader admitted(fixture("single-48mp.heic"));
            Contract::configure(admitted, policy_->maximumImagePixels());
            admitted.setScaledSize(QSize(1200, 900));
            const QImage result = admitted.read();
            preferences_->setMaximumImageMegapixels(25);
            QVERIFY2(!result.isNull(), qPrintable(codecError(admitted)));
            QCOMPARE(result.size(), QSize(1200, 900));
            QCOMPARE(admitted.device()->property("_licasaNativeRasterPixels").toULongLong(),
                     quint64(48'000'000));
        }
    }

    void matchesNativeAssembly_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QSize>("expectedSize");
        QTest::addColumn<bool>("icc");
        QTest::newRow("rotation and edge crop") << "grid-rotated.heic" << QSize(700, 1000) << false;
        QTest::newRow("mirror and edge crop") << "grid-mirrored.heic" << QSize(1000, 700) << false;
        QTest::newRow("alpha") << "with-alpha-512x512.heic" << QSize(512, 512) << false;
        QTest::newRow("clean aperture") << "clap_cropped.heic" << QSize(64, 64) << false;
        QTest::newRow("grid clean aperture") << "grid-cropped.heic" << QSize(800, 500) << false;
        QTest::newRow("display P3 color conversion") << "display-p3.heic" << QSize(64, 48) << true;
    }

    void matchesNativeAssembly()
    {
        QFETCH(QString, name);
        QFETCH(QSize, expectedSize);
        QFETCH(bool, icc);
        const QString path = fixture(name.toLatin1().constData());
        const QImage reference = referenceDecode(path, icc);
        QVERIFY(!reference.isNull());
        QCOMPARE(reference.size(), expectedSize);
        QImageReader reader(path);
        Contract::configure(reader, policy_->maximumImagePixels());
        reader.setAutoTransform(true);
        QCOMPARE(reader.size(), expectedSize);
        const QImage image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(image, reference);
    }

    void providerAndSaveRespectTheSamePolicy()
    {
        Licasa::AsyncImageProvider provider(*policy_);
        const QString url =
            QUrl::fromLocalFile(fixture("grid-48mp.heic")).toString(QUrl::FullyEncoded);
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(url, QSize(1200, 900)));
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
        QVERIFY2(response->errorString().isEmpty(), qPrintable(response->errorString()));
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY(texture);
        QCOMPARE(texture->textureSize(), QSize(1200, 900));
        QTemporaryDir output;
        Licasa::ImageSaveService save(*policy_);
        QSignalSpy failure(&save, &Licasa::ImageSaveService::saveFailed);
        save.save(QUrl::fromLocalFile(fixture("grid-48mp.heic")),
                  QUrl::fromLocalFile(output.filePath("blocked.png")), {});
        QTRY_COMPARE_WITH_TIMEOUT(failure.count(), 1, 15000);
        QVERIFY(!QFile::exists(output.filePath("blocked.png")));
    }

    void cancellationBeforeAndDuringDecode()
    {
        std::atomic_bool stop{true};
        QImageReader before(fixture("grid-48mp.heic"));
        Contract::configure(before, policy_->maximumImagePixels(), &stop);
        before.setScaledSize(QSize(1200, 900));
        QVERIFY(before.read().isNull());
        QVERIFY(!before.device()->property("_licasaNativeRasterPixels").isValid());

        // Cancel at an input boundary during real grid work; the next tile
        // must stop. The object and atomic outlive both reader and native calls.
        class CancellingBuffer final : public QBuffer {
          public:
            std::atomic_bool* flag = nullptr;
            qint64 remaining = -1;
            qint64 readData(char* bytes, qint64 size) override
            {
                const qint64 count = QBuffer::readData(bytes, size);
                if (remaining >= 0 && count > 0) {
                    remaining -= count;
                    if (remaining <= 0) {
                        flag->store(true);
                    }
                }
                return count;
            }
        } buffer;
        stop.store(false);
        buffer.flag = &stop;
        buffer.setData(contents(fixture("grid-48mp.heic")));
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader during(&buffer);
        Contract::configure(during, policy_->maximumImagePixels(), &stop);
        QCOMPARE(during.size(), QSize(8000, 6000));
        buffer.remaining = 16384;
        during.setScaledSize(QSize(1200, 900));
        QVERIFY(during.read().isNull());
        QVERIFY(stop.load());
    }

    void returnedImageOutlivesItsReader()
    {
        QImage retained;
        {
            QImageReader reader(fixture("with-alpha-512x512.heic"));
            Contract::configure(reader, policy_->maximumImagePixels());
            retained = reader.read();
            QVERIFY(!retained.isNull());
        }
        // Native planes may be wrapped without a copy. Destroying the reader
        // and its libheif context must leave that returned image valid.
        const QImage copy = retained.copy();
        QCOMPARE(retained, copy);
        retained = {};
        QVERIFY(!copy.isNull());
    }

    void malformedInputs()
    {
        const QByteArray valid = contents(fixture("display-p3.heic"));
        QVERIFY(!valid.isEmpty());
        auto reject = [&](QByteArray data) {
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "heic");
            Contract::configure(reader, policy_->maximumImagePixels());
            reader.setScaledSize(QSize(1200, 900));
            return reader.read().isNull();
        };
        for (qsizetype length = 0; length < valid.size(); length += 13) {
            QVERIFY2(reject(valid.left(length)),
                     qPrintable(QString("accepted truncation at %1").arg(length)));
        }
        for (const auto& replacement :
             {QByteArray::fromHex("0000000000000000"), QByteArray::fromHex("ffffffffffffffff"),
              QByteArray::fromHex("7fffffff7fffffff")}) {
            auto data = valid;
            const auto position = data.indexOf("ispe");
            QVERIFY(position >= 0);
            data.replace(position + 8, 8, replacement);
            QVERIFY(reject(data));
        }
        QRandomGenerator random(0x1ca5a);
        for (int trial = 0; trial < 200; ++trial) {
            auto data = valid;
            for (int mutation = 0; mutation < 4; ++mutation) {
                data[random.bounded(quint32(data.size()))] = char(random.generate());
            }
            // Mutations can remain valid. Exercise decode and require every
            // accepted result to retain the requested bound, not a fixed error.
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "heic");
            Contract::configure(reader, policy_->maximumImagePixels());
            reader.setScaledSize(QSize(1200, 900));
            const QImage image = reader.read();
            QVERIFY(image.isNull() || (image.width() <= 1200 && image.height() <= 900));
        }
    }

  private:
    QTemporaryDir settings_;
    std::unique_ptr<Licasa::ViewerPreferences> preferences_;
    std::unique_ptr<Licasa::ImageResourcePolicy> policy_;
};

int main(int argc, char** argv)
{
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    QGuiApplication application(argc, argv);
    Licasa::initializeImagePlugins();
    HeifPluginTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_heif_plugin.moc"
