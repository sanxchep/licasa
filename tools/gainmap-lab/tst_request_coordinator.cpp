#include "request_coordinator.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace Licasa;
using namespace Licasa::GainmapLab;

class CoordinatorTests : public QObject {
    Q_OBJECT
private:
    ExternalFileIdentity identity(const QString& path = QStringLiteral(GAINMAP_FIXTURE))
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) qFatal("Fixture unavailable");
        return externalFileIdentity(file).value();
    }
    std::optional<Result> run(const RequestCoordinator::Request& request)
    {
        QString error;
        return decode(request.path, request.identity, request.headroom, error,
                      request.limits, request.cancelled.get());
    }
private slots:
    void decodeAndHeadroom()
    {
        RequestCoordinator coordinator;
        const auto low = coordinator.request(GAINMAP_FIXTURE, identity(), 1);
        QVERIFY(low);
        QVERIFY(coordinator.decodeBytes() > 0);
        QVERIFY(coordinator.complete(*low, run(*low)));
        QVERIFY(!coordinator.image().isNull());
        const QImage lowImage = coordinator.image().copy();
        QVERIFY(coordinator.beginUpload());
        QCOMPARE(coordinator.uploadBytes(), coordinator.retainedBytes());
        QVERIFY(!coordinator.beginUpload());
        coordinator.endUpload();
        const auto high = coordinator.request(GAINMAP_FIXTURE, identity(), 4);
        QVERIFY(high);
        QVERIFY(coordinator.image().isNull());
        QVERIFY(coordinator.complete(*high, run(*high)));
        QVERIFY(coordinator.image() != lowImage);
        coordinator.cancel();
        QCOMPARE(coordinator.retainedBytes(), quint64(0));
        QCOMPARE(coordinator.uploadBytes(), quint64(0));
    }
    void rapidReplacementDrains()
    {
        RequestCoordinator coordinator;
        const auto first = coordinator.request(GAINMAP_FIXTURE, identity(), 1);
        QVERIFY(first);
        auto finished = run(*first); // Work finished but callback has not arrived.
        const auto reserved = coordinator.decodeBytes();
        for (int i = 0; i < 100; ++i) {
            QVERIFY(!coordinator.request(GAINMAP_FIXTURE, identity(), 2));
            QCOMPARE(coordinator.decodeBytes(), reserved);
        }
        QVERIFY(first->cancelled->load());
        QVERIFY(!coordinator.complete(*first, std::move(finished)));
        QCOMPARE(coordinator.decodeBytes(), quint64(0));
        const auto latest = coordinator.request(GAINMAP_FIXTURE, identity(), 4);
        QVERIFY(latest);
        QVERIFY(!coordinator.complete(*first, std::nullopt));
        QVERIFY(coordinator.decodeBytes() > 0);
        QVERIFY(coordinator.complete(*latest, run(*latest)));
    }
    void cancellation()
    {
        RequestCoordinator coordinator;
        const auto request = coordinator.request(GAINMAP_FIXTURE, identity(), 2);
        QVERIFY(request);
        coordinator.cancel();
        QVERIFY(coordinator.decodeBytes() > 0);
        QVERIFY(!run(*request));
        QVERIFY(!coordinator.complete(*request, std::nullopt));
        QCOMPARE(coordinator.decodeBytes(), quint64(0));
        QVERIFY(coordinator.image().isNull());
    }
    void staleSource()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("source.jpg");
        QVERIFY(QFile::copy(GAINMAP_FIXTURE, path));
        const auto original = identity(path);
        RequestCoordinator coordinator;
        const auto request = coordinator.request(path, original, 2);
        QVERIFY(request);
        auto result = run(*request);
        QVERIFY(result);
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::copy(GAINMAP_FIXTURE, path));
        QVERIFY(!coordinator.complete(*request, std::move(result)));
        QVERIFY(!coordinator.request(path, original, 2));
        QVERIFY(coordinator.image().isNull());
    }
    void budgets_data()
    {
        QTest::addColumn<int>("pool");
        QTest::addColumn<bool>("enough");
        for (int pool = 0; pool < 2; ++pool) {
            QTest::newRow(qPrintable(QString("pool-%1-exact").arg(pool))) << pool << true;
            QTest::newRow(qPrintable(QString("pool-%1-short").arg(pool))) << pool << false;
        }
    }
    void budgets()
    {
        QFETCH(int, pool);
        QFETCH(bool, enough);
        Limits limits;
        limits.rasterPixels = 1088;
        limits.compressedBytes = 16384;
        RequestCoordinator::Budgets budgets{1088 * 64 + 16384 * 2, 1088 * 8, 1088 * 8};
        if (!enough) {
            if (pool == 0) --budgets.decode;
            if (pool == 1) --budgets.retained;
            if (pool == 2) --budgets.upload;
        }
        RequestCoordinator coordinator(budgets);
        const auto request = coordinator.request(GAINMAP_FIXTURE, identity(), 2, limits);
        QCOMPARE(bool(request), enough);
        if (request) QVERIFY(coordinator.complete(*request, run(*request)));
        else {
            QVERIFY(coordinator.image().isNull());
            QVERIFY(!coordinator.beginUpload());
            QCOMPARE(coordinator.decodeBytes(), quint64(0));
        }
    }
    void uploadBoundary_data()
    {
        QTest::addColumn<bool>("enough");
        QTest::newRow("exact") << true;
        QTest::newRow("short") << false;
    }
    void uploadBoundary()
    {
        QFETCH(bool, enough);
        RequestCoordinator::Budgets budgets;
        budgets.upload = 8192 - (enough ? 0 : 1);
        RequestCoordinator coordinator(budgets);
        auto request = coordinator.request(GAINMAP_FIXTURE, identity(), 2);
        QVERIFY(request);
        QVERIFY(coordinator.complete(*request, run(*request)));
        QCOMPARE(coordinator.beginUpload(), enough);
    }
    void closeCancelsWorker()
    {
        std::optional<RequestCoordinator::Request> work;
        {
            RequestCoordinator coordinator;
            work = coordinator.request(GAINMAP_FIXTURE, identity(), 2);
            QVERIFY(work);
        }
        QVERIFY(work->cancelled->load());
        QVERIFY(!run(*work));
    }
    void oversizedResult()
    {
        RequestCoordinator coordinator;
        Limits limits;
        limits.rasterPixels = 1088;
        const auto request = coordinator.request(GAINMAP_FIXTURE, identity(), 2, limits);
        QVERIFY(request);
        Result result{QImage(64, 64, QImage::Format_RGBA16FPx4), {}, 0};
        QVERIFY(!coordinator.complete(*request, std::move(result)));
        QVERIFY(coordinator.image().isNull());
        QCOMPARE(coordinator.decodeBytes(), quint64(0));
    }
    void failedDecodeAndInvalidLimits()
    {
        RequestCoordinator coordinator;
        const auto request = coordinator.request(GAINMAP_FIXTURE, identity(), 2);
        QVERIFY(request);
        QVERIFY(!coordinator.complete(*request, std::nullopt));
        QCOMPARE(coordinator.decodeBytes(), quint64(0));
        Limits limits;
        limits.rasterPixels = std::numeric_limits<quint64>::max();
        QVERIFY(!coordinator.request(GAINMAP_FIXTURE, identity(), 2, limits));
        QVERIFY(!coordinator.request(GAINMAP_FIXTURE, identity(), qQNaN()));
        QVERIFY(coordinator.image().isNull());
    }
};
QTEST_GUILESS_MAIN(CoordinatorTests)
#include "tst_request_coordinator.moc"
