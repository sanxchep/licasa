#include "app/application_settings.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"

#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QQuickTextureFactory>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace Contract = Licasa::ImageDecodeContract;
namespace {
QString fixture(const char* name)
{
    return QStringLiteral(LICASA_SOURCE_DIR "/tests/test-assets/modern/jxl/") + QString::fromLatin1(name);
}
QString codecError(QImageReader& reader)
{
    return reader.device()->property(Contract::errorProperty).toString();
}
QByteArray contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
qint64 rssKiB()
{
    const auto status = contents(QStringLiteral("/proc/self/status"));
    for (const auto& line : status.split('\n')) {
        if (line.startsWith("VmRSS:")) {
            return line.mid(6).simplified().split(' ').first().toLongLong();
        }
    }
    return 0;
}
} // namespace

class JxlPluginTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("LicasaTests");
        QCoreApplication::setApplicationName("JxlPluginTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        preferences_->setMaximumImageMegapixels(25);
        policy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);
    }
    void fullImageMatchesAcrossWorkerCounts()
    {
        const QByteArray previous = qgetenv("LICASA_JXL_DECODE_WORKERS");
        const bool wasSet = qEnvironmentVariableIsSet("LICASA_JXL_DECODE_WORKERS");
        const auto restore = qScopeGuard([&] {
            if (wasSet) {
                qputenv("LICASA_JXL_DECODE_WORKERS", previous);
            } else {
                qunsetenv("LICASA_JXL_DECODE_WORKERS");
            }
        });
        const auto decode = [&](const char* workers) {
            qputenv("LICASA_JXL_DECODE_WORKERS", workers);
            QImageReader reader(fixture("large-48mp.jxl"));
            Contract::configure(reader, 100'000'000);
            return reader.read();
        };
        const QImage serial = decode("0");
        QVERIFY(!serial.isNull());
        const QImage parallel = decode("20");
        QVERIFY(!parallel.isNull());
        QCOMPARE(parallel, serial);
    }
    void metadataAndOversizedAdmission()
    {
        QImageReader reader(fixture("large-48mp.jxl"));
        Contract::configure(reader, policy_->maximumImagePixels());
        QVERIFY(reader.canRead());
        QCOMPARE(reader.size(), QSize(8000, 6000));
        QCOMPARE(reader.imageCount(), 1);
        QVERIFY(!reader.supportsAnimation());
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
        reader.setScaledSize(QSize(1200, 900));
        QVERIFY(reader.read().isNull());
        QVERIFY(codecError(reader).contains("limit"));
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
    }
    void nativePixelsMatchReferences_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("rotation");
        QTest::newRow("sRGB") << "full-hd.jxl" << 0;
        QTest::newRow("alpha") << "alpha.jxl" << 0;
        QTest::newRow("orientation") << "rotated.jxl" << 90;
        QTest::newRow("Display P3") << "display-p3.jxl" << 0;
    }
    void nativePixelsMatchReferences()
    {
        QFETCH(QString, name);
        QFETCH(int, rotation);
        const auto path = fixture(name.toLatin1().constData());
        QImage expected(path + ".png");
        QVERIFY(!expected.isNull());
        expected.convertToColorSpace(QColorSpace::SRgb);
        if (rotation) {
            expected = expected.transformed(QTransform().rotate(rotation));
        }
        QImageReader reader(path);
        Contract::configure(reader, policy_->maximumImagePixels());
        reader.setAutoTransform(true);
        QCOMPARE(reader.size(), expected.size());
        const QImage actual = reader.read();
        QVERIFY2(!actual.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(actual.size(), expected.size());
        QCOMPARE(actual.colorSpace(), QColorSpace(QColorSpace::SRgb));
        for (int y = 0; y < actual.height(); y += 3) {
            for (int x = 0; x < actual.width(); x += 3) {
                const auto a = actual.pixelColor(x, y);
                const auto e = expected.pixelColor(x, y);
                // Independent Qt CMS and libjxl/skcms may round differently.
                QVERIFY2(qAbs(a.red() - e.red()) <= 2 && qAbs(a.green() - e.green()) <= 2 &&
                             qAbs(a.blue() - e.blue()) <= 2,
                         qPrintable(QString("color at %1,%2: %3 vs %4")
                                        .arg(x)
                                        .arg(y)
                                        .arg(a.name())
                                        .arg(e.name())));
                QCOMPARE(a.alpha(), e.alpha());
            }
        }
    }
    void embeddedPreviewSkipsPrimary_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QSize>("natural");
        QTest::addColumn<QSize>("target");
        QTest::newRow("normal") << "embedded-preview.jxl" << QSize(2048, 1536) << QSize(400, 300);
        QTest::newRow("rotated") << "rotated-preview.jxl" << QSize(1536, 2048) << QSize(300, 400);
    }
    void embeddedPreviewSkipsPrimary()
    {
        QFETCH(QString, name);
        QFETCH(QSize, natural);
        QFETCH(QSize, target);
        QImageReader reader(fixture(name.toLatin1().constData()));
        // Exercise exact per-reader admission independently of the preference's
        // UI range: 3 MP primary is rejected, 196608-pixel preview is admitted.
        Contract::configure(reader, 200000);
        QCOMPARE(reader.size(), natural);
        QCOMPARE(reader.device()->property(Contract::embeddedPreviewSizeProperty).toSize(),
                 natural.width() > natural.height() ? QSize(512, 384) : QSize(384, 512));
        reader.setScaledSize(target);
        const auto image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(image.size(), target);
        QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                 quint64(512 * 384));
        QVERIFY(reader.text("PreviewPath").contains("embedded preview"));
        QImageReader full(fixture(name.toLatin1().constData()));
        Contract::configure(full, 200000);
        QVERIFY(full.read().isNull());
        QVERIFY(codecError(full).contains("limit"));
    }
    void admittedLargeRasterIsReportedHonestly()
    {
        QImageReader reader(fixture("large-48mp.jxl"));
        Contract::configure(reader, 100000000);
        reader.setScaledSize(QSize(1200, 900));
        const auto image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(image.size(), QSize(1200, 900));
        QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                 quint64(48000000));
        QVERIFY(reader.text("PreviewPath").contains("full native raster required"));
    }
    void animationTimingAndSeeking()
    {
        QImageReader reader(fixture("animated.jxl"));
        Contract::configure(reader, policy_->maximumImagePixels());
        QVERIFY(reader.supportsAnimation());
        QCOMPARE(reader.imageCount(), 0); // unknown without scanning all frames
        QCOMPARE(reader.loopCount(), 1);  // two plays, one repeat
        for (int frame = 0; frame < 3; ++frame) {
            const auto image = reader.read();
            QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
            QCOMPARE(reader.currentImageNumber(), frame);
            QCOMPARE(reader.nextImageDelay(), frame % 2 ? 120 : 40);
            QCOMPARE(image.pixelColor(64, 48).red(), (frame * 29 + 31) % 256);
        }
        QCOMPARE(reader.imageCount(), 3);
        QVERIFY(!reader.canRead());
        QVERIFY(reader.jumpToImage(1));
        const auto second = reader.read();
        QVERIFY2(!second.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(reader.currentImageNumber(), 1);
        QCOMPARE(second.pixelColor(64, 48).red(), 60);
        QVERIFY(reader.jumpToImage(0));
        QCOMPARE(reader.read().pixelColor(64, 48).red(), 31);
    }
    void longAnimationKeepsBoundedMemory()
    {
        QImageReader reader(fixture("animation-100-4k.jxl"));
        Contract::configure(reader, policy_->maximumImagePixels());
        const qint64 baseline = rssKiB();
        qint64 peak = baseline;
        quint64 nativePeak = 0;
        for (int frame = 0; frame < 100; ++frame) {
            const auto image = reader.read();
            QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
            QCOMPARE(image.size(), QSize(3840, 2160));
            QCOMPARE(image.pixelColor(1920, 1080).red(), (frame * 29 + 31) % 256);
            peak = std::max(peak, rssKiB());
            nativePeak = std::max(
                nativePeak, reader.device()->property("_licasaNativePeakBytes").toULongLong());
        }
        QCOMPARE(reader.imageCount(), 100);
        // Four references and temporary native working buffers are allowed;
        // retaining 100 output frames would instead exceed three GiB.
        QVERIFY(nativePeak < quint64(256) * 1024 * 1024);
#if !defined(__SANITIZE_ADDRESS__)
        if (baseline > 0) {
            QVERIFY2(peak - baseline < 384 * 1024, qPrintable(QString::number(peak - baseline)));
        }
#endif
        qInfo() << "100-frame JXL peak delta KiB:" << peak - baseline
                << "native bytes:" << nativePeak;
    }
    void cancellationAndLifetime()
    {
        std::atomic_bool stop{true};
        QImageReader before(fixture("full-hd.jxl"));
        Contract::configure(before, policy_->maximumImagePixels(), &stop);
        QVERIFY(before.read().isNull());
        stop.store(false);
        QImageReader during(fixture("large-48mp.jxl"));
        Contract::configure(during, 100000000, &stop);
        QCOMPARE(during.size(), QSize(8000, 6000));
        std::thread cancellation([&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stop.store(true);
        });
        QElapsedTimer elapsed;
        elapsed.start();
        const auto cancelled = during.read();
        cancellation.join();
        QVERIFY(cancelled.isNull());
        QVERIFY(elapsed.elapsed() < 5000);
        // Both small heap images and large mapped images must survive their
        // reader, including an implicit-sharing owner that outlives the first.
        for (const char* name : {"alpha.jxl", "full-hd.jxl"}) {
            QImage retained;
            {
                QImageReader reader(fixture(name));
                Contract::configure(reader, policy_->maximumImagePixels());
                retained = reader.read();
                QVERIFY(!retained.isNull());
            }
            QImage shared = retained;
            retained = {};
            QCOMPARE(shared, shared.copy());
        }
    }
    void providerAndSaveUseSharedAdmission()
    {
        Licasa::AsyncImageProvider provider(*policy_);
        const auto source = QUrl::fromLocalFile(fixture("large-48mp.jxl"));
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(source.toString(QUrl::FullyEncoded), QSize(1200, 900)));
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
        QVERIFY(response->errorString().contains("limit"));
        QTemporaryDir output;
        Licasa::ImageSaveService save(*policy_);
        QSignalSpy failure(&save, &Licasa::ImageSaveService::saveFailed);
        save.save(source, QUrl::fromLocalFile(output.filePath("blocked.png")), {});
        QTRY_COMPARE_WITH_TIMEOUT(failure.count(), 1, 15000);
        QVERIFY(!QFile::exists(output.filePath("blocked.png")));
    }
    void malformedAndAllocationFailures()
    {
        const auto valid = contents(fixture("alpha.jxl"));
        QVERIFY(!valid.isEmpty());
        auto read = [&](QByteArray bytes, quint64 limit) {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "jxl");
            Contract::configure(reader, limit);
            reader.setScaledSize(QSize(64, 48));
            return reader.read();
        };
        for (qsizetype count = 0; count < valid.size(); count += 101) {
            QVERIFY2(read(valid.first(count), policy_->maximumImagePixels()).isNull(),
                     qPrintable(QString::number(count)));
        }
        QVERIFY(read(valid, 13000).isNull()); // native scratch exceeds this exact working allowance
        QRandomGenerator random(0x1234aabb);
        for (int index = 0; index < 100; ++index) {
            auto data = valid;
            const qsizetype offset = random.bounded(quint32(data.size()));
            data[offset] = char(random.generate());
            const auto result = read(data, policy_->maximumImagePixels());
            QVERIFY(result.isNull() ||
                    Contract::allows(result.size(), policy_->maximumImagePixels()));
        }
        QVERIFY(read(QByteArray::fromHex("ff0affffffffffffffffffffffffffffff"),
                     policy_->maximumImagePixels())
                    .isNull());
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
    JxlPluginTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_jxl_plugin.moc"
