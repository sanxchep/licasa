#include "app/application_settings.h"
#include "export/image_export_contract.h"
#include "export/image_export_encoder.h"
#include "export/image_save_operations.h"
#include "export/image_save_service.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_resource_policy.h"
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#if defined(Q_OS_UNIX)
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#endif

namespace {
QImage fixture(bool alpha = false)
{
    QImage image(97, 61, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixel(x, y,
                           qRgba((x * 31 + y * 7) % 256, (x * 11 + y * 23) % 256,
                                 (x * 3 + y * 43) % 256, alpha && x % 5 != 4 ? x % 5 * 63 : 255));
        }
    }
    image.setColorSpace(QColorSpace::SRgb);
    image.setDotsPerMeterX(11811);
    image.setDotsPerMeterY(11811);
    return image;
}
QByteArray encode(const QImage& image, const QByteArray& format, QVariantMap settings = {})
{
    settings["format"] = format;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    const QString error =
        Licasa::ImageExport::encode(&buffer, image, format, Licasa::exportOptionsFromMap(settings));
    return error.isEmpty() ? bytes : QByteArray();
}
// Parse JPEG segments before SOS; a marker-like sequence inside entropy bytes
// must not satisfy a progressive/quality assertion.
QList<QByteArray> jpegSegments(const QByteArray& bytes, int wanted)
{
    QList<QByteArray> result;
    if (!bytes.startsWith(QByteArray::fromHex("ffd8"))) {
        return result;
    }
    int pos = 2;
    while (pos + 4 <= bytes.size()) {
        if (uchar(bytes[pos++]) != 0xff) {
            break;
        }
        while (pos < bytes.size() && uchar(bytes[pos]) == 0xff) {
            ++pos;
        }
        if (pos >= bytes.size()) {
            break;
        }
        const int marker = uchar(bytes[pos++]);
        if (marker == 0xda || marker == 0xd9) {
            break;
        }
        if (pos + 2 > bytes.size()) {
            break;
        }
        const int size = qFromBigEndian<quint16>(bytes.constData() + pos);
        if (size < 2 || pos + size > bytes.size()) {
            break;
        }
        if (marker == wanted) {
            result.append(bytes.mid(pos + 2, size - 2));
        }
        pos += size;
    }
    return result;
}
int tiffCompression(const QByteArray& bytes)
{
    if (bytes.size() < 8) {
        return -1;
    }
    const bool little = bytes.startsWith("II");
    auto u16 = [&](int offset) {
        return little ? qFromLittleEndian<quint16>(bytes.constData() + offset)
                      : qFromBigEndian<quint16>(bytes.constData() + offset);
    };
    const quint32 offset = little ? qFromLittleEndian<quint32>(bytes.constData() + 4)
                                  : qFromBigEndian<quint32>(bytes.constData() + 4);
    if (offset + 2 > quint32(bytes.size())) {
        return -1;
    }
    const int count = u16(int(offset));
    for (int i = 0; i < count; ++i) {
        const int entry = int(offset) + 2 + i * 12;
        if (entry + 12 > bytes.size()) {
            return -1;
        }
        if (u16(entry) == 259 && u16(entry + 2) == 3) {
            return u16(entry + 8);
        }
    }
    return -1;
}
QSet<QRgb> colors(const QImage& image)
{
    QSet<QRgb> values;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            values.insert(image.pixel(x, y));
        }
    }
    return values;
}
} // namespace

class EditorExportContracts final : public QObject {
    Q_OBJECT
  private slots:
    void pngWriteFailuresRemainErrors_data()
    {
        QTest::addColumn<int>("limit");
        for (int limit : {0, 1, 16, 64}) {
            QTest::newRow(qPrintable(QString::number(limit))) << limit;
        }
    }
    void pngWriteFailuresRemainErrors()
    {
        QFETCH(int, limit);
        class FailingOutput final : public QIODevice {
          public:
            explicit FailingOutput(int limit) : remaining(limit) { open(QIODevice::WriteOnly); }
            bool isSequential() const override { return true; }
            qint64 accepted = 0;

          protected:
            qint64 readData(char*, qint64) override { return -1; }
            qint64 writeData(const char*, qint64 size) override
            {
                const auto count = std::min<qint64>(size, remaining);
                remaining -= count;
                accepted += count;
                if (count != size) {
                    setErrorString(QStringLiteral("Injected output failure"));
                }
                return count > 0 ? count : -1;
            }

          private:
            qint64 remaining;
        } output(limit);
        const auto error = Licasa::ImageExport::encode(&output, fixture(), "png", {});
        QVERIFY2(error.contains("Injected output failure"), qPrintable(error));
        QVERIFY(output.accepted <= limit);
    }
    void sourceVersionProtectsStaleWindows()
    {
        QTemporaryDir dir;
        const auto source = QUrl::fromLocalFile(dir.filePath("source.png"));
        QVERIFY(fixture().save(source.toLocalFile()));
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        Licasa::ImageSaveService service(policy);
        const QString version = service.sourceVersion(source);
        QVERIFY(!version.isEmpty());
        QCOMPARE(service.sourceVersion(source), version);
        QSignalSpy completed(&service, &Licasa::ImageSaveService::saveCompleted);
        QSignalSpy failed(&service, &Licasa::ImageSaveService::saveFailed);
        QVERIFY(
            service.save(source, source, {{"exposure", .2}}, {{"format", "png"}}, this, version));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        QVERIFY(service.sourceVersion(source) != version);
        const auto destination = QUrl::fromLocalFile(dir.filePath("peer.png"));
        QVERIFY(!service.save(source, destination, {{"exposure", .7}}, {{"format", "png"}}, this,
                              version));
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.last().at(1).toString().contains("changed"));
        QVERIFY(!QFile::exists(destination.toLocalFile()));
        QVERIFY(service.save(source, destination, {}, {{"format", "png"}}, this,
                             service.sourceVersion(source)));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 10000);
        QCOMPARE(QImage(destination.toLocalFile()), QImage(source.toLocalFile()));
    }
    void atomicCommitFailureAndSourceReplacement_data()
    {
        QTest::addColumn<bool>("replaceSource");
        QTest::newRow("commit-denied") << false;
        QTest::newRow("source-replaced-after-encode") << true;
    }
    void atomicCommitFailureAndSourceReplacement()
    {
        QFETCH(bool, replaceSource);
        QTemporaryDir dir;
        const QString source = dir.filePath("source.png");
        const QString destination = dir.filePath("existing.png");
        QVERIFY(fixture().save(source));
        QFile existing(destination);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("destination must survive"), qint64(24));
        existing.close();
        auto read = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto original = read(source);
        const auto before = read(destination);
        const auto restore = qScopeGuard([&]() {
            QFile::setPermissions(dir.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        bool injected = false;
        const auto result = Licasa::ImageSaveOperations::writeEditedImage(
            source, destination, {}, Licasa::exportOptionsFromMap({{"format", "png"}}), 25, {},
            [&]() {
                if (replaceSource) {
                    QImage changed(17, 19, QImage::Format_RGB32);
                    changed.fill(Qt::green);
                    injected = changed.save(source);
                } else {
                    const bool changed =
                        QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner);
                    QFile probe(dir.filePath("permission-probe"));
                    injected = changed && !probe.open(QIODevice::WriteOnly);
                }
            });
        QVERIFY2(injected,
                 "Failure injection requires denied writes; run with desktop credentials.");
        QVERIFY(!result.errorMessage.isEmpty());
        QVERIFY(result.destinationUrl.isEmpty());
        QCOMPARE(read(destination), before);
        if (replaceSource) {
            QCOMPARE(QImage(source).size(), QSize(17, 19));
        } else {
            QCOMPARE(read(source), original);
        }
        QVERIFY(QFile::setPermissions(dir.path(),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto retry = Licasa::ImageSaveOperations::writeEditedImage(
            source, destination, {}, Licasa::exportOptionsFromMap({{"format", "png"}}), 25);
        QVERIFY2(retry.errorMessage.isEmpty(), qPrintable(retry.errorMessage));
        QCOMPARE(QImage(destination).size(), QImage(source).size());
        QCOMPARE(
            QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(),
            2);
    }
    void limitedWritePreservesExistingFiles()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("source.png");
        QImage large(257, 259, QImage::Format_RGB32);
        QRandomGenerator random(0x241029);
        for (int y = 0; y < large.height(); ++y) {
            for (int x = 0; x < large.width(); ++x) {
                large.setPixel(x, y, random.generate() | 0xff000000);
            }
        }
        QVERIFY(large.save(source));
        const QString destination = dir.filePath("existing.png");
        QVERIFY(QFile::copy(source, destination));
        QFile original(source);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto before = original.readAll();
        original.close();
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {"--limited-write", source, destination});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        const QByteArray childDiagnostics = child.readAllStandardError();
        QVERIFY2(child.exitCode() == 0, childDiagnostics.constData());
        for (const auto& path : {source, destination}) {
            QFile after(path);
            QVERIFY(after.open(QIODevice::ReadOnly));
            QCOMPARE(after.readAll(), before);
        }
        QCOMPARE(
            QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(),
            2);
    }
    void allFormatsFlattenMatte_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<bool>("black");
        for (const auto& value : Licasa::ImageExport::formats()) {
            const auto format = value.toMap().value("key").toByteArray();
            for (bool black : {false, true}) {
                QTest::newRow((format + (black ? "-black" : "-white")).constData())
                    << format << black;
            }
        }
    }
    void allFormatsFlattenMatte()
    {
        QFETCH(QByteArray, format);
        QFETCH(bool, black);
        QImage clear(64, 64, QImage::Format_ARGB32);
        clear.fill(Qt::transparent);
        const auto decoded = QImage::fromData(encode(clear, format,
                                                     {{"keepAlpha", false},
                                                      {"matte", black ? "#000000" : "#ffffff"},
                                                      {"iconSize", 64},
                                                      {"dither", false},
                                                      {"quality", 100}}));
        QVERIFY(!decoded.isNull());
        const QColor pixel = decoded.pixelColor(32, 32);
        QCOMPARE(pixel.alpha(), 255);
        const int expected = black ? 0 : 255;
        QVERIFY(qAbs(pixel.red() - expected) <= 2);
        QVERIFY(qAbs(pixel.green() - expected) <= 2);
        QVERIFY(qAbs(pixel.blue() - expected) <= 2);
    }
    void compressionPreservesPixels_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<int>("compression");
        for (int value : {0, 50, 100}) {
            QTest::newRow(qPrintable("png-" + QString::number(value)))
                << QByteArray("png") << value;
        }
        for (int value : {0, 50}) {
            QTest::newRow(qPrintable("tiff-" + QString::number(value)))
                << QByteArray("tiff") << value;
        }
    }
    void compressionPreservesPixels()
    {
        QFETCH(QByteArray, format);
        QFETCH(int, compression);
        const auto source = fixture(true);
        const auto bytes = encode(source, format, {{"compression", compression}});
        QVERIFY(!bytes.isEmpty());
        const auto decoded = QImage::fromData(bytes);
        if (format == "png") {
            QCOMPARE(decoded.convertToFormat(QImage::Format_RGBA8888),
                     source.convertToFormat(QImage::Format_RGBA8888));
        } else {
            QCOMPARE(tiffCompression(bytes), compression == 0 ? 1 : 5);
            const auto opposite = QImage::fromData(
                encode(source, format, {{"compression", compression == 0 ? 50 : 0}}));
            QCOMPARE(decoded, opposite);
            // TIFF's associated alpha can quantize hidden/partial RGB. Both
            // compression modes must preserve the same visible pixels/alpha.
            for (int y = 0; y < source.height(); ++y) {
                for (int x = 0; x < source.width(); ++x) {
                    QCOMPARE(decoded.pixelColor(x, y).alpha(), source.pixelColor(x, y).alpha());
                    if (source.pixelColor(x, y).alpha() == 255) {
                        QCOMPARE(decoded.pixelColor(x, y), source.pixelColor(x, y));
                    }
                }
            }
        }
    }
    void jpegOptionsHaveEncodedSemantics_data()
    {
        QTest::addColumn<int>("quality");
        QTest::addColumn<bool>("progressive");
        QTest::addColumn<bool>("optimized");
        for (int q : {1, 50, 100}) {
            for (bool p : {false, true}) {
                for (bool o : {false, true}) {
                    QTest::newRow(qPrintable(QString("q%1-p%2-o%3").arg(q).arg(p).arg(o)))
                        << q << p << o;
                }
            }
        }
    }
    void jpegOptionsHaveEncodedSemantics()
    {
        QFETCH(int, quality);
        QFETCH(bool, progressive);
        QFETCH(bool, optimized);
        const auto source = fixture();
        const QVariantMap settings{
            {"quality", quality}, {"progressive", progressive}, {"optimized", optimized}};
        const auto bytes = encode(source, "jpeg", settings);
        QCOMPARE(jpegSegments(bytes, progressive ? 0xc2 : 0xc0).size(), 1);
        QVERIFY(jpegSegments(bytes, progressive ? 0xc0 : 0xc2).isEmpty());
        const auto tables = jpegSegments(bytes, 0xdb);
        QVERIFY(!tables.isEmpty());
        QVERIFY(tables.first().size() >= 65);
        int sum = 0;
        for (int i = 1; i <= 64; ++i) {
            sum += uchar(tables.first()[i]);
        }
        if (quality == 100) {
            QCOMPARE(sum, 64);
        } else if (quality == 1) {
            QVERIFY(sum > 5000);
        } else {
            QVERIFY(sum > 64 && sum < 16000);
        }
        auto opposite = settings;
        opposite["optimized"] = !optimized;
        const auto other = encode(source, "jpeg", opposite);
        QCOMPARE(QImage::fromData(bytes), QImage::fromData(other));
        if (!progressive) {
            QVERIFY(jpegSegments(bytes, 0xc4) != jpegSegments(other, 0xc4));
        }
    }
    void paletteAndGrayModes_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<int>("palette");
        QTest::addColumn<bool>("dither");
        for (const auto& format : {QByteArray("png"), QByteArray("bmp"), QByteArray("xpm")}) {
            for (int palette : {8, 64, 216}) {
                for (bool dither : {false, true}) {
                    QTest::newRow(qPrintable(QString::fromLatin1(format) +
                                             QString("-%1-%2").arg(palette).arg(dither)))
                        << format << palette << dither;
                }
            }
        }
    }
    void paletteAndGrayModes()
    {
        QFETCH(QByteArray, format);
        QFETCH(int, palette);
        QFETCH(bool, dither);
        const auto source = fixture();
        const auto bytes = encode(
            source, format, {{"colorMode", 2}, {"paletteColors", palette}, {"dither", dither}});
        const auto decoded = QImage::fromData(bytes);
        QCOMPARE(decoded.size(), source.size());
        QVERIFY(colors(decoded).size() <= palette);
        const auto other = QImage::fromData(encode(
            source, format, {{"colorMode", 2}, {"paletteColors", palette}, {"dither", !dither}}));
        QVERIFY(decoded != other);
    }
    void grayscaleChannels_data()
    {
        QTest::addColumn<QByteArray>("format");
        for (const auto f : {"png", "jpeg", "tiff", "bmp", "pgm"}) {
            QTest::newRow(f) << QByteArray(f);
        }
    }
    void grayscaleChannels()
    {
        QFETCH(QByteArray, format);
        const auto source = fixture();
        const auto decoded = QImage::fromData(encode(source, format, {{"colorMode", 1}}));
        QCOMPARE(decoded.size(), source.size());
        for (int y = 0; y < decoded.height(); ++y) {
            for (int x = 0; x < decoded.width(); ++x) {
                const auto c = decoded.pixelColor(x, y);
                QCOMPARE(c.red(), c.green());
                QCOMPARE(c.red(), c.blue());
            }
        }
    }
    void alphaAndMatte_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<bool>("keep");
        QTest::addColumn<QString>("matte");
        for (const auto f : {"png", "tiff", "webp", "xpm"}) {
            for (bool keep : {false, true}) {
                for (const auto matte : {"#ffffff", "#000000"}) {
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(f).arg(keep).arg(matte)))
                        << QByteArray(f) << keep << QString(matte);
                }
            }
        }
    }
    void alphaAndMatte()
    {
        QFETCH(QByteArray, format);
        QFETCH(bool, keep);
        QFETCH(QString, matte);
        const auto source = fixture(true);
        const auto decoded = QImage::fromData(
            encode(source, format,
                   {{"keepAlpha", keep}, {"matte", matte}, {"lossless", true}, {"dither", false}}));
        QCOMPARE(decoded.size(), source.size());
        if (keep) {
            QCOMPARE(decoded.pixelColor(0, 0).alpha(), 0);
            if (format != "xpm") {
                QCOMPARE(decoded.pixelColor(2, 0).alpha(), 126);
            }
        } else {
            QCOMPARE(decoded.pixelColor(0, 0), QColor(matte));
            QVERIFY(!decoded.hasAlphaChannel() || decoded.pixelColor(2, 0).alpha() == 255);
        }
    }
    void webpLosslessAndLossy()
    {
        const auto source = fixture(true);
        const auto lossless = encode(source, "webp", {{"lossless", true}});
        const auto lossy = encode(source, "webp", {{"lossless", false}, {"quality", 99}});
        auto chunks = [](const QByteArray& bytes) {
            QList<QByteArray> result;
            for (int p = 12; p + 8 <= bytes.size();) {
                result.append(bytes.mid(p, 4));
                const quint32 n = qFromLittleEndian<quint32>(bytes.constData() + p + 4);
                if (n > quint32(bytes.size() - p - 8)) {
                    break;
                }
                p += 8 + int(n) + (n % 2);
            }
            return result;
        };
        QVERIFY(chunks(lossless).contains("VP8L"));
        QVERIFY(chunks(lossy).contains("VP8 "));
        const auto decoded = QImage::fromData(lossless);
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                if (source.pixelColor(x, y).alpha() > 0) {
                    QCOMPARE(decoded.pixelColor(x, y), source.pixelColor(x, y));
                }
            }
        }
    }
    void monochromeThresholdDitherAndInvert_data()
    {
        QTest::addColumn<QByteArray>("format");
        for (const auto f : {"pbm", "wbmp", "xbm"}) {
            QTest::newRow(f) << QByteArray(f);
        }
    }
    void monochromeThresholdDitherAndInvert()
    {
        QFETCH(QByteArray, format);
        QImage ramp(17, 5, QImage::Format_RGB32);
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 17; ++x) {
                ramp.setPixelColor(x, y, QColor(x * 15, x * 15, x * 15));
            }
        }
        for (int threshold : {0, 128, 255}) {
            const auto normal = QImage::fromData(
                encode(ramp, format, {{"threshold", threshold}, {"dither", false}}));
            const auto inverse = QImage::fromData(encode(
                ramp, format, {{"threshold", threshold}, {"dither", false}, {"invert", true}}));
            QCOMPARE(normal.size(), ramp.size());
            for (int y = 0; y < 5; ++y) {
                for (int x = 0; x < 17; ++x) {
                    QCOMPARE(normal.pixelColor(x, y).red(), x * 15 < threshold ? 0 : 255);
                    QCOMPARE(inverse.pixelColor(x, y).red(), 255 - normal.pixelColor(x, y).red());
                }
            }
        }
        ramp.fill(QColor(120, 120, 120));
        const auto plain = QImage::fromData(encode(ramp, format, {{"dither", false}}));
        const auto dots = QImage::fromData(encode(ramp, format, {{"dither", true}}));
        QCOMPARE(colors(plain).size(), 1);
        QCOMPARE(colors(dots).size(), 2);
    }
    void portableTextAndBinary_data()
    {
        QTest::addColumn<QByteArray>("format");
        for (const auto f : {"pbm", "pgm", "ppm"}) {
            QTest::newRow(f) << QByteArray(f);
        }
    }
    void portableTextAndBinary()
    {
        QFETCH(QByteArray, format);
        const auto source = fixture();
        const auto text = encode(source, format, {{"ascii", true}});
        const auto binary = encode(source, format, {{"ascii", false}});
        QCOMPARE(text.left(2), format == "pbm"   ? QByteArray("P1")
                               : format == "pgm" ? QByteArray("P2")
                                                 : QByteArray("P3"));
        QCOMPARE(binary.left(2), format == "pbm"   ? QByteArray("P4")
                                 : format == "pgm" ? QByteArray("P5")
                                                   : QByteArray("P6"));
        QCOMPARE(QImage::fromData(text), QImage::fromData(binary));
    }
    void iconFitFillAndHotspot_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<bool>("crop");
        QTest::addColumn<bool>("alpha");
        for (const auto f : {"ico", "cur", "icns"}) {
            for (bool crop : {false, true}) {
                for (bool alpha : {false, true}) {
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(f).arg(crop).arg(alpha)))
                        << QByteArray(f) << crop << alpha;
                }
            }
        }
    }
    void iconFitFillAndHotspot()
    {
        QFETCH(QByteArray, format);
        QFETCH(bool, crop);
        QFETCH(bool, alpha);
        QImage source(80, 40, QImage::Format_ARGB32);
        source.fill(Qt::red);
        const auto bytes = encode(source, format,
                                  {{"iconSize", 64},
                                   {"iconCrop", crop},
                                   {"keepAlpha", alpha},
                                   {"hotspotX", 63},
                                   {"hotspotY", 62}});
        const auto decoded = QImage::fromData(bytes);
        QCOMPARE(decoded.size(), QSize(64, 64));
        QCOMPARE(decoded.pixelColor(32, 32), QColor(Qt::red));
        if (crop) {
            QCOMPARE(decoded.pixelColor(32, 0), QColor(Qt::red));
        } else if (alpha) {
            QCOMPARE(decoded.pixelColor(32, 0).alpha(), 0);
        } else {
            QCOMPARE(decoded.pixelColor(32, 0), QColor(Qt::white));
        }
        if (format == "cur") {
            QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 2), quint16(2));
            QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 10), quint16(63));
            QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 12), quint16(62));
        }
    }
    void densityAndProfile_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<int>("dpi");
        QTest::addColumn<bool>("srgb");
        for (const auto f : {"png", "jpeg", "tiff", "bmp"}) {
            for (int dpi : {0, 1, 300, 2400}) {
                for (bool srgb : {false, true}) {
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(f).arg(dpi).arg(srgb)))
                        << QByteArray(f) << dpi << srgb;
                }
            }
        }
    }
    void densityAndProfile()
    {
        QFETCH(QByteArray, format);
        QFETCH(int, dpi);
        QFETCH(bool, srgb);
        auto source = fixture();
        source.setColorSpace(QColorSpace::DisplayP3);
        const auto decoded =
            QImage::fromData(encode(source, format, {{"dpi", dpi}, {"srgb", srgb}}));
        QVERIFY(!decoded.isNull());
        const int expected = dpi == 0 ? 11811 : qRound(dpi / .0254);
        QVERIFY(qAbs(decoded.dotsPerMeterX() - expected) <= 1);
        QVERIFY(qAbs(decoded.dotsPerMeterY() - expected) <= 1);
        if (format != "bmp") {
            QCOMPARE(decoded.colorSpace(),
                     QColorSpace(srgb ? QColorSpace::SRgb : QColorSpace::DisplayP3));
        }
    }
    void xpmTransparencyBoundary()
    {
        const auto source = fixture(true);
        for (int threshold : {0, 128, 255}) {
            const auto decoded =
                QImage::fromData(encode(source, "xpm", {{"threshold", threshold}}));
            for (int x = 0; x < 5; ++x) {
                QCOMPARE(decoded.pixelColor(x, 0).alpha(),
                         source.pixelColor(x, 0).alpha() < threshold ? 0 : 255);
            }
        }
    }
    void scalingMethodAndSoftenAreVisible()
    {
        QTemporaryDir dir;
        QImage checks(64, 64, QImage::Format_ARGB32);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                checks.setPixelColor(x, y, (x + y) % 2 ? Qt::black : Qt::white);
            }
        }
        checks.setColorSpace(QColorSpace::SRgb);
        auto soft = checks;
        Licasa::ImageEditParameters edits;
        edits.soften = .35;
        QVERIFY(Licasa::applyImageEdits(soft, edits));
        QCOMPARE(soft.size(), checks.size());
        QVERIFY(soft != checks);
        QVERIFY(colors(soft).size() > 2);
        auto neutral = checks;
        QVERIFY(Licasa::applyImageEdits(neutral, {}));
        QCOMPARE(neutral, checks);
        const QString path = dir.filePath("source.png");
        QVERIFY(checks.save(path));
        QList<QImage> outputs;
        for (bool nearest : {false, true}) {
            const auto result = Licasa::ImageSaveOperations::writeEditedImage(
                path, dir.filePath(nearest ? "nearest.png" : "smooth.png"), {},
                Licasa::exportOptionsFromMap(
                    {{"format", "png"}, {"scale", 2.0}, {"nearest", nearest}}),
                25);
            QVERIFY2(result.errorMessage.isEmpty(), qPrintable(result.errorMessage));
            outputs.append(QImage(result.destinationUrl.toLocalFile()));
            QCOMPARE(outputs.last().size(), QSize(128, 128));
        }
        QCOMPARE(colors(outputs[1]).size(), 2);
        QVERIFY(colors(outputs[0]).size() > 2);
    }
};

int main(int argc, char** argv)
{
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    QGuiApplication app(argc, argv);
    Licasa::initializeImagePlugins();
    if (app.arguments().contains("--limited-write")) {
#if defined(Q_OS_UNIX)
        std::signal(SIGXFSZ, SIG_IGN);
        const struct rlimit limit {
            4096, 4096
        };
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            return 2;
        }
        const auto args = app.arguments();
        const auto result = Licasa::ImageSaveOperations::writeEditedImage(
            args.at(2), args.at(3), {},
            Licasa::exportOptionsFromMap({{"format", "png"}, {"compression", 0}}), 25);
        return result.errorMessage.isEmpty() ? 3 : 0;
#else
        return 4;
#endif
    }
    EditorExportContracts tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_editor_export_contracts.moc"
