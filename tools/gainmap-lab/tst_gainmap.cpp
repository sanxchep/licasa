#include "jpeg_gainmap.h"
#include "test_gainmap_fixture.h"

#include "media/photo_asset_probe.h"

#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
#include <QtCore/qfloat16.h>
#include <QtEndian>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace Lab = Licasa::GainmapLab;

namespace {
constexpr int baseBytes = 2329;
constexpr int totalBytes = 3289;
constexpr int combinedPixels = 32 * 32 + 8 * 8;
constexpr auto iso = "urn:iso:std:iso:ts:21496:-1";
constexpr auto xmp = "http://ns.adobe.com/xap/1.0/";

QByteArray fixture(const QString& path = QStringLiteral(GAINMAP_FIXTURE))
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

float linear192() { return float(std::pow((192.0 / 255.0 + 0.055) / 1.055, 2.4)); }

bool checkGray(const QImage& image, float gain)
{
    std::array<qfloat16, 4> sample;
    std::memcpy(sample.data(), image.constScanLine(16) + 16 * sizeof(sample), sizeof(sample));
    const float expected = gain * linear192();
    return std::abs(float(sample[0]) - expected) < 0.012f &&
           std::abs(float(sample[1]) - expected) < 0.012f &&
           std::abs(float(sample[2]) - expected) < 0.012f && float(sample[3]) == 1.0f;
}

QByteArray exifOrientation(int value)
{
    QByteArray app1 = QByteArray::fromHex(
        "ffe1002245786966000049492a0008000000010012010300010000000100000000000000");
    qToLittleEndian<quint16>(quint16(value), app1.data() + 28);
    return app1;
}

QPoint storagePoint(int orientation, int x, int y)
{
    constexpr int w = 31;
    constexpr int h = 15;
    switch (orientation) {
    case 1:
        return {x, y};
    case 2:
        return {w - x, y};
    case 3:
        return {w - x, h - y};
    case 4:
        return {x, h - y};
    case 5:
        return {y, x};
    case 6:
        return {y, h - x};
    case 7:
        return {w - y, h - x};
    case 8:
        return {w - y, x};
    }
    return {};
}

void littleEndianMpf(char* tiff)
{
    // Translate only the known fixture's three IFD tags and two MP entries.
    const auto convert16 = [tiff](int offset) {
        qToLittleEndian(qFromBigEndian<quint16>(tiff + offset), tiff + offset);
    };
    const auto convert32 = [tiff](int offset) {
        qToLittleEndian(qFromBigEndian<quint32>(tiff + offset), tiff + offset);
    };
    tiff[0] = tiff[1] = 'I';
    convert16(2);
    convert32(4);
    convert16(8);
    for (int offset : {10, 22, 34}) {
        convert16(offset);
        convert16(offset + 2);
        convert32(offset + 4);
        if (offset != 10) {
            convert32(offset + 8);
        }
    }
    convert32(46);
    for (int offset : {50, 66}) {
        convert32(offset);
        convert32(offset + 4);
        convert32(offset + 8);
        convert16(offset + 12);
        convert16(offset + 14);
    }
}
} // namespace

class GainmapTest final : public QObject {
    Q_OBJECT
  private slots:
    void compoundCameraAlias()
    {
        const QString path = qEnvironmentVariable("LICASA_HDR_COMPOUND_CAMERA_FIXTURE");
        if (path.isEmpty()) {
            QSKIP("The retained Pixel 9 Pro XL fixture is optional.");
        }
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        const auto asset = Licasa::probePhotoAsset(QUrl::fromLocalFile(path));
        QVERIFY(asset.motion && asset.motion->hasEmbeddedVideo());
        QCOMPARE(asset.motion->embedded.offset, quint64(1754545));
        QCOMPARE(asset.motion->embedded.length, quint64(3022367));

        Lab::Limits limits;
        limits.rasterPixels = 16 * 1024 * 1024;
        QString error;
        const auto hdr = Lab::decode(path, *identity, 4.f, error, limits);
        QVERIFY2(hdr, qPrintable(error));
        QCOMPARE(hdr->linear.size(), QSize(2268, 4032));
        QCOMPARE(hdr->gainSize, QSize(566, 1008));
        QCOMPARE(quint64(hdr->jpegBytes), asset.motion->embedded.offset);
        QCOMPARE(QImage(path).size(), hdr->linear.size());
    }

    void reconstruction_data()
    {
        QTest::addColumn<QString>("carrier");
        QTest::addColumn<float>("headroom");
        QTest::addColumn<float>("expectedGain");
        QTest::newRow("dual-sdr") << QStringLiteral("dual") << 1.f << 1.f;
        QTest::newRow("dual-intermediate") << QStringLiteral("dual") << 2.f << 2.f;
        QTest::newRow("dual-full") << QStringLiteral("dual") << 4.f << 4.f;
        QTest::newRow("dual-above-capacity") << QStringLiteral("dual") << 8.f << 4.f;
        QTest::newRow("iso-only") << QStringLiteral("iso") << 4.f << 4.f;
        QTest::newRow("xmp-only") << QStringLiteral("xmp") << 4.f << 4.f;
        QTest::newRow("iso-precedes-conflicting-xmp") << QStringLiteral("conflict") << 4.f << 4.f;
        QTest::newRow("little-endian-mpf") << QStringLiteral("little") << 4.f << 4.f;
        QTest::newRow("motion-trailer-excluded") << QStringLiteral("trailer") << 4.f << 4.f;
    }

    void reconstruction()
    {
        QFETCH(QString, carrier);
        QFETCH(float, headroom);
        QFETCH(float, expectedGain);
        auto bytes = fixture();
        QCOMPARE(bytes.size(), totalBytes);
        if (carrier == "iso") {
            QVERIFY(bytes.contains(xmp));
            bytes.replace(xmp, "http://ns.adobx.com/xap/1.0/");
        } else if (carrier == "xmp") {
            QVERIFY(bytes.contains(iso));
            bytes.replace("urn:iso:", "urn:xyz:");
        } else if (carrier == "conflict") {
            QVERIFY(bytes.contains("hdrgm:GainMapMax=\"2\""));
            bytes.replace("hdrgm:GainMapMax=\"2\"", "hdrgm:GainMapMax=\"3\"");
        } else if (carrier == "little") {
            const auto offset = bytes.indexOf(QByteArray("MPF\0", 4));
            QVERIFY(offset >= 0);
            littleEndianMpf(bytes.data() + offset + 4);
        } else if (carrier == "trailer") {
            bytes.append(QByteArray::fromHex("00000018667479706d703432000000006d70343269736f6d"));
            bytes.append(fixture()); // JPEG signatures in the trailer must be ignored.
        }
        QVERIFY(carrier == "trailer" || bytes.size() == totalBytes);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("image.jpg");
        QVERIFY(writeFile(path, bytes));
        const QImage sdr(path);
        QCOMPARE(sdr.size(), QSize(32, 32));
        QCOMPARE(sdr.pixelColor(16, 16), QColor(192, 192, 192));
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        QString error;
        const auto result = Lab::decode(path, *identity, headroom, error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(result->gainSize, QSize(8, 8));
        QCOMPARE(result->jpegBytes, totalBytes);
        QCOMPARE(result->linear.size(), sdr.size());
        QCOMPARE(result->linear.format(), QImage::Format_RGBA16FPx4);
        QCOMPARE(result->linear.colorSpace(), QColorSpace(QColorSpace::SRgbLinear));
        QVERIFY(checkGray(result->linear, expectedGain));
        QCOMPARE(QImage(path), sdr);
    }

    void malformedCarrier_data()
    {
        QTest::addColumn<QString>("mutation");
        for (const char* mutation :
             {"ordinary", "no-mpf", "third-entry", "overlap", "oversized", "wrong-eoi",
              "hidden-jpeg", "broken-xmp", "nonfinite-xmp", "bad-iso-denominator"}) {
            QTest::newRow(mutation) << QString::fromLatin1(mutation);
        }
    }

    void malformedCarrier()
    {
        QFETCH(QString, mutation);
        auto bytes = fixture();
        const auto offset = bytes.indexOf(QByteArray("MPF\0", 4));
        QVERIFY(offset >= 0);
        char* tiff = bytes.data() + offset + 4;
        if (mutation == "ordinary") {
            bytes.truncate(baseBytes);
            bytes[offset] = 'X';
        } else if (mutation == "no-mpf") {
            bytes[offset] = 'X';
        } else if (mutation == "third-entry") {
            qToBigEndian<quint32>(3, tiff + 30);
        } else if (mutation == "overlap") {
            qToBigEndian<quint32>(115, tiff + 74);
        } else if (mutation == "oversized") {
            qToBigEndian<quint32>(std::numeric_limits<quint32>::max(), tiff + 70);
        } else if (mutation == "wrong-eoi") {
            bytes[bytes.size() - 1] = '\0';
        } else if (mutation == "hidden-jpeg") {
            const QByteArray extra = bytes.mid(baseBytes);
            qToBigEndian<quint32>(quint32(2 * extra.size()), tiff + 70);
            bytes.append(extra);
        } else if (mutation == "broken-xmp" || mutation == "nonfinite-xmp") {
            bytes.replace("urn:iso:", "urn:xyz:");
            QVERIFY(bytes.contains("      hdrgm:Gamma=\"1\""));
            if (mutation == "broken-xmp") {
                bytes.replace("hdrgm:Gamma=\"1\"", "hdrgm:Gamma=\"0\"");
            } else {
                // Shorten indentation to keep JPEG and MPF lengths intact.
                bytes.replace("      hdrgm:Gamma=\"1\"", "    hdrgm:Gamma=\"NaN\"");
            }
        } else if (mutation == "bad-iso-denominator") {
            // This fixture uses the common-denominator form. Leave valid
            // XMP present to exercise rejection of a broken preferred packet.
            const auto packet = bytes.lastIndexOf(iso) + qsizetype(std::strlen(iso)) + 1;
            QVERIFY(packet > baseBytes && packet + 9 <= bytes.size());
            qToBigEndian<quint32>(0, bytes.data() + packet + 5);
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("image.jpg");
        QVERIFY(writeFile(path, bytes));
        const QImage sdr(path);
        QCOMPARE(sdr.size(), QSize(32, 32));
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        QString error;
        QVERIFY(!Lab::decode(path, *identity, 4, error));
        QVERIFY(!error.isEmpty());
        if (mutation == "hidden-jpeg") {
            QVERIFY2(error.contains("ranges disagree"), qPrintable(error));
        }
        QCOMPARE(QImage(path), sdr);
    }

    void stalePrimarySize_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("matching-directory") << QStringLiteral("matching") << true;
        QTest::newRow("matching-directory-with-motion-item") << QStringLiteral("motion") << true;
        QTest::newRow("truncated-video-keeps-gain-map")
            << QStringLiteral("truncated-video") << true;
        QTest::newRow("broken-gain-map-keeps-video") << QStringLiteral("broken-gain") << false;
        QTest::newRow("missing-standard-xmp") << QStringLiteral("missing") << false;
        QTest::newRow("different-gain-length") << QStringLiteral("different-length") << false;
        QTest::newRow("foreign-item-namespace") << QStringLiteral("foreign-namespace") << false;
        QTest::newRow("duplicate-standard-xmp") << QStringLiteral("duplicate-xmp") << false;
        QTest::newRow("doctype") << QStringLiteral("doctype") << false;
        QTest::newRow("nonzero-primary-padding") << QStringLiteral("padding") << false;
        QTest::newRow("broken-boundary") << QStringLiteral("boundary") << false;
        QTest::newRow("oversized-discrepancy") << QStringLiteral("excess-gap") << false;
    }

    void stalePrimarySize()
    {
        QFETCH(QString, variant);
        QFETCH(bool, accepted);
        auto bytes = fixture();
        const auto signature = bytes.indexOf(xmp);
        QVERIFY(signature >= 4);
        const qsizetype packetStart = signature + qsizetype(std::strlen(xmp)) + 1;
        const int length = qFromBigEndian<quint16>(bytes.constData() + signature - 2);
        const auto packetBytes = signature - 2 + length - packetStart;
        QByteArray packet = bytes.mid(packetStart, packetBytes);
        QVERIFY(packet.contains("Item:Length=\"960\""));
        const QByteArray trailer =
            QByteArray::fromHex("00000018667479706d703432000000006d70343269736f6d");
        const bool hasMotionItem =
            variant == "motion" || variant == "truncated-video" || variant == "broken-gain";
        if (variant == "different-length") {
            packet.replace("Item:Length=\"960\"", "Item:Length=\"961\"");
        } else if (variant == "foreign-namespace") {
            packet.replace("http://ns.google.com/photos/1.0/container/item/",
                           "http://example.org/photos/1.0/container/item/");
        } else if (variant == "doctype") {
            packet.prepend("<!DOCTYPE x:xmpmeta [<!ENTITY gain \"960\">]>\n");
            packet.replace("Item:Length=\"960\"", "Item:Length=\"&gain;\"");
        } else if (variant == "padding") {
            packet.replace("Item:Semantic=\"Primary\"",
                           "Item:Semantic=\"Primary\" Item:Padding=\"1\"");
        } else if (hasMotionItem) {
            packet.replace(
                "<rdf:Description",
                "<rdf:Description xmlns:Camera=\"http://ns.google.com/photos/1.0/camera/\" "
                "Camera:MotionPhoto=\"1\" Camera:MotionPhotoVersion=\"1\"");
            packet.replace(
                "</rdf:Seq>",
                "<rdf:li rdf:parseType=\"Resource\"><Container:Item "
                "Item:Semantic=\"MotionPhoto\" Item:Mime=\"video/mp4\" Item:Length=\"24\"/>"
                "</rdf:li></rdf:Seq>");
        }
        int delta = int(packet.size() - packetBytes);
        bytes.replace(packetStart, packetBytes, packet);
        qToBigEndian<quint16>(quint16(length + delta), bytes.data() + signature - 2);
        if (variant == "missing") {
            bytes[signature] = 'x';
        } else if (variant == "duplicate-xmp") {
            const auto segment = bytes.mid(signature - 4, length + 2);
            bytes.insert(signature - 4, segment);
            delta += int(segment.size());
        }
        const auto mpf = bytes.indexOf(QByteArray("MPF\0", 4));
        QVERIFY(mpf >= 0);
        // Leave room between the MPF and the end of this tiny JPEG, as on
        // the camera original, so the stale count still includes the header.
        const int tail = variant == "excess-gap" ? 4096 : 512;
        QByteArray extra = QByteArray::fromHex("ffef0000");
        qToBigEndian<quint16>(quint16(tail + 2), extra.data() + 2);
        extra.append(QByteArray(tail, '\0'));
        const auto mpfEnd = mpf - 2 + qFromBigEndian<quint16>(bytes.constData() + mpf - 2);
        const auto gainOffsetField = mpf + 4 + 74;
        const auto gainOffset = qFromBigEndian<quint32>(bytes.constData() + gainOffsetField);
        qToBigEndian<quint32>(gainOffset + quint32(extra.size()), bytes.data() + gainOffsetField);
        bytes.insert(mpfEnd, extra);
        delta += int(extra.size());
        const int staleBy = variant == "excess-gap" ? 4097 : 307;
        qToBigEndian<quint32>(quint32(baseBytes + delta - staleBy), bytes.data() + mpf + 4 + 54);
        if (hasMotionItem) {
            if (variant == "broken-gain") {
                bytes[bytes.size() - 1] = '\0';
            }
            bytes.append(variant == "truncated-video" ? trailer.chopped(1) : trailer);
        } else if (variant == "boundary") {
            bytes[baseBytes + delta - 1] = '\0';
        }

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(hasMotionItem ? "camera.jfif" : "camera.jpg");
        QVERIFY(writeFile(path, bytes));
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        QString error;
        const auto result = Lab::decode(path, *identity, 4, error);
        QVERIFY2(result.has_value() == accepted, qPrintable(error));
        if (result) {
            QCOMPARE(result->jpegBytes, totalBytes + delta);
            QVERIFY(checkGray(result->linear, 4));
        }
        if (hasMotionItem) {
            QCOMPARE(QImage(path).pixelColor(16, 16), QColor(192, 192, 192));
            const auto asset = Licasa::probePhotoAsset(QUrl::fromLocalFile(path));
            if (variant == "truncated-video") {
                QVERIFY(!asset.motion || !asset.motion->hasEmbeddedVideo());
                return;
            }
            QVERIFY2(asset.kind == Licasa::PhotoAssetKind::MotionPhoto,
                     qPrintable(asset.metadataError));
            QVERIFY(asset.motion && asset.motion->hasEmbeddedVideo());
            const auto range = asset.motion->embedded;
            QCOMPARE(range.offset, quint64(totalBytes + delta));
            if (result) {
                QCOMPARE(range.offset, quint64(result->jpegBytes));
            }
            QCOMPARE(range.length, quint64(trailer.size()));
            Licasa::ByteRangeDevice video(path, range.offset, range.length, *identity);
            QVERIFY(video.open(QIODevice::ReadOnly));
            QCOMPARE(video.readAll(), trailer);
            QVERIFY(video.atEnd());
        }
    }

    void rejectsAboveTheFullResolutionCeiling()
    {
        auto bytes = fixture();
        const auto sof = bytes.indexOf(QByteArray::fromHex("ffc0"));
        QVERIFY(sof >= 0);
        // 4096² base pixels plus the gain image exceed the combined ceiling.
        qToBigEndian<quint16>(4096, bytes.data() + sof + 5);
        qToBigEndian<quint16>(4096, bytes.data() + sof + 7);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("oversized.jpg");
        QVERIFY(writeFile(path, bytes));
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        QString error;
        QVERIFY(!Lab::decode(path, *identity, 4, error,
                             {totalBytes, std::numeric_limits<quint64>::max()}));
        QVERIFY2(error.contains("raster budget"), qPrintable(error));
    }

    void generatedMaximumDimensions_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<int>("gainScale");
        QTest::addColumn<bool>("fits");
        // Primary and gain-map pixels together may reach 16 Mi and no more.
        QTest::newRow("largest-primary") << QSize(4096, 4094) << 128 << true;
        QTest::newRow("equal-map-at-the-ceiling") << QSize(4096, 2048) << 1 << true;
        QTest::newRow("primary-alone-at-the-ceiling") << QSize(4096, 4096) << 128 << false;
        QTest::newRow("equal-map-over") << QSize(4096, 2050) << 1 << false;
    }

    void generatedMaximumDimensions()
    {
        // Real, decodable files at the hard ceiling. The header-patched case
        // above cannot show that the largest admitted photo actually decodes.
        QFETCH(QSize, size);
        QFETCH(int, gainScale);
        QFETCH(bool, fits);
        QString error;
        const auto bytes = Lab::TestFixture::encodeGainMapJpeg(size, gainScale, false, &error);
        QVERIFY2(!bytes.isEmpty(), qPrintable(error));
        QVERIFY(quint64(bytes.size()) <= Lab::Limits{}.compressedBytes);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("generated.jpg");
        QVERIFY(writeFile(path, bytes));
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);

        // The default request never admits a photo this large.
        QVERIFY(!Lab::decode(path, *identity, 4, error));
        QVERIFY2(error.contains("raster budget"), qPrintable(error));
        Lab::Limits limits;
        limits.rasterPixels = std::numeric_limits<quint64>::max(); // The ceiling is not the caller's to raise.
        const auto decoded = Lab::decode(path, *identity, 4, error, limits);
        QCOMPARE(bool(decoded), fits);
        if (!fits) {
            QVERIFY2(error.contains("raster budget"), qPrintable(error));
            return;
        }
        QCOMPARE(decoded->linear.size(), size);
        QCOMPARE(decoded->linear.format(), QImage::Format_RGBA16FPx4);
        const quint64 pixels = quint64(size.width()) * size.height()
            + quint64(decoded->gainSize.width()) * decoded->gainSize.height();
        QVERIFY(pixels <= Lab::maximumRasterPixels);
        if (gainScale == 1) QCOMPARE(pixels, Lab::maximumRasterPixels);
        // One pixel less than the photo needs is refused.
        limits.rasterPixels = pixels - 1;
        QVERIFY(!Lab::decode(path, *identity, 4, error, limits));
        QVERIFY2(error.contains("raster budget"), qPrintable(error));
        // The ramp runs from black to above SDR white. The encoder chooses
        // its own tone curve, so only the ordering is asserted.
        const auto* line = reinterpret_cast<const qfloat16*>(decoded->linear.constScanLine(size.height() / 2));
        const float dark = float(line[0]), bright = float(line[size_t(size.width() - 1) * 4]);
        QVERIFY2(dark < 0.2f && bright > 1.05f, qPrintable(QStringLiteral("%1 %2").arg(dark).arg(bright)));
        qInfo() << "Generated" << size << "gain" << decoded->gainSize << "bytes" << bytes.size();
    }

    void retainsLinearDisplayP3()
    {
        const QString path = QStringLiteral(GAINMAP_P3_FIXTURE);
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        QString error;
        const auto decoded = Lab::decode(path, *identity, 4, error);
        QVERIFY2(decoded, qPrintable(error));
        QCOMPARE(decoded->linear.format(), QImage::Format_RGBA16FPx4);
        const QColorSpace color = decoded->linear.colorSpace();
        QCOMPARE(color.primaries(), QColorSpace::Primaries::DciP3D65);
        QCOMPARE(color.transferFunction(), QColorSpace::TransferFunction::Linear);
        QVERIFY(checkGray(decoded->linear, 4));
    }

    void rgbGainAtSeveralHeadrooms_data()
    {
        QTest::addColumn<float>("headroom");
        QTest::newRow("sdr") << 1.f;
        QTest::newRow("intermediate") << 2.f;
        QTest::newRow("full") << 4.f;
    }

    void rgbGainAtSeveralHeadrooms()
    {
        QFETCH(float, headroom);
        const QString path = QStringLiteral(GAINMAP_RGB_FIXTURE);
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        QString error;
        const auto decoded = Lab::decode(path, *identity, headroom, error);
        QVERIFY2(decoded, qPrintable(error));
        QCOMPARE(decoded->linear.size(), QSize(32, 16));
        QCOMPARE(decoded->gainSize, QSize(8, 4));
        // Use the gain grid (one map sample per four base pixels) to isolate
        // per-channel gain from interpolation at the edge of the map.
        const std::array<QPoint, 4> points{QPoint(0, 0), QPoint(28, 0), QPoint(0, 12),
                                           QPoint(28, 12)};
        const int mapCodes[4][3]{{254, 0, 0}, {0, 255, 1}, {0, 0, 254}, {255, 255, 255}};
        const float fullGain[]{4.f, 2.f, 3.f};
        const float weight = std::log2(headroom) / 2.f;
        for (size_t index = 0; index < points.size(); ++index) {
            const auto point = points[index];
            std::array<qfloat16, 4> sample;
            std::memcpy(sample.data(),
                        decoded->linear.constScanLine(point.y()) + point.x() * sizeof(sample),
                        sizeof(sample));
            for (int c = 0; c < 3; ++c) {
                const float expected =
                    linear192() * std::pow(fullGain[c], weight * mapCodes[index][c] / 255.f);
                QVERIFY2(std::abs(float(sample[c]) - expected) < 0.012f,
                         qPrintable(QStringLiteral("point %1 channel %2 actual %3 expected %4")
                                        .arg(index)
                                        .arg(c)
                                        .arg(float(sample[c]))
                                        .arg(expected)));
            }
            QCOMPARE(float(sample[3]), 1.0f);
        }
    }

    void primaryOrientation_data()
    {
        QTest::addColumn<int>("orientation");
        for (int i = 1; i <= 8; ++i) {
            const QByteArray name = QByteArray::number(i);
            QTest::newRow(name.constData()) << i;
        }
    }

    void primaryOrientation()
    {
        QFETCH(int, orientation);
        const QString unrotated = QStringLiteral(GAINMAP_RGB_FIXTURE);
        const auto unrotatedIdentity = Licasa::externalFileIdentityAtPath(unrotated);
        QVERIFY(unrotatedIdentity);
        QString error;
        const auto original = Lab::decode(unrotated, *unrotatedIdentity, 4, error);
        QVERIFY2(original, qPrintable(error));
        QCOMPARE(original->linear.size(), QSize(32, 16));
        auto bytes = fixture(unrotated);
        const auto mpf = bytes.indexOf(QByteArray("MPF\0", 4));
        QVERIFY(mpf >= 0);
        const auto baseSizeField = mpf + 4 + 54;
        const auto gainSizeField = mpf + 4 + 70;
        const auto baseBytes = qFromBigEndian<quint32>(bytes.constData() + baseSizeField);
        const auto gainBytes = qFromBigEndian<quint32>(bytes.constData() + gainSizeField);
        const QByteArray primaryExif = exifOrientation(orientation);
        // Deliberately disagree with the primary. The auxiliary JPEG is
        // sampled in storage coordinates and its EXIF must have no effect.
        const QByteArray gainExif = exifOrientation(orientation == 8 ? 2 : 8);
        qToBigEndian<quint32>(baseBytes + quint32(primaryExif.size()),
                              bytes.data() + baseSizeField);
        qToBigEndian<quint32>(gainBytes + quint32(gainExif.size()), bytes.data() + gainSizeField);
        bytes.insert(baseBytes + 2, gainExif);
        bytes.insert(2, primaryExif);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("oriented.jpg");
        QVERIFY(writeFile(path, bytes));
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        const auto oriented = Lab::decode(path, *identity, 4, error);
        QVERIFY2(oriented, qPrintable(error));
        QImageReader sdr(path);
        sdr.setAutoTransform(true);
        QCOMPARE(oriented->linear.size(), sdr.read().size());
        QCOMPARE(oriented->linear.size(), orientation >= 5 ? QSize(16, 32) : QSize(32, 16));
        QCOMPARE(oriented->linear.format(), QImage::Format_RGBA16FPx4);
        QCOMPARE(oriented->linear.colorSpace(), original->linear.colorSpace());
        QCOMPARE(oriented->gainSize, QSize(8, 4));
        QCOMPARE(oriented->jpegBytes, bytes.size());
        for (int y = 0; y < oriented->linear.height(); ++y) {
            for (int x = 0; x < oriented->linear.width(); ++x) {
                const QPoint source = storagePoint(orientation, x, y);
                QCOMPARE(std::memcmp(oriented->linear.constScanLine(y) + 8 * x,
                                     original->linear.constScanLine(source.y()) + 8 * source.x(),
                                     8),
                         0);
            }
        }
    }

    void boundsAndCancellation()
    {
        const QString path = QStringLiteral(GAINMAP_FIXTURE);
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        QString error;
        QVERIFY(!Lab::decode(path, *identity, 4, error, {totalBytes - 1, combinedPixels}));
        QVERIFY(error.contains("compressed"));
        QVERIFY(!Lab::decode(path, *identity, 4, error, {totalBytes, combinedPixels - 1}));
        QVERIFY(error.contains("raster"));
        QVERIFY2(Lab::decode(path, *identity, 4, error, {totalBytes, combinedPixels}),
                 qPrintable(error));
        QVERIFY(!Lab::decode(path, *identity, std::numeric_limits<float>::infinity(), error));
        QVERIFY(error.contains("headroom"));
        std::atomic_bool cancelled{true};
        QVERIFY(!Lab::decode(path, *identity, 4, error, {}, &cancelled));
        QVERIFY(error.contains("cancelled"));
    }

    void compressedCeiling()
    {
        const QSize size(3520, 3520);
        constexpr qsizetype ceiling = Lab::Limits{}.compressedBytes;
        QString error;
        auto bytes = Lab::TestFixture::encodeGainMapJpeg(size, 16, false, &error,
                                                         Lab::TestFixture::Texture::Noise);
        QVERIFY2(!bytes.isEmpty(), qPrintable(error));
        const qsizetype padding = ceiling - bytes.size();
        QVERIFY2(padding >= 4 && padding < 512 * 1024, qPrintable(QString::number(bytes.size())));

        // The entropy-coded images account for nearly the entire span. Add
        // valid JPEG comments to hit the exact byte boundaries. Both the
        // MPF TIFF header and gain image move by the same
        // amount, so their relative offset stays valid.
        const auto mpf = bytes.indexOf(QByteArray("MPF\0MM", 6));
        QVERIFY(mpf >= 0);
        const auto primarySizeField = mpf + 4 + 54;
        const auto primarySize = qFromBigEndian<quint32>(bytes.constData() + primarySizeField);
        QByteArray comments;
        while (comments.size() < padding) {
            const auto left = padding - comments.size();
            const qsizetype segmentBytes = left <= 65536 ? left : qMin<qsizetype>(65536, left - 4);
            const auto start = comments.size();
            comments.append(QByteArray(segmentBytes, ' '));
            qToBigEndian<quint16>(0xfffe, comments.data() + start);
            qToBigEndian<quint16>(quint16(segmentBytes - 2), comments.data() + start + 2);
        }
        qToBigEndian<quint32>(primarySize + quint32(padding), bytes.data() + primarySizeField);
        bytes.insert(2, comments);
        QCOMPARE(bytes.size(), ceiling);
        qInfo() << "Generated" << size << "JPEG with" << padding << "bytes of comments";

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("compressed-limit.jpg");
        QVERIFY(writeFile(path, bytes));
        const auto identity = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(identity);
        const Lab::Limits limits{std::numeric_limits<quint64>::max(), Lab::maximumRasterPixels};
        {
            const auto decoded = Lab::decode(path, *identity, 4, error, limits);
            QVERIFY2(decoded, qPrintable(error));
            QCOMPARE(decoded->jpegBytes, ceiling);
            QCOMPARE(decoded->linear.size(), size);
            QCOMPARE(decoded->linear.format(), QImage::Format_RGBA16FPx4);
        }

        // Extend the first comment by one byte, keeping both JPEGs intact.
        const auto length = qFromBigEndian<quint16>(bytes.constData() + 4);
        qToBigEndian<quint16>(length + 1, bytes.data() + 4);
        qToBigEndian<quint32>(primarySize + quint32(padding) + 1,
                              bytes.data() + primarySizeField + padding);
        bytes.insert(6, ' ');
        QCOMPARE(bytes.size(), ceiling + 1);
        QVERIFY(writeFile(path, bytes));
        const auto oversized = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(oversized);
        QVERIFY(!Lab::decode(path, *oversized, 4, error, limits));
        QVERIFY2(error.contains("compressed gain-map budget"), qPrintable(error));
    }

    void rejectsReplacedSource()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("image.jpg");
        QVERIFY(writeFile(path, fixture()));
        const auto original = Licasa::externalFileIdentityAtPath(path);
        QVERIFY(original);
        QVERIFY(QFile::rename(path, directory.filePath("old.jpg")));
        QVERIFY(writeFile(path, fixture()));
        QString error;
        QVERIFY(!Lab::decode(path, *original, 4, error));
        QVERIFY(error.contains("source changed"));
    }
};

QTEST_GUILESS_MAIN(GainmapTest)
#include "tst_gainmap.moc"
