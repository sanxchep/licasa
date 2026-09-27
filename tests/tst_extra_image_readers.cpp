#include "../src/imaging/image_decode_contract.h"

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
