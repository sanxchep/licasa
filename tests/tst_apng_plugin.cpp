#include "app/application_settings.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_animation.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QImageWriter>
#include <QQuickImageProvider>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

namespace Contract = Licasa::ImageDecodeContract;
namespace {
constexpr char pngSignatureBytes[] = "\x89PNG\r\n\x1a\n";

const std::array<quint32, 256>& crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> values{};
        for (quint32 n = 0; n < values.size(); ++n) {
            quint32 c = n;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? 0xedb88320u ^ (c >> 1u) : c >> 1u;
            }
            values[n] = c;
        }
        return values;
    }();
    return table;
}

quint32 updateCrc(quint32 crc, const QByteArray& bytes)
{
    const auto& table = crcTable();
    for (const char value : bytes) {
        const auto byte = static_cast<quint8>(value);
        crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8u);
    }
    return crc;
}

QByteArray chunk(const QByteArray& type, const QByteArray& payload)
{
    QByteArray result(12 + payload.size(), Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(payload.size()), reinterpret_cast<uchar*>(result.data()));
    std::copy(type.cbegin(), type.cend(), result.begin() + 4);
    std::copy(payload.cbegin(), payload.cend(), result.begin() + 8);
    quint32 crc = 0xffffffffu;
    crc = updateCrc(crc, type);
    crc = updateCrc(crc, payload) ^ 0xffffffffu;
    qToBigEndian<quint32>(crc, reinterpret_cast<uchar*>(result.data() + 8 + payload.size()));
    return result;
}

QByteArray encodePng(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    QImageWriter writer(&buffer, "png");
    writer.setCompression(6);
    return writer.write(image) ? bytes : QByteArray{};
}

struct PngPayload {
    QByteArray ihdr;
    QList<QByteArray> idat;
};

PngPayload pngPayload(const QImage& image)
{
    const QByteArray png = encodePng(image);
    PngPayload result;
    if (png.size() < 33 || png.left(8) != QByteArray::fromRawData(pngSignatureBytes, 8)) {
        return result;
    }

    qsizetype offset = 8;
    while (offset + 12 <= png.size()) {
        const quint32 length =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(png.constData() + offset));
        const QByteArray type = png.mid(offset + 4, 4);
        const quint64 next = quint64(offset) + 12u + quint64(length);
        if (next > quint64(png.size())) {
            return {};
        }
        const QByteArray payload = png.mid(offset + 8, length);
        if (type == "IHDR") {
            result.ihdr = payload;
        } else if (type == "IDAT") {
            result.idat.append(payload);
        } else if (type == "IEND") {
            break;
        }
        offset = qsizetype(next);
    }
    return result;
}

QByteArray be32(quint32 value)
{
    QByteArray result(4, Qt::Uninitialized);
    qToBigEndian<quint32>(value, reinterpret_cast<uchar*>(result.data()));
    return result;
}

QByteArray frameControl(quint32 sequence, const QSize& size, const QPoint& offset,
                        quint16 delayNumerator, quint16 delayDenominator, quint8 dispose,
                        quint8 blend)
{
    QByteArray data(26, Qt::Uninitialized);
    qToBigEndian<quint32>(sequence, reinterpret_cast<uchar*>(data.data()));
    qToBigEndian<quint32>(quint32(size.width()), reinterpret_cast<uchar*>(data.data() + 4));
    qToBigEndian<quint32>(quint32(size.height()), reinterpret_cast<uchar*>(data.data() + 8));
    qToBigEndian<quint32>(quint32(offset.x()), reinterpret_cast<uchar*>(data.data() + 12));
    qToBigEndian<quint32>(quint32(offset.y()), reinterpret_cast<uchar*>(data.data() + 16));
    qToBigEndian<quint16>(delayNumerator, reinterpret_cast<uchar*>(data.data() + 20));
    qToBigEndian<quint16>(delayDenominator, reinterpret_cast<uchar*>(data.data() + 22));
    data[24] = char(dispose);
    data[25] = char(blend);
    return data;
}

QByteArray apngFixture()
{
    QImage first(QSize(4, 4), QImage::Format_RGBA8888);
    first.fill(QColor(255, 0, 0, 255));
    QImage overlay(QSize(2, 2), QImage::Format_RGBA8888);
    overlay.fill(QColor(0, 255, 0, 128));
    QImage final(QSize(1, 1), QImage::Format_RGBA8888);
    final.fill(QColor(0, 0, 255, 255));

    const PngPayload firstPng = pngPayload(first);
    const PngPayload overlayPng = pngPayload(overlay);
    const PngPayload finalPng = pngPayload(final);
    if (firstPng.ihdr.size() != 13 || firstPng.idat.isEmpty() || overlayPng.idat.isEmpty() ||
        finalPng.idat.isEmpty()) {
        return {};
    }

    QByteArray bytes = QByteArray::fromRawData(pngSignatureBytes, 8);
    bytes += chunk("IHDR", firstPng.ihdr);
    QByteArray animationControl;
    animationControl += be32(3); // frames
    animationControl += be32(2); // total plays; QImageReader loopCount == 1
    bytes += chunk("acTL", animationControl);

    quint32 sequence = 0;
    bytes += chunk("fcTL", frameControl(sequence++, QSize(4, 4), QPoint(0, 0), 100, 1000, 0, 0));
    for (const auto& payload : firstPng.idat) {
        bytes += chunk("IDAT", payload);
    }

    bytes += chunk("fcTL", frameControl(sequence++, QSize(2, 2), QPoint(1, 1), 200, 1000, 2, 1));
    for (const auto& payload : overlayPng.idat) {
        bytes += chunk("fdAT", be32(sequence++) + payload);
    }

    bytes += chunk("fcTL", frameControl(sequence++, QSize(1, 1), QPoint(0, 0), 300, 1000, 0, 0));
    for (const auto& payload : finalPng.idat) {
        bytes += chunk("fdAT", be32(sequence++) + payload);
    }
    bytes += chunk("IEND", {});
    return bytes;
}

QString codecError(QImageReader& reader)
{
    return reader.device() ? reader.device()->property(Contract::errorProperty).toString()
                           : QString{};
}

bool closeColor(const QColor& actual, const QColor& expected, int tolerance = 1)
{
    return std::abs(actual.red() - expected.red()) <= tolerance &&
           std::abs(actual.green() - expected.green()) <= tolerance &&
           std::abs(actual.blue() - expected.blue()) <= tolerance &&
           std::abs(actual.alpha() - expected.alpha()) <= tolerance;
}

QString writeFixture(QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

QByteArray replaceFirstFrameWidth(QByteArray bytes, quint32 width)
{
    const qsizetype marker = bytes.indexOf("fcTL");
    if (marker < 4 || marker + 30 > bytes.size()) {
        return {};
    }
    const qsizetype chunkStart = marker - 4;
    const quint32 length =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + chunkStart));
    if (length != 26) {
        return {};
    }
    QByteArray payload = bytes.mid(marker + 4, 26);
    qToBigEndian<quint32>(width, reinterpret_cast<uchar*>(payload.data() + 4));
    bytes.replace(chunkStart, 38, chunk("fcTL", payload));
    return bytes;
}

struct Viewer {
    Licasa::ViewerPreferences preferences;
    Licasa::ImageResourcePolicy policy{preferences};
    std::unique_ptr<Licasa::AsyncImageProvider> pool{new Licasa::AsyncImageProvider(policy)};
    std::unique_ptr<Licasa::ImageAnimationService> service{
        new Licasa::ImageAnimationService(policy, *pool)};
    std::unique_ptr<QQuickImageProvider> frames{service->createFrameProvider()};
    std::unique_ptr<QObject> owner{new QObject};
    Licasa::ImageAnimation* animation =
        qobject_cast<Licasa::ImageAnimation*>(service->createController(owner.get()));

    Viewer() { preferences.setMaximumImageMegapixels(25); }
    ~Viewer()
    {
        owner.reset();
        pool.reset();
    }

    QImage frame() const
    {
        QSize size;
        return frames->requestImage(animation->frameSource().path().mid(1), &size, {});
    }
};
} // namespace

class ApngPluginTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("LicasaTests");
        QCoreApplication::setApplicationName("ApngPluginTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        preferences_->setMaximumImageMegapixels(25);
        policy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);

        QVERIFY(QImageReader::supportedImageFormats().contains("apng"));
        QVERIFY(QImageReader::supportedMimeTypes().contains("image/apng"));
    }

    void animationContractAndComposition()
    {
        QByteArray data = apngFixture();
        QVERIFY(!data.isEmpty());
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "apng");
        Contract::configure(reader, policy_->maximumImagePixels());

        QVERIFY2(reader.canRead(), qPrintable(codecError(reader)));
        QCOMPARE(reader.format(), QByteArray("apng"));
        QCOMPARE(reader.size(), QSize(4, 4));
        QVERIFY(reader.supportsAnimation());
        QCOMPARE(reader.imageCount(), 3);
        QCOMPARE(reader.loopCount(), 1);
        QVERIFY(!reader.supportsOption(QImageIOHandler::ScaledSize));
        QVERIFY(buffer.property("_licasaApngHandler").toBool());
        QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());

        const QImage frame0 = reader.read();
        QVERIFY2(!frame0.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(frame0.size(), QSize(4, 4));
        QVERIFY(closeColor(frame0.pixelColor(1, 1), QColor(255, 0, 0, 255)));
        QCOMPARE(reader.nextImageDelay(), 100);
        QCOMPARE(buffer.property("_licasaNativeRasterPixels").toULongLong(), quint64(16));

        const QImage frame1 = reader.read();
        QVERIFY2(!frame1.isNull(), qPrintable(codecError(reader)));
        QVERIFY(closeColor(frame1.pixelColor(0, 0), QColor(255, 0, 0, 255)));
        QVERIFY(closeColor(frame1.pixelColor(1, 1), QColor(127, 128, 0, 255), 2));
        QCOMPARE(reader.nextImageDelay(), 200);
        QCOMPARE(buffer.property("_licasaNativeRasterPixels").toULongLong(), quint64(4));

        const QImage frame2 = reader.read();
        QVERIFY2(!frame2.isNull(), qPrintable(codecError(reader)));
        QVERIFY(closeColor(frame2.pixelColor(0, 0), QColor(0, 0, 255, 255)));
        // Frame 1 used PREVIOUS disposal, so its green overlay must be gone.
        QVERIFY(closeColor(frame2.pixelColor(1, 1), QColor(255, 0, 0, 255)));
        QCOMPARE(reader.nextImageDelay(), 300);
        QCOMPARE(buffer.property("_licasaNativeRasterPixels").toULongLong(), quint64(1));

        QVERIFY(reader.jumpToImage(1));
        const QImage seeked = reader.read();
        QVERIFY2(!seeked.isNull(), qPrintable(codecError(reader)));
        QVERIFY(closeColor(seeked.pixelColor(1, 1), QColor(127, 128, 0, 255), 2));
        QVERIFY(closeColor(seeked.pixelColor(0, 0), QColor(255, 0, 0, 255)));
    }

    void pngExtensionIsDetectedButStaticPngFallsThrough()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray apng = apngFixture();
        const QString disguised = writeFixture(directory, "animation.png", apng);
        QVERIFY(!disguised.isEmpty());

        QImageReader animated(disguised);
        Contract::configure(animated, policy_->maximumImagePixels());
        QVERIFY2(animated.canRead(), qPrintable(codecError(animated)));
        QCOMPARE(animated.format(), QByteArray("apng"));
        QVERIFY(animated.supportsAnimation());
        QCOMPARE(animated.imageCount(), 3);
        QVERIFY(!animated.read().isNull());
        QVERIFY(animated.device()->property("_licasaApngHandler").toBool());

        QImage ordinaryImage(QSize(7, 5), QImage::Format_RGB32);
        ordinaryImage.fill(QColor(20, 40, 60));
        QByteArray png = encodePng(ordinaryImage);
        QVERIFY(!png.isEmpty());
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader ordinary(&buffer);
        Contract::configure(ordinary, policy_->maximumImagePixels());
        QVERIFY(ordinary.canRead());
        QCOMPARE(ordinary.format(), QByteArray("png"));
        QVERIFY(!ordinary.supportsAnimation());
        const QImage decoded = ordinary.read();
        QCOMPARE(decoded.size(), QSize(7, 5));
        QVERIFY(!buffer.property("_licasaApngHandler").isValid());
    }

    void selectedPixelLimitRejectsBeforeRasterDecode()
    {
        QByteArray data = apngFixture();
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "apng");
        Contract::configure(reader, 8);
        QCOMPARE(reader.size(), QSize(4, 4));
        QVERIFY(reader.supportsAnimation());
        QVERIFY(reader.read().isNull());
        QVERIFY(codecError(reader).contains("limit", Qt::CaseInsensitive));
        QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());
    }

    void cancellationBeforeRasterDecode()
    {
        QByteArray data = apngFixture();
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        std::atomic_bool cancelled{true};
        QImageReader reader(&buffer, "apng");
        Contract::configure(reader, policy_->maximumImagePixels(), &cancelled);
        QVERIFY(reader.read().isNull());
        QVERIFY(codecError(reader).contains("cancel", Qt::CaseInsensitive));
        QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());
    }

    void malformedInputsFailBeforeRasterDecode()
    {
        const QByteArray original = apngFixture();
        QVERIFY(original.size() > 100);

        for (const int length : {20, 40, 60, 80, 100, int(original.size() - 1)}) {
            QByteArray truncated = original.left(length);
            QBuffer buffer(&truncated);
            QVERIFY(buffer.open(QIODevice::ReadOnly));
            QImageReader reader(&buffer, "apng");
            Contract::configure(reader, policy_->maximumImagePixels());
            QVERIFY2(reader.read().isNull(),
                     qPrintable(QStringLiteral("truncation %1 decoded").arg(length)));
            QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());
        }

        QByteArray badControl = replaceFirstFrameWidth(original, 5);
        QVERIFY(!badControl.isEmpty());
        QBuffer oversized(&badControl);
        QVERIFY(oversized.open(QIODevice::ReadOnly));
        QImageReader reader(&oversized, "apng");
        Contract::configure(reader, policy_->maximumImagePixels());
        QVERIFY(reader.read().isNull());
        QVERIFY(codecError(reader).contains("frame-control", Qt::CaseInsensitive));
        QVERIFY(!oversized.property("_licasaNativeRasterPixels").isValid());

        QByteArray badFdat = original;
        const qsizetype fdat = badFdat.indexOf("fdAT");
        QVERIFY(fdat >= 0 && fdat + 12 < badFdat.size());
        badFdat[fdat + 10] ^= char(0x5a); // payload mutation, CRC left unchanged
        QBuffer corrupt(&badFdat);
        QVERIFY(corrupt.open(QIODevice::ReadOnly));
        QImageReader corruptReader(&corrupt, "apng");
        Contract::configure(corruptReader, policy_->maximumImagePixels());
        QVERIFY(!corruptReader.read().isNull()); // frame 0 is independent IDAT
        QCOMPARE(corrupt.property("_licasaNativeRasterPixels").toULongLong(), quint64(16));
        QVERIFY(corruptReader.read().isNull()); // frame 1 validates its fdAT lazily
        QVERIFY(codecError(corruptReader).contains("CRC", Qt::CaseInsensitive));
        QVERIFY(!corrupt.property("_licasaNativeRasterPixels").isValid());
    }

    void genericAnimationServiceSeeksApng()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, "service.apng", apngFixture());
        QVERIFY(!path.isEmpty());

        Viewer viewer;
        viewer.animation->setSource(QUrl::fromLocalFile(path));
        viewer.animation->setPlaying(false);
        viewer.animation->setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 0, 5000);
        QCOMPARE(viewer.animation->frameCount(), 3);
        QVERIFY(closeColor(viewer.frame().pixelColor(1, 1), QColor(255, 0, 0, 255)));

        viewer.animation->seekFrame(2);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 2, 5000);
        QVERIFY(closeColor(viewer.frame().pixelColor(0, 0), QColor(0, 0, 255, 255)));
        QVERIFY(closeColor(viewer.frame().pixelColor(1, 1), QColor(255, 0, 0, 255)));
        QVERIFY(viewer.animation->error().isEmpty());
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
    ApngPluginTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_apng_plugin.moc"
