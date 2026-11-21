#include "async_decoder.h"
#include "../hdr-probe/gainmap_photo.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QBuffer>
#include <QImageReader>
#include <QRegularExpression>
#include <QtCore/qfloat16.h>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

using namespace Licasa;
using namespace Licasa::GainmapLab;
namespace {
ExternalFileIdentity identity(const QString& path = QStringLiteral(GAINMAP_FIXTURE))
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Missing fixture");
    return externalFileIdentity(file).value();
}
struct Gate {
    QSemaphore release;
    std::atomic_bool entered{false};
    std::atomic_int calls{0};
    std::atomic_bool offOwner{false};
};
enum class DecodeBarrier { BeforeProbe, AfterProbe, AfterDecode };

AsyncDecoder::Decode gated(const std::shared_ptr<Gate>& gate,
                           DecodeBarrier barrier = DecodeBarrier::AfterDecode)
{
    QThread* owner = QThread::currentThread();
    return [gate, owner, barrier](const RequestCoordinator::Request& request, QString& error) {
        gate->offOwner = QThread::currentThread() != owner;
        const auto waitForReplacement = [&] {
            if (++gate->calls != 1) return true;
            gate->entered = true;
            return gate->release.tryAcquire(1, 5000);
        };
        if (barrier == DecodeBarrier::BeforeProbe && !waitForReplacement())
            return std::optional<Result>();
        const auto reserveRaster = [&](quint64 pixels) {
            return barrier != DecodeBarrier::AfterProbe
                || (request.reserveRaster(pixels) && waitForReplacement());
        };
        auto result = decode(request.path, request.identity, request.headroom, error,
                             request.limits, request.cancelled.get(), nullptr, reserveRaster);
        if (barrier == DecodeBarrier::AfterDecode && !waitForReplacement())
            return std::optional<Result>();
        return result;
    };
}
}
class AsyncTests : public QObject {
    Q_OBJECT
private slots:
    void deliveryAndUpload()
    {
        AsyncDecoder worker;
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        bool onOwner = false;
        connect(&worker, &AsyncDecoder::settled, &worker, [&] {
            onOwner = QThread::currentThread() == worker.thread();
        });
        const auto serial = worker.submit(GAINMAP_FIXTURE, identity(), 4);
        QCOMPARE(settled.count(), 0); // Delivery is queued even for rejection.
        QTRY_COMPARE(settled.count(), 1);
        QCOMPARE(settled[0][0].toULongLong(), serial);
        QVERIFY(settled[0][1].toBool());
        QVERIFY(onOwner);
        QVERIFY(!worker.busy());
        QCOMPARE(worker.coordinator().decodeBytes(), quint64(0));
        QVERIFY(!worker.withUpload([](QImage&) { return false; }));
        QCOMPARE(worker.coordinator().uploadBytes(), quint64(0));
        const uchar* storage = worker.coordinator().image().constBits();
        QVERIFY(worker.withUpload([&](QImage& image) {
            QString primaries, error;
            if (!GainmapPhotoProbe::prepareUpload(image, primaries, error)) return false;
            return primaries == "srgb" && image.constBits() == storage &&
                   worker.coordinator().uploadBytes() == quint64(image.sizeInBytes());
        }));
        QCOMPARE(worker.coordinator().uploadBytes(), quint64(0));
        worker.cancel();
        QVERIFY(worker.coordinator().image().isNull());
    }
    void rapidNavigationAndHeadroom()
    {
        auto gate = std::make_shared<Gate>();
        AsyncDecoder worker({}, gated(gate));
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        worker.submit(GAINMAP_FIXTURE, identity(), 1);
        QTRY_VERIFY(gate->entered.load());
        QVERIFY(gate->offOwner.load());
        const auto reservation = worker.coordinator().decodeBytes();
        quint64 latest = 0;
        for (int i = 0; i < 100; ++i) latest = worker.submit(GAINMAP_FIXTURE, identity(), 4);
        QCOMPARE(worker.coordinator().decodeBytes(), reservation);
        QCOMPARE(gate->calls.load(), 1);
        QVERIFY(worker.coordinator().image().isNull());
        gate->release.release();
        QTRY_COMPARE(settled.count(), 1);
        QCOMPARE(settled[0][0].toULongLong(), latest);
        QVERIFY(settled[0][1].toBool());
        QCOMPARE(gate->calls.load(), 2);
        QString error;
        auto expected = decode(GAINMAP_FIXTURE, identity(), 4, error);
        QVERIFY(expected);
        QCOMPARE(worker.coordinator().image(), expected->linear);
    }
    void cancelWhileRunning()
    {
        auto gate = std::make_shared<Gate>();
        AsyncDecoder worker({}, gated(gate));
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        worker.submit(GAINMAP_FIXTURE, identity(), 2);
        QTRY_VERIFY(gate->entered.load());
        worker.submit(GAINMAP_FIXTURE, identity(), 4); // Also cancel pending intent.
        worker.cancel();
        QVERIFY(worker.coordinator().decodeBytes() > 0);
        gate->release.release();
        QTRY_VERIFY(!worker.busy());
        QCoreApplication::processEvents();
        QCOMPARE(settled.count(), 0);
        QCOMPARE(gate->calls.load(), 1);
        QCOMPARE(worker.coordinator().decodeBytes(), quint64(0));
        QVERIFY(worker.coordinator().image().isNull());
        QVERIFY(!worker.withUpload([](QImage&) { return true; }));
    }
    void staleAtDelivery()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("photo.jpg");
        QVERIFY(QFile::copy(GAINMAP_FIXTURE, path));
        auto gate = std::make_shared<Gate>();
        AsyncDecoder worker({}, gated(gate));
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        worker.submit(path, identity(path), 2);
        QTRY_VERIFY(gate->entered.load());
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::copy(GAINMAP_FIXTURE, path));
        gate->release.release();
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(!settled[0][1].toBool());
        QVERIFY(worker.coordinator().image().isNull());
        QCOMPARE(worker.coordinator().decodeBytes(), quint64(0));
    }
    void replaceGainMap_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<int>("barrier");
        QTest::addColumn<QString>("failure");
        for (const auto& operation : {"atomic", "same-inode", "symlink-retarget"}) {
            const QByteArray name(operation);
            QTest::newRow((name + "-before-probe").constData())
                << QString::fromLatin1(operation) << int(DecodeBarrier::BeforeProbe)
                << QStringLiteral("JPEG source changed or is unavailable.");
            QTest::newRow((name + "-after-probe").constData())
                << QString::fromLatin1(operation) << int(DecodeBarrier::AfterProbe)
                << QStringLiteral("JPEG source changed during reconstruction.");
            QTest::newRow((name + "-after-decode").constData())
                << QString::fromLatin1(operation) << int(DecodeBarrier::AfterDecode)
                << QStringLiteral("Gain-map result became stale or invalid.");
        }
    }
    void replaceGainMap()
    {
        QFETCH(QString, operation);
        QFETCH(int, barrier);
        QFETCH(QString, failure);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile first(GAINMAP_FIXTURE);
        QFile second(QFileInfo(first).dir().filePath("jpeg-gainmap-p3-red.jpg"));
        QVERIFY(first.open(QIODevice::ReadOnly));
        QVERIFY(second.open(QIODevice::ReadOnly));
        auto original = first.readAll();
        auto replacement = second.readAll();
        const auto size = qMax(original.size(), replacement.size());
        original.append(QByteArray(size - original.size(), '\0'));
        replacement.append(QByteArray(size - replacement.size(), '\0'));
        const auto path = directory.filePath("photo.jpg");
        const auto initialPath = operation == "symlink-retarget" ? directory.filePath("initial.jpg") : path;
        const auto newPath = operation == "symlink-retarget" ? directory.filePath("alternate.jpg") : path;
        const auto timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
        const auto write = [&](const QString& target, const QByteArray& bytes) {
            QFile file(target);
            return file.open(QIODevice::ReadWrite | QIODevice::Truncate)
                && file.write(bytes) == bytes.size() && file.flush()
                && file.setFileTime(timestamp, QFileDevice::FileModificationTime);
        };
        QVERIFY(write(initialPath, original));
        if (operation == "symlink-retarget") QVERIFY(QFile::link(initialPath, path));
        const auto before = identity(path);
        QString error;
        const auto firstRaster = decode(path, before, 4, error);
        QVERIFY2(firstRaster, qPrintable(error));
        auto gate = std::make_shared<Gate>();
        AsyncDecoder worker({}, gated(gate, DecodeBarrier(barrier)));
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        const auto replaced = worker.submit(path, before, 4);
        QTRY_VERIFY(gate->entered.load());

        if (operation == "atomic") {
            QSaveFile temp(path);
            QVERIFY(temp.open(QIODevice::WriteOnly));
            QCOMPARE(temp.write(replacement), replacement.size());
            QVERIFY(temp.commit());
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(timestamp, QFileDevice::FileModificationTime));
        } else {
            QVERIFY(write(newPath, replacement));
            if (operation == "symlink-retarget") {
                QVERIFY(QFile::remove(path));
                QVERIFY(QFile::link(newPath, path));
            }
        }
        const auto after = identity(path);
        QVERIFY(before != after);
        QCOMPARE(before.size, after.size);
        QCOMPARE(QFileInfo(path).lastModified(), timestamp);
        gate->release.release();
        QTRY_COMPARE(settled.count(), 1);
        QCOMPARE(settled[0][0].toULongLong(), replaced);
        QVERIFY(!settled[0][1].toBool());
        QCOMPARE(settled[0][2].toString(), failure);
        QVERIFY(worker.coordinator().image().isNull());
        QCOMPARE(worker.coordinator().decodeBytes(), quint64(0));

        const auto expected = decode(newPath, after, 4, error);
        QVERIFY2(expected, qPrintable(error));
        QVERIFY(firstRaster->linear.pixelColor(16, 16) != expected->linear.pixelColor(16, 16));
        const auto latest = worker.submit(path, after, 4);
        QTRY_COMPARE(settled.count(), 2);
        QCOMPARE(settled[1][0].toULongLong(), latest);
        QVERIFY2(settled[1][1].toBool(), qPrintable(settled[1][2].toString()));
        QCOMPARE(worker.coordinator().image(), expected->linear);
    }
    void staleBeforeUpload()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("photo.jpg");
        QVERIFY(QFile::copy(GAINMAP_FIXTURE, path));
        AsyncDecoder worker;
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        worker.submit(path, identity(path), 2);
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(settled[0][1].toBool());
        QVERIFY(QFile::remove(path));
        QVERIFY(!worker.withUpload([](QImage&) { return true; }));
        QVERIFY(worker.coordinator().image().isNull());
        QCOMPARE(worker.coordinator().uploadBytes(), quint64(0));
    }
    void decodeFailureAndRecovery()
    {
        AsyncDecoder worker;
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        Limits limits;
        limits.rasterPixels = 1; // Admitted worker, rejected by actual decoder.
        worker.submit(GAINMAP_FIXTURE, identity(), 2, limits);
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(!settled[0][1].toBool());
        // The photo is larger than its own limit. Waiting cannot change that.
        QCOMPARE(settled[0][3].value<DecodeFailure>(), DecodeFailure::Unavailable);
        QCOMPARE(worker.coordinator().decodeBytes(), quint64(0));
        worker.submit(GAINMAP_FIXTURE, identity(), 4);
        QTRY_COMPARE(settled.count(), 2);
        QVERIFY(settled[1][1].toBool());
    }
    void rasterGrowthKeepsItsReservation()
    {
        Limits limits;
        limits.rasterPixels = maximumRasterPixels;
        const auto initial = RequestCoordinator::decodeAllowance({});
        const quint64 total = initial.decode + initial.retained + 72;
        auto pool = std::make_shared<ResourceBudget>(RequestCoordinator::decodeAllowance(limits), 0, total);
        RequestCoordinator coordinator(pool);
        auto request = coordinator.request(GAINMAP_FIXTURE, identity(), 4, limits, true);
        QVERIFY(request);
        QCOMPARE(coordinator.decodeBytes(), initial.decode);
        auto peer = pool->acquire({1, 0, 0});
        QVERIFY(peer);
        QVERIFY(!request->reserveRaster(Limits{}.rasterPixels + 1));
        QCOMPARE(pool->used().decode, initial.decode + 1);
        QCOMPARE(pool->used().retained, initial.retained);
        peer.reset();
        QVERIFY(request->reserveRaster(Limits{}.rasterPixels + 1));
        QCOMPARE(coordinator.decodeBytes(), initial.decode + 64);
        QCOMPARE(pool->used().retained, initial.retained + 8);
        coordinator.cancel();
        QVERIFY(!request->reserveRaster(Limits{}.rasterPixels + 2));
        QCOMPARE(pool->used().decode, initial.decode + 64);
        QVERIFY(!coordinator.complete(*request, std::nullopt));
        QCOMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, quint64(0));
    }
    void twoRequestsContendWhileTheLimitShrinks()
    {
        Limits limits;
        limits.rasterPixels = maximumRasterPixels;
        const auto initial = RequestCoordinator::decodeAllowance({});
        constexpr quint64 growth = 1024 * 1024;
        const auto caps = RequestCoordinator::decodeAllowance(limits);
        const quint64 twoInitial = 2 * (initial.decode + initial.retained);
        const quint64 oneGrowth = 72 * growth;
        auto pool = std::make_shared<ResourceBudget>(caps, 0, twoInitial + oneGrowth);
        RequestCoordinator first(pool), second(pool);
        const auto source = identity();
        auto a = first.request(GAINMAP_FIXTURE, source, 2, limits, true);
        auto b = second.request(GAINMAP_FIXTURE, source, 4, limits, true);
        QVERIFY(a && b);
        const auto pixels = Limits{}.rasterPixels + growth;
        pool->setLimits(caps, 0, twoInitial + oneGrowth - 1);
        QVERIFY(!a->reserveRaster(pixels));
        QCOMPARE(pool->used().decode, 2 * initial.decode);
        pool->setLimits(caps, 0, twoInitial + oneGrowth);
        QVERIFY(a->reserveRaster(pixels));
        QVERIFY(!b->reserveRaster(pixels));
        first.cancel();
        QVERIFY(!b->reserveRaster(pixels)); // The first worker still owns its allowance.
        QVERIFY(!first.complete(*a, std::nullopt));
        QVERIFY(b->reserveRaster(pixels));
        QCOMPARE(pool->used().decode, initial.decode + 64 * growth);
        QCOMPARE(pool->used().retained, initial.retained + 8 * growth);
        QVERIFY(!second.complete(*b, std::nullopt));
        QCOMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, quint64(0));
    }
    void cameraSizedDecode_data()
    {
        QTest::addColumn<QString>("fixtureVariable");
        QTest::newRow("pixel6") << QStringLiteral("LICASA_HDR_PIXEL6_FIXTURE");
        QTest::newRow("pixel9") << QStringLiteral("LICASA_HDR_COMPOUND_CAMERA_FIXTURE");
    }
    void cameraSizedDecode()
    {
        QFETCH(QString, fixtureVariable);
        const auto path = qEnvironmentVariable(fixtureVariable.toLatin1().constData());
        if (path.isEmpty()) QSKIP("The retained camera fixture is optional.");
        const auto source = identity(path);
        QString error;
        QVERIFY(!decode(path, source, 4, error));
        QCOMPARE(error, QStringLiteral("JPEG exceeds the gain-map raster budget."));

        Limits limits;
        limits.rasterPixels = maximumRasterPixels;
        const auto reference = decode(path, source, 4, error, limits);
        QVERIFY2(reference, qPrintable(error));
        const quint64 pixels = quint64(reference->linear.width()) * reference->linear.height()
            + quint64(reference->gainSize.width()) * reference->gainSize.height();
        const auto exact = RequestCoordinator::decodeAllowance({limits.compressedBytes, pixels});
        auto caps = RequestCoordinator::decodeAllowance(limits);
        caps.upload = caps.retained;
        const quint64 total = exact.decode + exact.retained;
        auto pool = std::make_shared<ResourceBudget>(caps, caps.upload, total - 1);
        AsyncDecoder worker({}, {}, nullptr, pool);
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        worker.submit(path, source, 4, limits);
        QTRY_COMPARE_WITH_TIMEOUT(settled.count(), 1, 12000);
        QVERIFY(!settled[0][1].toBool());
        QCOMPARE(settled[0][2].toString(), QStringLiteral("JPEG gain-map decode exceeds the shared memory budget."));
        // A caller may retry this refusal. No other failure reports it.
        QCOMPARE(settled[0][3].value<DecodeFailure>(), DecodeFailure::Memory);
        QCOMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, quint64(0));

        pool->setLimits(caps, caps.upload, total);
        worker.submit(path, source, 4, limits);
        QTRY_COMPARE_WITH_TIMEOUT(settled.count(), 2, 12000);
        QVERIFY2(settled[1][1].toBool(), qPrintable(settled[1][2].toString()));
        QVERIFY(worker.coordinator().image() == reference->linear);
        QCOMPARE(worker.gainSize(), reference->gainSize);
        QCOMPARE(worker.jpegBytes(), reference->jpegBytes);
        QCOMPARE(pool->used().decode, quint64(0));
        const auto retained = quint64(reference->linear.sizeInBytes());
        QCOMPARE(pool->used().retained, retained);
        QVERIFY(worker.withUpload([&](QImage&) { return pool->used().upload == retained; }));
        QCOMPARE(pool->used().upload, quint64(0));
        worker.cancel();
        QCOMPARE(pool->used().retained, quint64(0));
        qInfo() << "Camera primary/map:" << reference->linear.size() << reference->gainSize
                << "decode/retained allowance:" << exact.decode << exact.retained
                << "retained raster:" << retained;
    }
    void cameraIndependentReconstruction_data()
    {
        QTest::addColumn<QString>("fixtureVariable");
        QTest::addColumn<float>("headroom");
        for (const char* camera : {"pixel6", "pixel9"})
            for (const float headroom : {1.0f, 1.5f, 2.0f, 4.0f, 8.0f})
                QTest::addRow("%s-%g", camera, double(headroom))
                    << QString::fromLatin1(qstrcmp(camera, "pixel6") ? "LICASA_HDR_COMPOUND_CAMERA_FIXTURE"
                                                                     : "LICASA_HDR_PIXEL6_FIXTURE")
                    << headroom;
    }
    void cameraIndependentReconstruction()
    {
        // Camera pixels from the lab against a reference that shares none of
        // its code: Qt's JPEG reader decodes both images, the gain-map values
        // come from the map's own XMP, and the Ultra HDR v1 formula is written
        // out here. Samples sit where the map and the base are locally flat, so
        // map upsampling, chroma upsampling and IDCT choices cannot matter.
        QFETCH(QString, fixtureVariable);
        QFETCH(float, headroom);
        const auto path = qEnvironmentVariable(fixtureVariable.toLatin1().constData());
        if (path.isEmpty()) QSKIP("The retained camera fixture is optional.");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        // The gain map is the JPEG holding the second hdrgm block. The MPF
        // index is not used, since the lab already reconciles it.
        const auto first = bytes.indexOf("hdrgm:Version=");
        const auto block = bytes.lastIndexOf("hdrgm:Version=");
        QVERIFY(first >= 0 && block > first);
        const auto start = bytes.lastIndexOf(QByteArray("\xff\xd8\xff", 3), block);
        QVERIFY(start > first);
        const QByteArray xmp = bytes.mid(block, 4096);
        const auto field = [&](const char* name, std::optional<double> fallback) -> std::optional<double> {
            const QRegularExpression pattern(QStringLiteral("hdrgm:%1=\"([-+0-9.eE]+)\"").arg(QLatin1String(name)));
            const auto match = pattern.match(QString::fromLatin1(xmp));
            return match.hasMatch() ? std::optional<double>(match.captured(1).toDouble()) : fallback;
        };
        const auto mapMin = field("GainMapMin", 0.0), mapMax = field("GainMapMax", std::nullopt);
        const auto gamma = field("Gamma", 1.0), offsetSdr = field("OffsetSDR", 1.0 / 64);
        const auto offsetHdr = field("OffsetHDR", 1.0 / 64), capacityMin = field("HDRCapacityMin", 0.0);
        const auto capacityMax = field("HDRCapacityMax", std::nullopt);
        QVERIFY(mapMax && capacityMax && *capacityMax > *capacityMin && *gamma > 0);
        QVERIFY(!xmp.contains("hdrgm:BaseRenditionIsHDR=\"True\""));

        QImageReader baseReader(path, "jpeg");
        baseReader.setAutoTransform(false);
        QCOMPARE(baseReader.transformation(), QImageIOHandler::TransformationNone);
        const QImage base = baseReader.read().convertToFormat(QImage::Format_RGB888);
        QBuffer mapBytes;
        mapBytes.setData(bytes.mid(start));
        QVERIFY(mapBytes.open(QIODevice::ReadOnly));
        QImageReader mapReader(&mapBytes, "jpeg");
        const QImage map = mapReader.read();
        QVERIFY(!base.isNull() && !map.isNull());
        QVERIFY2(map.isGrayscale(), "Per-channel camera gain maps are not covered by this reference");
        const QImage gain = map.convertToFormat(QImage::Format_Grayscale8);

        Limits limits;
        limits.rasterPixels = maximumRasterPixels;
        QString error;
        const auto decoded = decode(path, identity(path), headroom, error, limits);
        QVERIFY2(decoded, qPrintable(error));
        QCOMPARE(decoded->linear.size(), base.size());
        QCOMPARE(decoded->gainSize, gain.size());

        const double weight = std::clamp((std::log2(double(headroom)) - *capacityMin) / (*capacityMax - *capacityMin), 0.0, 1.0);
        const auto eotf = [](double code) {
            const double v = code / 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        const auto flatGain = [&](int mx, int my) {
            const int value = qGray(gain.pixel(mx, my));
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if (qGray(gain.pixel(mx + dx, my + dy)) != value) return -1;
            return value;
        };
        const auto flatBase = [&](int x, int y) {
            for (int c = 0; c < 3; ++c) {
                int low = 255, high = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    const uchar* row = base.constScanLine(y + dy);
                    for (int dx = -2; dx <= 2; ++dx) {
                        low = std::min<int>(low, row[(x + dx) * 3 + c]);
                        high = std::max<int>(high, row[(x + dx) * 3 + c]);
                    }
                }
                if (high - low > 4) return false;
            }
            return true;
        };
        int samples = 0, chromatic = 0, boosted = 0;
        double worst = 0, worstBoosted = 0, wrongBoosted = 0;
        QString worstAt;
        // Every map pixel is a candidate; most are rejected by flatness.
        for (int my = 2; my < gain.height() - 2; ++my) {
            for (int mx = 2; mx < gain.width() - 2; ++mx) {
                const int code = flatGain(mx, my);
                const int x = int((mx + 0.5) * base.width() / gain.width());
                const int y = int((my + 0.5) * base.height() / gain.height());
                if (code < 0 || x < 2 || y < 2 || x >= base.width() - 2 || y >= base.height() - 2 || !flatBase(x, y))
                    continue;
                const double recovery = std::pow(code / 255.0, 1.0 / *gamma);
                const double logBoost = *mapMin * (1 - recovery) + *mapMax * recovery;
                const double factor = std::exp2(logBoost * weight);
                // A tenth less of the boost exponent: what the comparison must reject.
                const double wrongFactor = std::exp2(logBoost * weight * 0.9);
                const uchar* pixel = base.constScanLine(y) + x * 3;
                qfloat16 actual[4];
                std::memcpy(actual, decoded->linear.constScanLine(y) + x * 8, 8);
                for (int c = 0; c < 3; ++c) {
                    const double sdr = eotf(pixel[c]) + *offsetSdr;
                    const double expected = sdr * factor - *offsetHdr;
                    // Two codes of JPEG decoding difference in the base, one
                    // code in the map, and half-float rounding.
                    const double allowed = factor * (eotf(std::min(255, pixel[c] + 2)) - eotf(pixel[c]))
                        + 0.006 * std::abs(expected) + 0.002;
                    const double ratio = std::abs(double(actual[c]) - expected) / allowed;
                    if (ratio > worst) {
                        worst = ratio;
                        worstAt = QStringLiteral("(%1,%2) c%3 map %4 base %5: %6 vs %7")
                            .arg(x).arg(y).arg(c).arg(code).arg(pixel[c]).arg(double(actual[c])).arg(expected);
                    }
                    if (factor > 1.05) {
                        worstBoosted = std::max(worstBoosted, ratio);
                        wrongBoosted = std::max(wrongBoosted,
                                                std::abs(double(actual[c]) - (sdr * wrongFactor - *offsetHdr)) / allowed);
                    }
                }
                ++samples;
                chromatic += std::max({pixel[0], pixel[1], pixel[2]}) - std::min({pixel[0], pixel[1], pixel[2]}) > 24;
                boosted += factor > 1.05;
            }
        }
        qInfo().noquote() << QStringLiteral("%1 samples, %2 chromatic, %3 boosted; weight %4; worst %5 of tolerance "
                                            "(boosted %6, wrong exponent %7) at %8")
            .arg(samples).arg(chromatic).arg(boosted).arg(weight)
            .arg(worst, 0, 'f', 3).arg(worstBoosted, 0, 'f', 3).arg(wrongBoosted, 0, 'f', 2).arg(worstAt);
        QVERIFY2(samples >= 1000, "Too few locally flat samples to compare");
        QVERIFY2(chromatic >= 100, "Too few coloured samples to compare");
        QVERIFY2(worst <= 1.0, qPrintable(worstAt));
        if (weight > 0) {
            QVERIFY2(boosted >= 100, "Too few samples above SDR white");
            QVERIFY2(wrongBoosted > 2.0, "The comparison cannot tell a 10% error in the boost exponent");
        }
    }
    void closeWhileRunning()
    {
        auto gate = std::make_shared<Gate>();
        auto* worker = new AsyncDecoder({}, gated(gate));
        int callbacks = 0;
        connect(worker, &AsyncDecoder::settled, this, [&] { ++callbacks; });
        worker->submit(GAINMAP_FIXTURE, identity(), 4);
        QTRY_VERIFY(gate->entered.load());
        delete worker; // Must neither wait for the gate nor deliver a callback.
        gate->release.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        QCOMPARE(callbacks, 0);
    }
    void workerException()
    {
        AsyncDecoder worker({}, [](const RequestCoordinator::Request&, QString&) -> std::optional<Result> {
            throw std::runtime_error("test worker exception");
        });
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        worker.submit(GAINMAP_FIXTURE, identity(), 2);
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(!settled[0][1].toBool());
        QVERIFY(!settled[0][2].toString().isEmpty());
        QVERIFY(!worker.busy());
        QCOMPARE(worker.coordinator().decodeBytes(), quint64(0));
    }
    void rejectionAndRecovery()
    {
        RequestCoordinator::Budgets budgets;
        budgets.decode = 0;
        AsyncDecoder worker(budgets);
        QSignalSpy settled(&worker, &AsyncDecoder::settled);
        worker.submit(GAINMAP_FIXTURE, identity(), 2);
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(!settled[0][1].toBool());
        QVERIFY(!worker.busy());
        QVERIFY(worker.coordinator().image().isNull());
        AsyncDecoder normal;
        QSignalSpy completed(&normal, &AsyncDecoder::settled);
        normal.submit(GAINMAP_FIXTURE, identity(), qQNaN());
        QTRY_COMPARE(completed.count(), 1);
        QVERIFY(!completed[0][1].toBool());
        normal.submit(GAINMAP_FIXTURE, identity(), 2);
        QTRY_COMPARE(completed.count(), 2);
        QVERIFY(completed[1][1].toBool());
    }
};
QTEST_GUILESS_MAIN(AsyncTests)
#include "tst_async_decoder.moc"
