#include "app/application_settings.h"
#include "imaging/async_image_provider.h"
#include "imaging/image_resource_policy.h"
#include "png_preview_fixture.h"

#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QQuickTextureFactory>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>
#include <memory>

class PreviewHandoffTest final : public QObject {
    Q_OBJECT

  private slots:
    void xpmHandoffPreservesNativeAndFallbackPixels_data()
    {
        QTest::addColumn<bool>("named");
        QTest::newRow("fast-hex-palette") << false;
        QTest::newRow("qt-named-palette") << true;
    }

    void xpmHandoffPreservesNativeAndFallbackPixels()
    {
        QFETCH(bool, named);
        QTemporaryDir settings, images;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        preferences.setMaximumImageMegapixels(80);
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath("handoff.xpm");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray header = "/* XPM */\nstatic const char*x[]={\"5000 5000 1 1\",\"a c " +
                                  QByteArray(named ? "red" : "#123456") + "\",\n";
        QCOMPARE(file.write(header), header.size());
        const QByteArray row = "\"" + QByteArray(5000, 'a') + "\",\n";
        for (int y = 0; y < 5000; ++y) {
            QCOMPARE(file.write(row), row.size());
        }
        QCOMPARE(file.write("};\n"), 3);
        file.close();
        const QUrl url = QUrl::fromLocalFile(path);
        const QString base = "1/" + QString::fromLatin1(QUrl::toPercentEncoding(path));
        std::unique_ptr<QQuickImageResponse> preview(provider.requestImageResponse(
            base + "?licasa_stage=preview&cm=0&licasa_handoff=1", QSize(128, 128)));
        provider.waitForPendingWork();
        QVERIFY2(preview->errorString().isEmpty(), qPrintable(preview->errorString()));
        QVERIFY(provider.hasDecodedFullBase(url));
        const quint64 hits = provider.decodedBaseCacheHitCount();
        std::unique_ptr<QQuickImageResponse> full(
            provider.requestImageResponse(base + "?licasa_stage=full&cm=0", QSize()));
        provider.waitForPendingWork();
        QVERIFY2(full->errorString().isEmpty(), qPrintable(full->errorString()));
        QCOMPARE(provider.decodedBaseCacheHitCount() - hits, 1u);
        std::unique_ptr<QQuickTextureFactory> texture(full->textureFactory());
        QVERIFY(texture);
        const QImage actual = texture->image();
        QCOMPARE(actual.size(), QSize(5000, 5000));
        QImage expectedRow(5000, 1, QImage::Format_RGB32);
        expectedRow.fill(named ? QColor(Qt::red) : QColor("#123456"));
        expectedRow = expectedRow.convertToFormat(actual.format());
        for (int y = 0; y < 5000; ++y) {
            QVERIFY(std::memcmp(actual.constScanLine(y), expectedRow.constBits(), 5000 * 4) == 0);
        }
        provider.releasePictureResources();
        provider.waitForPendingWork();
        QVERIFY(!provider.hasDecodedFullBase(url));
    }

    void svgHandoffRespectsRasterAndByteBounds_data()
    {
        QTest::addColumn<int>("extent");
        QTest::addColumn<bool>("retained");
        QTest::newRow("below-large-base-threshold") << 4999 << false;
        QTest::newRow("large-base-threshold") << 5000 << true;
        QTest::newRow("one-use-large-base") << 8200 << true;
        QTest::newRow("above-full-raster-budget") << 14000 << false;
    }

    void svgHandoffRespectsRasterAndByteBounds()
    {
        QFETCH(int, extent);
        QFETCH(bool, retained);
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        preferences.setMaximumImageMegapixels(80);
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("handoff.svg"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray document = "<svg xmlns='http://www.w3.org/2000/svg' width='" +
                                    QByteArray::number(extent) + "' height='" +
                                    QByteArray::number(extent) +
                                    "'><rect width='100%' height='100%' fill='#1e508c'/></svg>";
        QCOMPARE(file.write(document), document.size());
        file.close();
        const QUrl url = QUrl::fromLocalFile(path);
        const QString base =
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path));
        std::unique_ptr<QQuickImageResponse> preview(provider.requestImageResponse(
            base + QStringLiteral("?licasa_stage=preview&cm=0&licasa_handoff=1"), QSize(128, 128)));
        provider.waitForPendingWork();
        QVERIFY2(preview->errorString().isEmpty(), qPrintable(preview->errorString()));
        QCOMPARE(provider.hasDecodedFullBase(url), retained);
        if (retained) {
            const quint64 hits = provider.decodedBaseCacheHitCount();
            std::unique_ptr<QQuickImageResponse> full(provider.requestImageResponse(
                base + QStringLiteral("?licasa_stage=full&cm=0"), QSize()));
            provider.waitForPendingWork();
            QVERIFY2(full->errorString().isEmpty(), qPrintable(full->errorString()));
            std::unique_ptr<QQuickTextureFactory> texture(full->textureFactory());
            QVERIFY(texture);
            const QImage actual = texture->image();
            QCOMPARE(actual.size(), QSize(extent, extent));
            // Compare against the native renderer, including its SVG percent
            // geometry convention and transparent regions, not an assumed fill.
            QImageReader reference(path);
            const QImage expected = reference.read().convertToFormat(actual.format());
            QCOMPARE(expected.size(), actual.size());
            const size_t bytes = size_t(extent) * actual.depth() / 8;
            for (int y = 0; y < extent; ++y) {
                QVERIFY(std::memcmp(actual.constScanLine(y), expected.constScanLine(y), bytes) ==
                        0);
            }
            QCOMPARE(provider.decodedBaseCacheHitCount() - hits, 1u);
            if (extent == 8200) {
                QVERIFY(!provider.hasDecodedFullBase(url));
            }
        }
        provider.releasePictureResources();
        provider.waitForPendingWork();
        QVERIFY(!provider.hasDecodedFullBase(url));
    }

    void oversizedPreviewBaseIsConsumedOnce_data()
    {
        QTest::addColumn<bool>("handoff");
        QTest::addColumn<bool>("invalidate");
        QTest::addColumn<bool>("release");
        QTest::newRow("preview-only") << false << false << false;
        QTest::newRow("full-handoff") << true << false << false;
        QTest::newRow("changed-source") << true << true << false;
        QTest::newRow("released-source") << true << false << true;
    }

    void oversizedPreviewBaseIsConsumedOnce()
    {
        QFETCH(bool, handoff);
        QFETCH(bool, invalidate);
        QFETCH(bool, release);
        QTemporaryDir settings;
        QTemporaryDir images;
        QVERIFY(settings.isValid());
        QVERIFY(images.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        preferences.setMaximumImageMegapixels(80);
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::AsyncImageProvider provider(policy);
        const QString path = images.filePath(QStringLiteral("handoff.png"));
        // Just beyond the persistent 256 MiB edit-cache boundary.
        QVERIFY(LicasaTest::writePlainLargePng(path, true, 8200));
        const QUrl url = QUrl::fromLocalFile(path);
        const QString base =
            QStringLiteral("1/") + QString::fromLatin1(QUrl::toPercentEncoding(path));
        const QString hint = handoff ? QStringLiteral("&licasa_handoff=1") : QString();
        std::unique_ptr<QQuickImageResponse> preview(provider.requestImageResponse(
            base + QStringLiteral("?licasa_stage=preview&cm=0") + hint, QSize(128, 128)));
        provider.waitForPendingWork();
        QVERIFY2(preview->errorString().isEmpty(), qPrintable(preview->errorString()));
        QCOMPARE(provider.hasDecodedFullBase(url), handoff);
        const quint64 hits = provider.decodedBaseCacheHitCount();
        if (release) {
            provider.releasePictureResources();
            provider.waitForPendingWork();
            QVERIFY(!provider.hasDecodedFullBase(url));
        }
        if (invalidate) {
            QImage replacement(32, 24, QImage::Format_ARGB32);
            replacement.fill(QColor(12, 34, 56, 78));
            QVERIFY(replacement.save(path));
            QVERIFY(!provider.hasDecodedFullBase(url));
        }
        std::unique_ptr<QQuickImageResponse> full(provider.requestImageResponse(
            base + QStringLiteral("?licasa_stage=full&cm=0"), QSize()));
        provider.waitForPendingWork();
        QVERIFY2(full->errorString().isEmpty(), qPrintable(full->errorString()));
        std::unique_ptr<QQuickTextureFactory> texture(full->textureFactory());
        QVERIFY(texture);
        const QImage actual = texture->image();
        QCOMPARE(actual.size(), invalidate ? QSize(32, 24) : QSize(8200, 8200));
        // Check every visible byte with a one-row oracle, including Qt's
        // premultiplied texture representation, without another large raster.
        QImage expectedRow(actual.width(), 1, QImage::Format_ARGB32);
        expectedRow.fill(invalidate ? QColor(12, 34, 56, 78) : QColor(30, 80, 140));
        expectedRow = expectedRow.convertToFormat(actual.format());
        const size_t rowBytes = size_t(actual.width()) * actual.depth() / 8;
        for (int y = 0; y < actual.height(); ++y) {
            if (!invalidate && y == 2500) {
                expectedRow.setPixelColor(2500, 0, QColor(210, 40, 90, 120));
            }
            QVERIFY2(std::memcmp(actual.constScanLine(y), expectedRow.constBits(), rowBytes) == 0,
                     qPrintable(QStringLiteral("Full raster differs at row %1").arg(y)));
        }
        QCOMPARE(provider.decodedBaseCacheHitCount() - hits,
                 handoff && !invalidate && !release ? 1u : 0u);
        if (!invalidate) {
            QVERIFY(!provider.hasDecodedFullBase(url));
        }
        provider.releasePictureResources();
        provider.waitForPendingWork();
        QVERIFY(!provider.hasDecodedFullBase(url));
    }
};

QTEST_MAIN(PreviewHandoffTest)
#include "tst_preview_handoff.moc"
