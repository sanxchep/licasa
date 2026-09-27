#include "app/application_settings.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QGuiApplication>
#include <QImageIOHandler>
#include <QImageReader>
#include <QList>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <memory>

namespace Contract = Licasa::ImageDecodeContract;
namespace {
QString fixture(const char* name)
{
    return QStringLiteral(LICASA_SOURCE_DIR "/tests/test-assets/formats/core-matrix/avif/") +
           QString::fromLatin1(name);
}
QString alphaFixture()
{
    return QStringLiteral(LICASA_SOURCE_DIR
                          "/tests/test-assets/backgrounds-and-alpha/transparent-alpha.avif");
}
QString codecError(QImageReader& reader)
{
    return reader.device() ? reader.device()->property(Contract::errorProperty).toString()
                           : QString{};
}
QByteArray contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

bool setBoxSize(QByteArray* data, const QByteArray& type, quint32 size)
{
    if (!data || type.size() != 4) {
        return false;
    }
    const qsizetype marker = data->indexOf(type);
    if (marker < 4) {
        return false;
    }
    qToBigEndian<quint32>(size, reinterpret_cast<uchar*>(data->data() + marker - 4));
    return true;
}

bool setFirstIlocExtentLength(QByteArray* data, quint32 extentLength)
{
    if (!data) {
        return false;
    }
    const qsizetype marker = data->indexOf("iloc");
    if (marker < 4) {
        return false;
    }

    const qsizetype boxStart = marker - 4;
    if (boxStart + 16 > data->size()) {
        return false;
    }
    const quint32 boxSize =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data->constData() + boxStart));
    if (boxSize < 20 || quint64(boxStart) + boxSize > quint64(data->size())) {
        return false;
    }

    const uchar version = uchar(data->at(marker + 4));
    if (version != 0) {
        return false;
    }

    const uchar sizes = uchar(data->at(marker + 8));
    const int offsetSize = (sizes >> 4) & 0x0f;
    const int lengthSize = sizes & 0x0f;
    const uchar baseSizes = uchar(data->at(marker + 9));
    const int baseOffsetSize = (baseSizes >> 4) & 0x0f;
    if (offsetSize != 4 || lengthSize != 4 || baseOffsetSize != 4) {
        return false;
    }

    // Version-0 iloc layout used by the deterministic test fixture:
    // full-box(4), sizes(2), item-count(2), item-id(2), data-ref(2),
    // base-offset(4), extent-count(2), extent-offset(4), extent-length(4).
    const qsizetype itemCountOffset = marker + 10;
    const qsizetype extentCountOffset = marker + 20;
    const qsizetype extentLengthOffset = marker + 26;
    if (extentLengthOffset + 4 != boxStart + qsizetype(boxSize) ||
        qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(data->constData() + itemCountOffset)) != 1 ||
        qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(data->constData() + extentCountOffset)) != 1) {
        return false;
    }
    qToBigEndian<quint32>(extentLength,
                          reinterpret_cast<uchar*>(data->data() + extentLengthOffset));
    return true;
}

bool setNclx(QByteArray* data, quint16 primaries, quint16 transfer)
{
    if (!data) {
        return false;
    }
    const qsizetype marker = data->indexOf("nclx");
    if (marker < 0 || marker + 11 > data->size()) {
        return false;
    }
    qToBigEndian<quint16>(primaries, reinterpret_cast<uchar*>(data->data() + marker + 4));
    qToBigEndian<quint16>(transfer, reinterpret_cast<uchar*>(data->data() + marker + 6));
    return true;
}

QImage decodeBytes(QByteArray data, quint64 pixelLimit, QString* error = nullptr)
{
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer, "avif");
    Contract::configure(reader, pixelLimit);
    QImage image = reader.read();
    if (error) {
        *error = codecError(reader);
    }
    return image;
}
} // namespace

class AvifPluginTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("LicasaTests");
        QCoreApplication::setApplicationName("AvifPluginTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        preferences_->setMaximumImageMegapixels(25);
        policy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);
        QVERIFY(QImageReader::supportedImageFormats().contains("avif"));
        QVERIFY(QImageReader::supportedMimeTypes().contains("image/avif"));
    }

    void metadataAndBasicDecode_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QSize>("size");
        QTest::newRow("micro") << "micro-1x1.avif" << QSize(1, 1);
        QTest::newRow("icon") << "icon-64x64.avif" << QSize(64, 64);
        QTest::newRow("medium") << "medium-640x360.avif" << QSize(640, 360);
        QTest::newRow("full-hd") << "full-hd-1920x1080.avif" << QSize(1920, 1080);
    }

    void metadataAndBasicDecode()
    {
        QFETCH(QString, name);
        QFETCH(QSize, size);
        QImage image;
        {
            QImageReader reader(fixture(name.toLatin1().constData()));
            Contract::configure(reader, policy_->maximumImagePixels());
            QVERIFY2(reader.canRead(), qPrintable(codecError(reader)));
            QCOMPARE(reader.format(), QByteArray("avif"));
            QCOMPARE(reader.size(), size);
            QCOMPARE(reader.imageCount(), 1);
            QVERIFY(!reader.supportsAnimation());
            QVERIFY(reader.supportsOption(QImageIOHandler::ScaledSize));
            QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
            image = reader.read();
            QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
            QCOMPARE(image.size(), size);
            QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                     quint64(size.width()) * quint64(size.height()));
            QVERIFY(reader.text("PreviewPath").contains("full native raster required"));
        }
        // The returned pixels are Qt-owned and remain valid after both the
        // QImageReader and the lazily loaded native backend handler are gone.
        QVERIFY(!image.isNull());
        QVERIFY(image.constBits() != nullptr);
    }

#ifdef LICASA_AVIF_XMP_FIXTURE
    void xmpMetadataBridgeIsMetadataOnly()
    {
        QImageReader reader(QStringLiteral(LICASA_AVIF_XMP_FIXTURE));
        Contract::configure(reader, policy_->maximumImagePixels());
        QVERIFY2(reader.canRead(), qPrintable(codecError(reader)));
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
        const QString bridgeError = reader.text(QStringLiteral("LicasaXmpError"));
        QVERIFY2(bridgeError.isEmpty(), qPrintable(bridgeError));
        const QByteArray encoded = reader.text(QStringLiteral("LicasaXmpBase64")).toLatin1();
        QVERIFY(!encoded.isEmpty());
        const QByteArray xmp = QByteArray::fromBase64(encoded);
        QCOMPARE(xmp.toBase64(), encoded);
        QVERIFY(!xmp.isEmpty());
        QVERIFY(xmp.size() <= 1024 * 1024);
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
    }
#endif

    void requestedSizeReportsNativeWork()
    {
        QImageReader reader(fixture("uhd-4k-3840x2160.avif"));
        Contract::configure(reader, policy_->maximumImagePixels());
        QCOMPARE(reader.size(), QSize(3840, 2160));
        reader.setScaledSize(QSize(640, 360));
        const QImage image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(image.size(), QSize(640, 360));
        QCOMPARE(reader.device()->property("_licasaNativeRasterPixels").toULongLong(),
                 quint64(3840) * 2160);
        QVERIFY(reader.text("PreviewPath").contains("full native raster required"));
    }

    void selectedPixelLimitRejectsBeforeAv1Decode()
    {
        QImageReader reader(fixture("uhd-4k-3840x2160.avif"));
        Contract::configure(reader, 1'000'000);
        // Metadata is allowed to describe the image even though pixel admission
        // rejects the working AV1 raster before avifDecoderNextImage().
        QCOMPARE(reader.size(), QSize(3840, 2160));
        reader.setScaledSize(QSize(640, 360));
        QVERIFY(reader.read().isNull());
        QVERIFY(codecError(reader).contains("limit", Qt::CaseInsensitive));
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
    }

    void alphaIsPreserved()
    {
        QImageReader reader(alphaFixture());
        Contract::configure(reader, policy_->maximumImagePixels());
        const QImage image = reader.read();
        QVERIFY2(!image.isNull(), qPrintable(codecError(reader)));
        QCOMPARE(image.size(), QSize(800, 600));
        QVERIFY(image.hasAlphaChannel());
        int minimum = 255;
        int maximum = 0;
        for (int y = 0; y < image.height(); y += 7) {
            for (int x = 0; x < image.width(); x += 7) {
                const int alpha = image.pixelColor(x, y).alpha();
                minimum = std::min(minimum, alpha);
                maximum = std::max(maximum, alpha);
            }
        }
        QVERIFY(minimum < 255);
        QVERIFY(maximum > 0);
    }

    void cancellationBeforeDecode()
    {
        std::atomic_bool cancelled{true};
        QImageReader reader(fixture("full-hd-1920x1080.avif"));
        Contract::configure(reader, policy_->maximumImagePixels(), &cancelled);
        QVERIFY(reader.read().isNull());
        QVERIFY(codecError(reader).contains("cancel", Qt::CaseInsensitive));
        QVERIFY(!reader.device()->property("_licasaNativeRasterPixels").isValid());
    }

    void nclxColorMapping()
    {
        const QByteArray original = contents(fixture("medium-640x360.avif"));
        QVERIFY(original.indexOf("nclx") >= 0);

        const QImage srgb = decodeBytes(original, policy_->maximumImagePixels());
        QVERIFY(!srgb.isNull());
        QCOMPARE(srgb.colorSpace(), QColorSpace(QColorSpace::SRgb));

        QByteArray p3Bytes = original;
        QVERIFY(setNclx(&p3Bytes, 12, 13)); // SMPTE 432 primaries + sRGB transfer.
        const QImage p3 = decodeBytes(p3Bytes, policy_->maximumImagePixels());
        QVERIFY(!p3.isNull());
        QCOMPARE(p3.colorSpace(), QColorSpace(QColorSpace::DisplayP3));

        QByteArray pqBytes = original;
        QVERIFY(setNclx(&pqBytes, 12, 16)); // PQ must not be mislabeled as SDR Display P3.
        const QImage pq = decodeBytes(pqBytes, policy_->maximumImagePixels());
        QVERIFY(!pq.isNull());
        QVERIFY(!pq.colorSpace().isValid());

        QByteArray hlgBytes = original;
        QVERIFY(setNclx(&hlgBytes, 9, 18)); // Rec.2020 + HLG remains explicitly untagged.
        const QImage hlg = decodeBytes(hlgBytes, policy_->maximumImagePixels());
        QVERIFY(!hlg.isNull());
        QVERIFY(!hlg.colorSpace().isValid());
    }

    void containerStructureBounds()
    {
        const QByteArray original = contents(fixture("medium-640x360.avif"));
        QVERIFY(original.size() > 512);

        QList<QByteArray> cases;

        QByteArray hugeFtyp = original;
        qToBigEndian<quint32>(0x7fffffffu, reinterpret_cast<uchar*>(hugeFtyp.data()));
        cases.append(hugeFtyp);

        QByteArray hugeMeta = original;
        QVERIFY(setBoxSize(&hugeMeta, "meta", 0x7fffffffu));
        cases.append(hugeMeta);

        QByteArray shortIloc = original;
        QVERIFY(setBoxSize(&shortIloc, "iloc", 8));
        cases.append(shortIloc);

        QByteArray oversizedItemCount = original;
        const qsizetype iinf = oversizedItemCount.indexOf("iinf");
        QVERIFY(iinf >= 0 && iinf + 10 <= oversizedItemCount.size());
        qToBigEndian<quint16>(0xffffu,
                              reinterpret_cast<uchar*>(oversizedItemCount.data() + iinf + 8));
        cases.append(oversizedItemCount);

        QByteArray outOfRangeExtent = original;
        QVERIFY(setFirstIlocExtentLength(&outOfRangeExtent, 0x7fffffffu));
        cases.append(outOfRangeExtent);

        for (qsizetype index = 0; index < cases.size(); ++index) {
            QByteArray data = cases.at(index);
            QBuffer buffer(&data);
            QVERIFY(buffer.open(QIODevice::ReadOnly));
            QImageReader reader(&buffer, "avif");
            Contract::configure(reader, policy_->maximumImagePixels());
            QVERIFY2(reader.read().isNull(),
                     qPrintable(QStringLiteral("malformed BMFF case %1 decoded").arg(index)));
            QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());
        }
    }

    void malformedAndTruncatedInputs()
    {
        const QByteArray original = contents(fixture("medium-640x360.avif"));
        QVERIFY(original.size() > 256);
        for (int length : {0, 4, 8, 12, 16, 24, 64, 128, 256}) {
            QByteArray data = original.left(length);
            QBuffer buffer(&data);
            QVERIFY(buffer.open(QIODevice::ReadOnly));
            QImageReader reader(&buffer, "avif");
            Contract::configure(reader, policy_->maximumImagePixels());
            QVERIFY2(reader.read().isNull(),
                     qPrintable(QStringLiteral("truncation %1 decoded").arg(length)));
            QVERIFY(!buffer.property("_licasaNativeRasterPixels").isValid());
        }

        QByteArray corrupted = original;
        // Preserve the ftyp signature so the AVIF plugin is selected, then
        // damage container bytes beyond the bounded signature probe.
        for (int offset : {96, 128, 192, 256}) {
            if (offset < corrupted.size()) {
                corrupted[offset] ^= char(0x5a);
            }
        }
        QBuffer buffer(&corrupted);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "avif");
        Contract::configure(reader, policy_->maximumImagePixels());
        const QImage result = reader.read();
        // A mutation can land in ignorable metadata; if it remains valid it
        // still must respect the selected policy and declared dimensions.
        QVERIFY(result.isNull() || result.size() == QSize(640, 360));
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
    AvifPluginTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_avif_plugin.moc"
