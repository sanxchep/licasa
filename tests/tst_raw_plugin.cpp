#include "app/application_settings.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_probe.h"
#include "imaging/image_resource_policy.h"
#include "plugins/raw_stream.h"

#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QQuickTextureFactory>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

namespace Contract = Licasa::ImageDecodeContract;
namespace {
QString fixture(const char* name)
{
    return QStringLiteral(LICASA_SOURCE_DIR "/tests/test-assets/modern/raw/") + QString::fromLatin1(name);
}
QString error(QImageReader& reader)
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
QImage pattern(int w, int h)
{
    QImage image(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            image.setPixelColor(
                x, y,
                QColor(31 + x * 180 / w, 47 + y * 160 / h, 67 + ((x / 32 + y / 32) % 2) * 120));
        }
    }
    return image;
}
} // namespace

class RawPluginTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("LicasaTests");
        QCoreApplication::setApplicationName("RawPluginTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        preferences_->setMaximumImageMegapixels(25);
        policy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);
    }
    void previewFirstAndOrientation_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("rotated");
        QTest::newRow("preview without scaled request") << "small.dng" << false;
        QTest::newRow("orientation") << "rotated.dng" << true;
        QTest::newRow("largest preview") << "multiple-previews.dng" << false;
    }
    void previewFirstAndOrientation()
    {
        QFETCH(QString, name);
        QFETCH(bool, rotated);
        QImage image;
        {
            QImageReader reader(fixture(qPrintable(name)));
            Contract::configure(reader, policy_->maximumImagePixels());
            QVERIFY2(reader.canRead(), qPrintable(error(reader)));
            QCOMPARE(reader.size(), rotated ? QSize(1536, 2048) : QSize(2048, 1536));
            QVERIFY(reader.device()->property(Contract::previewFirstProperty).toBool());
            QVERIFY(!reader.device()->property("_licasaRawDemosaic").isValid());
            QVERIFY(!reader.supportsAnimation());
            image = reader.read();
            QVERIFY2(!image.isNull(), qPrintable(error(reader)));
            QCOMPARE(reader.device()->property("_licasaRawDemosaic").toBool(), false);
            QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                     quint64(512 * 384));
            QVERIFY(reader.text("PreviewPath").contains("no demosaic"));
        }
        QImage expected = pattern(512, 384);
        if (rotated) {
            expected = expected.transformed(QTransform().rotate(90));
        }
        QCOMPARE(image.size(), expected.size());
        QCOMPARE(image.colorSpace(), QColorSpace(QColorSpace::SRgb));
        for (int y = 0; y < image.height(); y += 7) {
            for (int x = 0; x < image.width(); x += 7) {
                QCOMPARE(image.pixelColor(x, y), expected.pixelColor(x, y));
            }
        }
    }
    void oversizedSensorKeepsPreview()
    {
        QImageReader reader(fixture("large-48mp.dng"));
        Contract::configure(reader, 25000000);
        QCOMPARE(reader.size(), QSize(8000, 6000));
        QCOMPARE(reader.device()->property(Contract::sensorSizeProperty).toSize(),
                 QSize(8000, 6000));
        reader.setScaledSize(QSize(800, 600));
        const auto image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(error(reader)));
        QCOMPARE(image.size(), QSize(800, 600));
        QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                 quint64(1024 * 768));
        QImageReader full(fixture("large-48mp.dng"));
        Contract::configure(full, 25000000, nullptr, true);
        full.setScaledSize(QSize(800, 600));
        QVERIFY(full.read().isNull());
        QVERIFY(error(full).contains("limit"));
        QVERIFY(!full.device()->property("_licasaRawDemosaic").isValid());

        QImageReader tiny(fixture("multiple-previews.dng"));
        Contract::configure(tiny, 20000);
        const auto fallback = tiny.read();
        QVERIFY2(!fallback.isNull(), qPrintable(error(tiny)));
        QCOMPARE(fallback.size(), QSize(128, 96));
    }
    void fullDevelopmentRequiresRequest()
    {
        QImageReader preview(fixture("no-preview.dng"));
        Contract::configure(preview, 25000000);
        QVERIFY(preview.canRead());
        QVERIFY(preview.read().isNull());
        QVERIFY(error(preview).contains("embedded preview"));
        QVERIFY(!preview.device()->property("_licasaRawDemosaic").isValid());
        for (const char* name : {"small.dng", "no-preview.dng", "rotated.dng"}) {
            QImageReader full(fixture(name));
            Contract::configure(full, 25000000, nullptr, true);
            const auto image = full.read();
            QVERIFY2(!image.isNull(), qPrintable(error(full)));
            QCOMPARE(image.size(),
                     QByteArray(name) == "rotated.dng" ? QSize(1536, 2048) : QSize(2048, 1536));
            QVERIFY(full.device()->property("_licasaRawDemosaic").toBool());
            QVERIFY(image.pixelColor(image.width() / 2, image.height() / 2) != QColor(0, 0, 0));
        }
    }
    void fastDevelopmentProvidesBoundedFallback()
    {
        QImageReader fast(fixture("no-preview.dng"));
        Contract::configure(fast, 25000000, nullptr, true, true);
        fast.setScaledSize(QSize(800, 600));
        const auto image = fast.read();
        QVERIFY2(!image.isNull(), qPrintable(error(fast)));
        QCOMPARE(image.size(), QSize(800, 600));
        QVERIFY(fast.device()->property("_licasaRawDemosaic").toBool());
        QVERIFY(fast.text("PreviewPath").contains("half-size"));
        QCOMPARE(image.colorSpace(), QColorSpace(QColorSpace::SRgb));

        QImageReader full(fixture("no-preview.dng"));
        Contract::configure(full, 25000000, nullptr, true);
        const auto fullImage = full.read();
        QVERIFY2(!fullImage.isNull(), qPrintable(error(full)));
        QVERIFY(fullImage.width() > image.width());
        QVERIFY(fullImage.height() > image.height());
    }

    void interactiveDevelopmentKeepsExportQualitySeparate()
    {
        QImageReader interactive(fixture("no-preview.dng"));
        Contract::configure(interactive, 25000000, nullptr, true, false, true);
        const auto interactiveImage = interactive.read();
        QVERIFY2(!interactiveImage.isNull(), qPrintable(error(interactive)));
        QCOMPARE(interactiveImage.size(), QSize(2048, 1536));
        QCOMPARE(interactiveImage.format(), QImage::Format_ARGB32);
        QVERIFY(interactive.text("PreviewPath").contains("PPG"));

        QImageReader exportQuality(fixture("no-preview.dng"));
        Contract::configure(exportQuality, 25000000, nullptr, true);
        const auto exportImage = exportQuality.read();
        QVERIFY2(!exportImage.isNull(), qPrintable(error(exportQuality)));
        QCOMPARE(exportImage.size(), interactiveImage.size());
        QCOMPARE(exportImage.format(), QImage::Format_RGB888);
        QVERIFY(exportQuality.text("PreviewPath").contains("AHD"));
    }

    void saveAndProbeRespectRawPixels()
    {
        Licasa::ImageProbe probe(*policy_);
        const auto source = QUrl::fromLocalFile(fixture("no-preview.dng"));
        QCOMPARE(probe.inspect(source).value("size").toSize(), QSize(2048, 1536));
        QVERIFY(probe.inspect(source).value("previewFirst").toBool());
        QVERIFY(probe.inspect(source).value("withinBudget").toBool());
        const auto large = QUrl::fromLocalFile(fixture("large-48mp.dng"));
        QVERIFY(!probe.inspect(large).value("withinBudget").toBool());
        preferences_->setMaximumImageMegapixels(100);
        QVERIFY(probe.inspect(large).value("withinBudget").toBool());
        preferences_->setMaximumImageMegapixels(25);
        QVERIFY(!probe.inspect(large).value("withinBudget").toBool());
        QTemporaryDir output;
        const auto destination = QUrl::fromLocalFile(output.filePath("developed.png"));
        Licasa::ImageSaveService service(*policy_);
        QSignalSpy finished(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);
        QVERIFY(service.save(source, destination, {}));
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(QImageReader(destination.toLocalFile()).size(), QSize(2048, 1536));
        // Explicit provider promotion and save both develop the sensor image.
        Licasa::AsyncImageProvider provider(*policy_);
        const auto id = QStringLiteral("view/") +
                        QString::fromLatin1(QUrl::toPercentEncoding(source.toLocalFile())) +
                        QStringLiteral("?licasa_stage=full");
        std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(id, {}));
        QSignalSpy ready(response.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY2(texture, qPrintable(response->errorString()));
        QCOMPARE(texture->textureSize(), QSize(2048, 1536));
    }
    void cancellationAndWorker()
    {
        std::atomic_bool cancelled = true;
        QImageReader cancelledReader(fixture("large-48mp.dng"));
        // Keep this check on Licasa's RAW handler. Qt can retry its TIFF
        // plugin after an auto-detected DNG read is cancelled.
        cancelledReader.setFormat("dng");
        Contract::configure(cancelledReader, 100000000, &cancelled, true);
        QVERIFY(Contract::cancelled(cancelledReader.device()));
        QVERIFY(cancelledReader.read().isNull());
        cancelled = false;
        QElapsedTimer elapsed;
        elapsed.start();
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            cancelled = true;
        });
        QImage image;
        {
            QImageReader reader(fixture("large-48mp.dng"));
            Contract::configure(reader, 100000000, &cancelled, true);
            image = reader.read();
        }
        cancel.join();
        QVERIFY(image.isNull());
        QVERIFY(elapsed.elapsed() < 3000);
        Licasa::AsyncImageProvider provider(*policy_);
        const auto id = QStringLiteral("view/") +
                        QString::fromLatin1(QUrl::toPercentEncoding(fixture("large-48mp.dng")));
        std::unique_ptr<QQuickImageResponse> response(
            provider.requestImageResponse(id, QSize(800, 600)));
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY2(texture, qPrintable(response->errorString()));
        QCOMPARE(texture->textureSize(), QSize(800, 600));
    }
    void boundedStreamArithmetic()
    {
        QByteArray bytes("1234 1.5\n", 9);
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        Licasa::RawStream stream(&buffer);
        QCOMPARE(stream.seek(std::numeric_limits<INT64>::min(), SEEK_CUR), -1);
        QCOMPARE(stream.seek(std::numeric_limits<INT64>::max(), SEEK_END), -1);
        QCOMPARE(stream.tell(), INT64(0));
        int value = 0;
        QCOMPARE(stream.scanf_one("%d", &value), 1);
        QCOMPARE(value, 1234);
        float decimal = 0;
        QCOMPARE(stream.scanf_one("%f", &decimal), 1);
        QCOMPARE(decimal, 1.5f);
        QVERIFY(stream.eof());
        QVERIFY_EXCEPTION_THROWN(stream.read(&value, std::numeric_limits<size_t>::max(), 2),
                                 LibRaw_exceptions);
        QCOMPARE(stream.seek(0, SEEK_SET), 0);
        stream.setReadLimit(1);
        QVERIFY_EXCEPTION_THROWN(stream.read(&value, 1, 2), LibRaw_exceptions);
        QVERIFY(stream.limitReached());
        std::atomic_bool cancelled = true;
        buffer.setProperty(Contract::cancellationProperty,
                           QVariant::fromValue(reinterpret_cast<quintptr>(&cancelled)));
        QVERIFY_EXCEPTION_THROWN(stream.get_char(), LibRaw_exceptions);
    }
    void malformedPreviewsAndMetadata()
    {
        for (const char* name :
             {"broken-preview-offset.dng", "oversized-preview.dng", "truncated.dng"}) {
            QImageReader reader(fixture(name));
            Contract::configure(reader, 25000000);
            reader.setScaledSize(QSize(64, 48));
            QVERIFY2(reader.read().isNull(), name);
            QVERIFY(!reader.device()->property("_licasaRawDemosaic").isValid());
        }
        const auto original = contents(fixture("small.dng"));
        QVERIFY(!original.isEmpty());
        QRandomGenerator random(0x2ea792bc);
        QElapsedTimer elapsed;
        elapsed.start();
        for (int i = 0; i < 160; ++i) {
            auto data = original;
            if (i < 60) {
                data.truncate(i * 11);
            } else {
                for (int j = 0; j < 8; ++j) {
                    data[random.bounded(1024)] = char(random.generate());
                }
            }
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "dng");
            Contract::configure(reader, 25000000);
            reader.setScaledSize(QSize(64, 48));
            const auto image = reader.read();
            QVERIFY(image.isNull() || Contract::allows(image.size(), 25000000));
            QVERIFY(!buffer.property("_licasaRawDemosaic").toBool());
        }
        QVERIFY(elapsed.elapsed() < 15000);
    }

  private:
    QTemporaryDir settings_;
    std::unique_ptr<Licasa::ViewerPreferences> preferences_;
    std::unique_ptr<Licasa::ImageResourcePolicy> policy_;
};
int main(int argc, char** argv)
{
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    QGuiApplication app(argc, argv);
    Licasa::initializeImagePlugins();
    RawPluginTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_raw_plugin.moc"
