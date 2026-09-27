#include "imaging/progressive_jpeg_preview.h"

#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cstdlib>

namespace {
QString fixture(const char* name)
{
    return QStringLiteral(LICASA_SOURCE_DIR "/tests/test-assets/formats/additional-probes/") +
           QString::fromLatin1(name);
}
} // namespace

class ProgressiveJpegPreviewTest final : public QObject {
    Q_OBJECT

  private slots:
    void earlyPassProvidesUsableColorPreview()
    {
        std::atomic_bool cancelled = false;
        const QSize size(320, 180);
        const QImage early = Licasa::readEarlyProgressiveJpegPreview(fixture("progressive.jpeg"),
                                                                     size, 1000 * 1000, &cancelled);
        QCOMPARE(early.size(), size);
        QVERIFY(early.colorSpace().isValid());

        QImageReader referenceReader(fixture("progressive.jpeg"));
        referenceReader.setScaledSize(size);
        const QImage reference = referenceReader.read();
        QVERIFY2(!reference.isNull(), qPrintable(referenceReader.errorString()));
        quint64 difference = 0;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const QRgb actual = early.pixel(x, y);
                const QRgb expected = reference.pixel(x, y);
                difference += quint64(std::abs(qRed(actual) - qRed(expected))) +
                              quint64(std::abs(qGreen(actual) - qGreen(expected))) +
                              quint64(std::abs(qBlue(actual) - qBlue(expected)));
            }
        }
        QVERIFY2(difference / (size.width() * size.height() * 3) < 30,
                 "The early scan is too far from the completed image to use as a preview");
    }

    void ordinaryAndIneligibleFilesUseQt()
    {
        std::atomic_bool cancelled = false;
        const QSize size(320, 180);
        QVERIFY(Licasa::readEarlyProgressiveJpegPreview(fixture("baseline-extension.jpeg"), size,
                                                        1000 * 1000, &cancelled)
                    .isNull());
        QVERIFY(Licasa::readEarlyProgressiveJpegPreview(fixture("progressive.jpeg"), size, 1,
                                                        &cancelled)
                    .isNull());
        cancelled = true;
        QVERIFY(Licasa::readEarlyProgressiveJpegPreview(fixture("progressive.jpeg"), size,
                                                        1000 * 1000, &cancelled)
                    .isNull());
    }

    void profiledAndTruncatedFilesUseQt()
    {
        QFile original(fixture("progressive.jpeg"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray bytes = original.readAll();
        QVERIFY(bytes.startsWith(QByteArray::fromHex("ffd8")));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto writeSample = [&temporary](const QString& name, const QByteArray& data) {
            const QString path = temporary.filePath(name);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
                return QString();
            }
            return path;
        };
        QByteArray profiled = bytes;
        profiled.insert(2, QByteArray::fromHex("ffe2000e") + QByteArray("ICC_PROFILE\0", 12));
        const QString profiledPath = writeSample(QStringLiteral("profiled.jpg"), profiled);
        const QString truncatedPath =
            writeSample(QStringLiteral("truncated.jpg"), bytes.left(bytes.size() / 4));
        QVERIFY(!profiledPath.isEmpty());
        QVERIFY(!truncatedPath.isEmpty());
        std::atomic_bool cancelled = false;
        for (const QString& path : {profiledPath, truncatedPath}) {
            QVERIFY(Licasa::readEarlyProgressiveJpegPreview(path, QSize(320, 180), 1000 * 1000,
                                                            &cancelled)
                        .isNull());
        }
    }
};

QTEST_MAIN(ProgressiveJpegPreviewTest)
#include "tst_progressive_jpeg_preview.moc"
