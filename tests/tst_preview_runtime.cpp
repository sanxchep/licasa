#include "imaging/fast_tiff_preview.h"

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <tiffio.h>

#include <atomic>
#include <vector>

namespace {
quint16 grayValue(int x, int y) { return quint16((x * 307 + y * 103) & 65535); }

QColor rgbaValue(int x, int y, bool alpha)
{
    return QColor((x * 3 + y * 7) & 255, (x * 11 + y * 5) & 255, (x * 17 + y * 19) & 255,
                  alpha ? (x * 23 + y * 29) & 255 : 255);
}

bool writeTiff(const QString& path, const QSize& size, bool gray, bool white, bool alpha,
               bool compressed, bool bigTiff = false)
{
    TIFF* tiff = TIFFOpen(QFile::encodeName(path).constData(), bigTiff ? "w8" : "w");
    if (!tiff) {
        return false;
    }
    const int samples = gray ? 1 : alpha ? 4 : 3;
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, size.width());
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, size.height());
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, gray ? 16 : 8);
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, samples);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC,
                 gray ? (white ? PHOTOMETRIC_MINISWHITE : PHOTOMETRIC_MINISBLACK)
                      : PHOTOMETRIC_RGB);
    TIFFSetField(tiff, TIFFTAG_COMPRESSION,
                 compressed ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_NONE);
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, bigTiff ? 128 : 3);
    TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    if (alpha) {
        uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, &extra);
    }
    const QByteArray profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    TIFFSetField(tiff, TIFFTAG_ICCPROFILE, profile.size(), profile.constData());

    std::vector<quint16> storage((size_t(size.width()) * (gray ? 2 : samples) + 1) / 2);
    bool success = true;
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<uchar*>(storage.data());
        for (int x = 0; x < size.width(); ++x) {
            if (gray) {
                storage[size_t(x)] = grayValue(x, y);
            } else {
                const QColor value = rgbaValue(x, y, alpha);
                row[samples * x] = uchar(value.red());
                row[samples * x + 1] = uchar(value.green());
                row[samples * x + 2] = uchar(value.blue());
                if (alpha) {
                    row[samples * x + 3] = uchar(value.alpha());
                }
            }
        }
        if (TIFFWriteScanline(tiff, storage.data(), uint32_t(y)) != 1) {
            success = false;
            break;
        }
    }
    TIFFClose(tiff);
    return success;
}
} // namespace

class PreviewRuntimeTest final : public QObject {
    Q_OBJECT

  private slots:
    void largeUncompressedBigTiff()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("large.tiff"));
        const QSize source(10000, 2501);
        const QSize request(401, 101);
        QVERIFY(writeTiff(path, source, false, false, true, false, true));

        std::atomic_bool cancelled{false};
        const QImage image =
            Licasa::readFastTiffPreview(path, request, 2'000'000, 8 * 1024 * 1024, &cancelled);
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), source.scaled(request, Qt::KeepAspectRatio));
        QCOMPARE(image.format(), QImage::Format_RGBA8888);
        QCOMPARE(image.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
        for (int y = 0; y < image.height(); ++y) {
            const int sourceY = int(qint64(y) * source.height() / image.height());
            for (int x = 0; x < image.width(); ++x) {
                const int sourceX = int(qint64(x) * source.width() / image.width());
                QCOMPARE(image.pixelColor(x, y), rgbaValue(sourceX, sourceY, true));
            }
        }
        cancelled.store(true);
        QVERIFY(Licasa::readFastTiffPreview(path, request, 2'000'000, 8 * 1024 * 1024, &cancelled)
                    .isNull());
    }

    void sampledPixelsMatchSource_data()
    {
        QTest::addColumn<bool>("gray");
        QTest::addColumn<bool>("white");
        QTest::addColumn<bool>("alpha");
        QTest::addColumn<bool>("compressed");
        QTest::addColumn<QSize>("sourceSize");
        QTest::addColumn<QSize>("requestedSize");
        QTest::addColumn<qint64>("byteLimit");
        QTest::newRow("gray-black")
            << true << false << false << false << QSize(19, 13) << QSize(11, 9) << qint64(1024);
        QTest::newRow("gray-white")
            << true << true << false << false << QSize(19, 13) << QSize(11, 9) << qint64(1024);
        QTest::newRow("rgb") << false << false << false << false << QSize(19, 13) << QSize(11, 9)
                             << qint64(1024);
        QTest::newRow("rgba") << false << false << true << false << QSize(19, 13) << QSize(11, 9)
                              << qint64(1024);
        QTest::newRow("rgb-deflate")
            << false << false << false << true << QSize(19, 13) << QSize(11, 9) << qint64(1024);
        QTest::newRow("rgba-deflate")
            << false << false << true << true << QSize(19, 13) << QSize(11, 9) << qint64(1024);
        QTest::newRow("rgb-identity-width") << false << false << false << false << QSize(1601, 999)
                                            << QSize(1601, 999) << qint64(8 * 1024 * 1024);
        QTest::newRow("rgba-identity-width") << false << false << true << false << QSize(1601, 999)
                                             << QSize(1601, 999) << qint64(8 * 1024 * 1024);
        QTest::newRow("rgba-tight-budget")
            << false << false << true << true << QSize(19, 13) << QSize(11, 9) << qint64(37);
        QTest::newRow("wide-direct-mapping") << false << false << true << false << QSize(16385, 3)
                                             << QSize(16385, 3) << qint64(256 * 1024);
    }

    void sampledPixelsMatchSource()
    {
        QFETCH(bool, gray);
        QFETCH(bool, white);
        QFETCH(bool, alpha);
        QFETCH(bool, compressed);
        QFETCH(QSize, sourceSize);
        QFETCH(QSize, requestedSize);
        QFETCH(qint64, byteLimit);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sample.tif"));
        QVERIFY(writeTiff(path, sourceSize, gray, white, alpha, compressed));
        std::atomic_bool cancelled{false};
        const QImage image =
            Licasa::readFastTiffPreview(path, requestedSize, 2'000'000, byteLimit, &cancelled);
        QVERIFY(!image.isNull());
        QVERIFY(image.sizeInBytes() <= byteLimit);
        if (sourceSize == requestedSize) {
            QCOMPARE(image.size(), sourceSize);
        }
        QCOMPARE(image.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
        QCOMPARE(image.format(), gray    ? QImage::Format_Grayscale8
                                 : alpha ? QImage::Format_RGBA8888
                                         : QImage::Format_RGB888);
        for (int y = 0; y < image.height(); ++y) {
            const int sourceY =
                int(quint64(y) * quint64(sourceSize.height()) / quint64(image.height()));
            for (int x = 0; x < image.width(); ++x) {
                const int sourceX =
                    int(quint64(x) * quint64(sourceSize.width()) / quint64(image.width()));
                if (gray) {
                    const quint16 value = grayValue(sourceX, sourceY);
                    const int reduced = (white ? quint16(65535 - value) : value) >> 8;
                    QCOMPARE(image.pixelColor(x, y), QColor(reduced, reduced, reduced));
                } else {
                    QCOMPARE(image.pixelColor(x, y), rgbaValue(sourceX, sourceY, alpha));
                }
            }
        }
        cancelled.store(true);
        QVERIFY(Licasa::readFastTiffPreview(path, requestedSize, 1'000'000, byteLimit, &cancelled)
                    .isNull());
        QVERIFY(Licasa::readFastTiffPreview(path, requestedSize, 0, byteLimit, nullptr).isNull());
    }
};

QTEST_GUILESS_MAIN(PreviewRuntimeTest)
#include "tst_preview_runtime.moc"
