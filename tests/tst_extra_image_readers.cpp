#include "../src/imaging/image_decode_contract.h"
#include "../src/imaging/parallel_smooth_scale.h"
#include "../src/imaging/planar_rgb_pack.h"
#include <limits>

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QTemporaryFile>
#include <QTest>
#include <QtEndian>

namespace {
QString fixture(const char* name)
{
    return QStringLiteral(LICASA_SOURCE_DIR "/tests/test-assets/modern/format-readers/") +
           QString::fromLatin1(name);
}
} // namespace

class ExtraImageReadersTest final : public QObject {
    Q_OBJECT

  private slots:
    void rgbPackingPreservesSaturationAndOddWidth()
    {
        const int32_t red[] = {
            std::numeric_limits<int32_t>::min(), -1, 0, 1, 127, 128, 254, 255, 256,
            std::numeric_limits<int32_t>::max(), 42};
        const int32_t green[] = {255, 254, 128, 127, 1, 0, -1, 256, 42, 12, 90};
        const int32_t blue[] = {12, 34, 56, 78, 90, 123, 145, 167, 189, 210, 255};
        QRgb actual[11];
        Licasa::packRgb8Row(red, green, blue, actual, 11);
        for (int i = 0; i < 11; ++i) {
            QCOMPARE(
                QColor::fromRgb(actual[i]),
                QColor(qBound(0, red[i], 255), qBound(0, green[i], 255), qBound(0, blue[i], 255)));
        }
    }

    void parallelNativePreviewPreservesColorAndTransparency_data()
    {
        QTest::addColumn<QImage::Format>("format");
        QTest::newRow("rgb32") << QImage::Format_RGB32;
        QTest::newRow("argb32") << QImage::Format_ARGB32;
        QTest::newRow("premultiplied") << QImage::Format_ARGB32_Premultiplied;
    }

    void parallelNativePreviewPreservesColorAndTransparency()
    {
        QFETCH(QImage::Format, format);
        QImage source(4096, 4096, QImage::Format_ARGB32);
        QVERIFY(!source.isNull());
        for (int y = 0; y < source.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                row[x] = qRgba(x % 256, y % 256, (x + y) % 256, (x / 7 + y / 9) % 256);
            }
        }
        source = source.convertToFormat(format);
        source.setColorSpace(QColorSpace::SRgb);
        const QSize target(512, 512);
        const QImage expected =
            source.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage actual = Licasa::parallelSmoothScale(source, target);
        QCOMPARE(actual.size(), expected.size());
        QCOMPARE(actual.format(), expected.format());
        QCOMPARE(actual.colorSpace(), source.colorSpace());
        for (int y = 0; y < target.height(); ++y) {
            for (int x = 0; x < target.width(); ++x) {
                const QRgb a = actual.pixel(x, y), b = expected.pixel(x, y);
                QVERIFY(qAbs(qRed(a) - qRed(b)) <= 1);
                QVERIFY(qAbs(qGreen(a) - qGreen(b)) <= 1);
                QVERIFY(qAbs(qBlue(a) - qBlue(b)) <= 1);
                QVERIFY(qAbs(qAlpha(a) - qAlpha(b)) <= 1);
            }
        }
    }

    void oversizedJp2SourceIsRejectedBeforeDecode()
    {
#ifdef LICASA_TEST_JP2
        QFile original(fixture("jp2-check.jp2"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QByteArray bytes = original.readAll();
        const QByteArray normal = bytes;
        const qsizetype siz = bytes.indexOf(QByteArray::fromHex("ff4fff51"));
        const qsizetype ihdr = bytes.indexOf("ihdr");
        QVERIFY(siz >= 0 && siz + 42 <= bytes.size());
        QVERIFY(ihdr >= 0 && ihdr + 12 <= bytes.size());
        qToBigEndian<quint32>(4300, reinterpret_cast<uchar*>(bytes.data() + ihdr + 4));
        qToBigEndian<quint32>(7600, reinterpret_cast<uchar*>(bytes.data() + ihdr + 8));
        qToBigEndian<quint32>(7600, reinterpret_cast<uchar*>(bytes.data() + siz + 8));
        qToBigEndian<quint32>(4300, reinterpret_cast<uchar*>(bytes.data() + siz + 12));

        QTemporaryFile hostile(QDir::tempPath() + "/licasa-jp2-XXXXXX.jp2");
        QVERIFY(hostile.open());
        QCOMPARE(hostile.write(bytes), qint64(bytes.size()));
        QVERIFY(hostile.flush());

        QImageReader reader(hostile.fileName());
        reader.setScaledSize(QSize(1200, 678));
        Licasa::ImageDecodeContract::configure(reader, 25'000'000);
        QCOMPARE(reader.size(), QSize(7600, 4300));
        QVERIFY(reader.read().isNull());

        QTemporaryFile changed(QDir::tempPath() + "/licasa-jp2-XXXXXX.jp2");
        QVERIFY(changed.open());
        QCOMPARE(changed.write(normal), qint64(normal.size()));
        QVERIFY(changed.flush());
        QImageReader cached(changed.fileName());
        cached.setScaledSize(QSize(1200, 678));
        Licasa::ImageDecodeContract::configure(cached, 25'000'000);
        QCOMPARE(cached.size(), QSize(7, 5));
        QVERIFY(changed.seek(0));
        QCOMPARE(changed.write(bytes), qint64(bytes.size()));
        QVERIFY(changed.flush());
        QVERIFY(cached.read().isNull());
#endif
    }

    void jp2ReducedDecodeHonorsItsAllocationCap()
    {
#ifdef LICASA_TEST_JP2
        QImageReader reader(fixture("jp2-check.jp2"));
        Licasa::ImageDecodeContract::configure(reader, 1000);
        reader.setScaledSize(QSize(3, 2));
        reader.device()->setProperty(Licasa::ImageDecodeContract::scaledDecodePixelBudgetProperty,
                                     QVariant::fromValue(quint64(1)));
        QVERIFY(reader.read().isNull());
#endif
    }

    void registrationAndExactPixels()
    {
        QImageReader referenceReader(fixture("qoi-check.ppm"));
        const QImage reference = referenceReader.read();
        QVERIFY2(!reference.isNull(), qPrintable(referenceReader.errorString()));
        QCOMPARE(reference.size(), QSize(7, 5));

        struct ReaderCase {
            const char* extension;
            const char* filename;
        };
        const ReaderCase cases[] = {
#ifdef LICASA_TEST_QOI
            {"qoi", "qoi-check.qoi"},
#endif
#ifdef LICASA_TEST_JP2
            {"jp2", "jp2-check.jp2"},
#endif
        };
        for (const auto& readerCase : cases) {
            const QByteArray extension(readerCase.extension);
            QVERIFY(QImageReader::supportedImageFormats().contains(extension));
            QImageReader reader(fixture(readerCase.filename));
            QCOMPARE(reader.size(), reference.size());
            const QImage decoded = reader.read();
            QVERIFY2(!decoded.isNull(), qPrintable(reader.errorString()));
            for (int y = 0; y < reference.height(); ++y) {
                for (int x = 0; x < reference.width(); ++x) {
                    QCOMPARE(decoded.pixel(x, y), reference.pixel(x, y));
                }
            }

            QImageReader scaled(fixture(readerCase.filename));
            scaled.setScaledSize(QSize(3, 2));
            const QImage scaledImage = scaled.read();
            QVERIFY2(!scaledImage.isNull(), qPrintable(QString::fromLatin1(readerCase.filename) +
                                                       ": " + scaled.errorString()));
            QCOMPARE(scaledImage.size(), QSize(3, 2));
        }
    }
};

QTEST_MAIN(ExtraImageReadersTest)
#include "tst_extra_image_readers.moc"
