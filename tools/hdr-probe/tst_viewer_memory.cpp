// Memory pressure through the real HDR item and renderer: camera-sized photos
// contending for the shared pool, limit changes at each stage of becoming HDR,
// and faults on the render thread. Kept apart from tst_viewer_hdr.cpp because
// these rows decode 16 MP photos and run for about a minute per render loop.
#include "../../src/imaging/hdr/hdr_image_item.h"
#include "../../src/imaging/hdr/render_target_budget.h"
#include "../../src/imaging/hdr/viewer_memory_policy.h"
#include "../../src/io/external_file_identity.h"
#include "../gainmap-lab/test_gainmap_fixture.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QVulkanInstance>
#include <QtTest>

namespace {
// Two camera-sized gain-map photos made by the lab codec, so that the memory
// tests need no private fixture. Either fits the shared pool; both do not.
struct LargePhotos {
    static constexpr quint64 warmBytes = quint64(4096) * 4094 * 8, coolBytes = quint64(4096) * 3000 * 8;
    QString warm, cool, error;
    bool create()
    {
        static QTemporaryDir directory;
        static QString failure;
        const QString first = directory.filePath("warm.jpg"), second = directory.filePath("cool.jpg");
        const auto encode = [](const QString& path, QSize size, int gainScale, bool coolRamp) {
            if (QFile::exists(path)) return true;
            const auto bytes = Licasa::GainmapLab::TestFixture::encodeGainMapJpeg(size, gainScale, coolRamp, &failure);
            QSaveFile file(path);
            return !bytes.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        };
        if (!directory.isValid() || !encode(first, QSize(4096, 4094), 128, false)
            || !encode(second, QSize(4096, 3000), 4, true)) {
            error = failure.isEmpty() ? QStringLiteral("Unable to write the generated photos.") : failure;
            return false;
        }
        warm = first;
        cool = second;
        return true;
    }
};

}

class ViewerMemoryTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { Licasa::configureHdrMemoryBudget(8192); }
    void largePhotoNavigationWaitsForRetirement()
    {
        // Two camera-sized photos cannot be retained together. The second
        // one's decode starts while the first texture still waits for its fence.
        using namespace Licasa;
        LargePhotos photos;
        QVERIFY2(photos.create(), qPrintable(photos.error));
        const QString warm = photos.warm, cool = photos.cool;
        constexpr quint64 warmBytes = LargePhotos::warmBytes, coolBytes = LargePhotos::coolBytes;
        const auto pool = viewerResourceBudget();
        QVERIFY(warmBytes + coolBytes > pool->limits().retained);
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        const QColor fallback(19, 59, 29);
        QQuickWindow window;
        window.setVulkanInstance(&instance);
        window.resize(128, 96);
        window.setColor(fallback);
        QVERIFY(reserveHdrTargets(&window, QSize(128, 96), 1));
        const auto targets = pool->used().targets;
        auto item = std::make_unique<HdrImageItem>(window.contentItem());
        item->setSize(QSizeF(128, 96));
        item->setTestHeadroom(4);
        item->setSource(QUrl::fromLocalFile(warm));
        window.show();
        // The item restarts its work when it first learns the output is ready.
        QTRY_VERIFY(item->outputReady());
        item->setActive(true);
        const auto center = [&] {
            const auto frame = window.grabWindow();
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };
        QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), HdrImageItem::PhotoStatus::Hdr, 30000);
        QCOMPARE(pool->used().retained, warmBytes);
        const auto warmPixel = center();
        QVERIFY(warmPixel.isValid() && warmPixel != fallback);

        for (int round = 0; round < 2; ++round) {
            item->setSource(QUrl::fromLocalFile(cool));
            // The outgoing photo may be drawn until the scene graph synchronizes.
            QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), HdrImageItem::PhotoStatus::Hdr, 30000);
            QCOMPARE(pool->used().retained, coolBytes);
            QCOMPARE(pool->used().upload, coolBytes);
            const auto coolPixel = center();
            QVERIFY(coolPixel.isValid() && coolPixel != fallback && coolPixel != warmPixel);
            item->setSource(QUrl::fromLocalFile(warm));
            QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), HdrImageItem::PhotoStatus::Hdr, 30000);
            QCOMPARE(pool->used().retained, warmBytes);
            QCOMPARE(pool->used().upload, warmBytes);
            QCOMPARE(center(), warmPixel);
        }
        item.reset();
        window.releaseResources();
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().targets, targets);
    }
    void largePhotosContendAcrossWindows()
    {
        // The shared pool cannot retain both camera-sized photos. A second
        // window is refused, keeps SDR and leaves the resident photo untouched.
        using namespace Licasa;
        LargePhotos photos;
        QVERIFY2(photos.create(), qPrintable(photos.error));
        const QString warm = photos.warm, cool = photos.cool;
        constexpr quint64 warmBytes = LargePhotos::warmBytes, coolBytes = LargePhotos::coolBytes;
        const auto pool = viewerResourceBudget();
        const auto caps = pool->limits();
        QVERIFY(warmBytes + coolBytes > caps.retained);
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        const QColor fallback(19, 59, 29);
        const auto newWindow = [&] {
            auto window = std::make_unique<QQuickWindow>();
            window->setVulkanInstance(&instance);
            window->resize(128, 96);
            window->setColor(fallback);
            return window;
        };
        // Inactive until the window's first frame has reported its output.
        const auto prepare = [](QQuickWindow& window, const QString& path) {
            auto photo = std::make_unique<HdrImageItem>(window.contentItem());
            photo->setSize(QSizeF(128, 96));
            photo->setTestHeadroom(4);
            photo->setSource(QUrl::fromLocalFile(path));
            window.show();
            return photo;
        };
        const auto center = [](QQuickWindow& window) {
            const auto frame = window.grabWindow();
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };
        auto holderWindow = newWindow();
        QVERIFY(reserveHdrTargets(holderWindow.get(), QSize(128, 96), 1));
        auto holder = prepare(*holderWindow, cool);
        QTRY_VERIFY(holder->outputReady());
        holder->setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(holder->photoStatus(), HdrImageItem::PhotoStatus::Hdr, 30000);
        const auto resident = center(*holderWindow);
        QVERIFY(resident.isValid() && resident != fallback);
        QCOMPARE(pool->used().retained, coolBytes);
        QCOMPARE(pool->used().upload, coolBytes);

        auto deniedWindow = newWindow();
        QVERIFY(reserveHdrTargets(deniedWindow.get(), QSize(128, 96), 1));
        auto denied = prepare(*deniedWindow, warm);
        QTRY_VERIFY(denied->outputReady());
        denied->setActive(true);
        QCOMPARE(denied->photoStatus(), HdrImageItem::PhotoStatus::Preparing);
        // Each attempt is refused when it asks for the raster allowance. The
        // attempts are bounded and no admitted byte exceeds a category limit.
        QElapsedTimer attempts;
        attempts.start();
        while (denied->photoStatus() == HdrImageItem::PhotoStatus::Preparing && attempts.elapsed() < 60000) {
            QTest::qWait(3);
            const auto used = pool->used();
            QVERIFY(used.retained <= caps.retained && used.decode <= caps.decode);
            QCOMPARE(used.upload, coolBytes);
            QVERIFY(!denied->testUsage()["ready"].toBool());
        }
        QCOMPARE(denied->photoStatus(), HdrImageItem::PhotoStatus::Unavailable);
        QVERIFY(!denied->testUsage()["decoded"].toBool());
        QCOMPARE(center(*deniedWindow), fallback);
        QCOMPARE(holder->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        QCOMPARE(center(*holderWindow), resident);
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, coolBytes);
        QCOMPARE(pool->used().upload, coolBytes);

        // Once the holder has gone, a new request for the refused photo fits.
        holder.reset();
        holderWindow.reset();
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
        denied->setActive(false);
        denied->setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(denied->photoStatus(), HdrImageItem::PhotoStatus::Hdr, 30000);
        QCOMPARE(pool->used().retained, warmBytes);
        QCOMPARE(pool->used().upload, warmBytes);
        const auto shown = center(*deniedWindow);
        QVERIFY(shown.isValid() && shown != fallback);
        denied.reset();
        deniedWindow.reset();
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().targets, quint64(0));
    }
    void memoryLimitChangesAtBarriers_data()
    {
        QTest::addColumn<QString>("barrier");
        QTest::addColumn<QString>("action");
        for (const char* barrier : {"growth", "decode", "upload", "retire"})
            for (const char* action : {"raise", "cancel", "close"})
                QTest::addRow("%s-%s", barrier, action) << QString::fromLatin1(barrier) << QString::fromLatin1(action);
    }
    void memoryLimitChangesAtBarriers()
    {
        // The memory preference drops to its minimum and GPU room closes while a
        // camera-sized photo is at one stage of becoming HDR. Admitted work keeps its
        // charge, refused work shows nothing, and a small photo in another
        // window never changes.
        QFETCH(QString, barrier);
        QFETCH(QString, action);
        using namespace Licasa;
        LargePhotos photos;
        QVERIFY2(photos.create(), qPrintable(photos.error));
        const QString warm = photos.warm, cool = photos.cool;
        constexpr quint64 warmBytes = LargePhotos::warmBytes, coolBytes = LargePhotos::coolBytes;
        constexpr quint64 smallBytes = 32 * 32 * 8;
        const auto pool = viewerResourceBudget();
        const auto restore = [] { configureHdrMemoryBudget(8192); };
        const auto reset = qScopeGuard(restore);
        const auto squeeze = [&](quint64 uploadInUse) {
            configureHdrMemoryBudget(Constants::minimumImageMemoryMiB);
            auto blocked = pool->limits();
            blocked.upload = uploadInUse;
            pool->setLimits(blocked, pool->gpuLimit(), pool->totalLimit());
        };
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        const QColor fallback(19, 59, 29);
        const auto newWindow = [&](QSize size) {
            auto window = std::make_unique<QQuickWindow>();
            window->setVulkanInstance(&instance);
            window->resize(size);
            window->setColor(fallback);
            return window;
        };
        // Inactive until the window's first frame has reported its output. The
        // item restarts its work at that report, which would cancel a decode
        // that a barrier below is waiting on.
        const auto prepare = [](QQuickWindow& window, const QString& path, float headroom) {
            auto photo = std::make_unique<HdrImageItem>(window.contentItem());
            photo->setSize(QSizeF(window.size()));
            photo->setTestHeadroom(headroom);
            photo->setSource(QUrl::fromLocalFile(path));
            window.show();
            return photo;
        };
        const auto center = [](QQuickWindow& window) {
            const auto frame = window.grabWindow();
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };
        using Status = HdrImageItem::PhotoStatus;

        auto peerWindow = newWindow(QSize(64, 64));
        QVERIFY(reserveHdrTargets(peerWindow.get(), QSize(64, 64), 1));
        auto peer = prepare(*peerWindow, QStringLiteral(GAINMAP_FIXTURE), 1.6f);
        QTRY_VERIFY(peer->outputReady());
        peer->setActive(true);
        QTRY_COMPARE(peer->photoStatus(), Status::Hdr);
        const auto gray = center(*peerWindow);
        QVERIFY(gray.isValid() && gray != fallback);
        const auto peerTargets = pool->used().targets;
#define PEER_UNDISTURBED() do { QCOMPARE(peer->photoStatus(), Status::Hdr); QCOMPARE(center(*peerWindow), gray); } while (false)

        auto window = newWindow(QSize(128, 96));
        QVERIFY(reserveHdrTargets(window.get(), QSize(128, 96), 1));
        const auto bothTargets = pool->used().targets;
        const auto wanted = barrier == "retire" ? cool : warm;
        const auto wantedBytes = barrier == "retire" ? coolBytes : warmBytes;
        auto photo = prepare(*window, warm, 4);
        QTRY_VERIFY(photo->outputReady());
        QColor outgoing;

        if (barrier == "growth") {
            // The first allowance fits under the minimum. The raster does not.
            squeeze(smallBytes);
            photo->setActive(true);
            QCOMPARE(photo->photoStatus(), Status::Preparing);
        } else if (barrier == "decode") {
            const auto first = GainmapLab::RequestCoordinator::decodeAllowance({});
            photo->setActive(true);
            QElapsedTimer waiting;
            waiting.start();
            while (pool->used().decode <= first.decode && !photo->testUsage()["decoded"].toBool()
                   && waiting.elapsed() < 60000) {
                QCoreApplication::processEvents();
                QThread::usleep(200);
            }
            const auto working = pool->used().decode;
            QVERIFY2(working > first.decode, "The codec finished before its grown allowance was observed");
            squeeze(smallBytes);
            // The worker still holds everything it was granted.
            QCOMPARE(pool->used().decode, working);
        } else if (barrier == "upload") {
            auto blocked = pool->limits();
            blocked.upload = smallBytes;
            pool->setLimits(blocked, pool->gpuLimit(), pool->totalLimit());
            photo->setActive(true);
            QTRY_VERIFY_WITH_TIMEOUT(photo->testUsage()["decoded"].toBool(), 60000);
            QVERIFY(!photo->testUsage()["ready"].toBool());
            squeeze(smallBytes);
            QCOMPARE(pool->used().retained, smallBytes + wantedBytes);
        } else {
            photo->setActive(true);
            QTRY_COMPARE_WITH_TIMEOUT(photo->photoStatus(), Status::Hdr, 30000);
            outgoing = center(*window);
            QVERIFY(outgoing.isValid() && outgoing != fallback);
            photo->setSource(QUrl::fromLocalFile(wanted));
            squeeze(smallBytes + warmBytes);
            // The outgoing texture may still be in a submitted frame.
            QCOMPARE(pool->used().upload, smallBytes + warmBytes);
            QCOMPARE(pool->used().retained, smallBytes + warmBytes);
        }
        PEER_UNDISTURBED();

        if (action == "raise") {
            // Left alone, the refused photo settles without ever being drawn.
            QTRY_COMPARE_WITH_TIMEOUT(photo->photoStatus(), Status::Unavailable, 60000);
            QVERIFY(!photo->testUsage()["ready"].toBool());
            QTRY_COMPARE(pool->used().decode, quint64(0));
            QTRY_COMPARE(pool->used().upload, smallBytes);
            const bool waitingRaster = barrier == "decode" || barrier == "upload";
            QCOMPARE(photo->testUsage()["decoded"].toBool(), waitingRaster);
            QCOMPARE(pool->used().retained, smallBytes + (waitingRaster ? wantedBytes : 0));
            QCOMPARE(center(*window), fallback);
            PEER_UNDISTURBED();
            restore();
            window->update();
            QTRY_COMPARE_WITH_TIMEOUT(photo->photoStatus(), Status::Hdr, 30000);
            QCOMPARE(pool->used().retained, smallBytes + wantedBytes);
            QCOMPARE(pool->used().upload, smallBytes + wantedBytes);
            const auto shown = center(*window);
            QVERIFY(shown.isValid() && shown != fallback && shown != outgoing);
        } else if (action == "cancel") {
            photo->setActive(false);
            QCOMPARE(photo->photoStatus(), Status::None);
            QTRY_COMPARE_WITH_TIMEOUT(pool->used().decode, quint64(0), 60000);
            QTRY_COMPARE(pool->used().retained, smallBytes);
            QTRY_COMPARE(pool->used().upload, smallBytes);
            QCOMPARE(pool->used().targets, bothTargets);
            QCOMPARE(center(*window), fallback);
            PEER_UNDISTURBED();
            restore();
            photo->setActive(true);
            QTRY_COMPARE_WITH_TIMEOUT(photo->photoStatus(), Status::Hdr, 30000);
            QCOMPARE(pool->used().retained, smallBytes + wantedBytes);
        } else {
            photo.reset();
            window.reset();
            QTRY_COMPARE_WITH_TIMEOUT(pool->used().decode, quint64(0), 60000);
            QTRY_COMPARE(pool->used().retained, smallBytes);
            QTRY_COMPARE(pool->used().upload, smallBytes);
            QCOMPARE(pool->used().targets, peerTargets);
        }
        PEER_UNDISTURBED();
#undef PEER_UNDISTURBED
        photo.reset();
        window.reset();
        peer.reset();
        peerWindow.reset();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(60000));
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().targets, quint64(0));
    }
    void renderFaultsReleaseAndRecover_data()
    {
        QTest::addColumn<QString>("fault");
        for (const char* fault : {"texture", "held-fence", "held-fence-close", "held-fence-large", "lost-fence"})
            QTest::newRow(fault) << QString::fromLatin1(fault);
    }
    void renderFaultsReleaseAndRecover()
    {
        // Faults on the render thread: no texture, a retirement fence that
        // stays pending, and a fence that reports a lost device.
        QFETCH(QString, fault);
        using namespace Licasa;
        using Status = HdrImageItem::PhotoStatus;
        const bool large = fault == "held-fence-large";
        LargePhotos photos;
        if (large) QVERIFY2(photos.create(), qPrintable(photos.error));
        const QString warm = photos.warm, cool = photos.cool;
        const QString gray = QStringLiteral(GAINMAP_FIXTURE);
        const QString red = QFileInfo(gray).dir().filePath("jpeg-gainmap-p3-red.jpg");
        // The cool photo leaves room for a default request beside it, so the
        // warm one is refused only when it asks for its whole raster.
        const QString firstPath = large ? cool : gray, secondPath = large ? warm : red;
        const quint64 firstBytes = large ? LargePhotos::coolBytes : 32 * 32 * 8;
        const quint64 secondBytes = large ? LargePhotos::warmBytes : 32 * 32 * 8;
        const auto pool = viewerResourceBudget();
        const auto clearFaults = qScopeGuard([] { HdrImageItem::setTestFaults({}); });
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        const QColor fallback(19, 59, 29);
        auto window = std::make_unique<QQuickWindow>();
        window->setVulkanInstance(&instance);
        window->resize(64, 64);
        window->setColor(fallback);
        QVERIFY(reserveHdrTargets(window.get(), QSize(1280, 720), 1));
        const auto targets = pool->used().targets;
        auto item = std::make_unique<HdrImageItem>(window->contentItem());
        item->setSize(QSizeF(64, 64));
        item->setTestHeadroom(large ? 4 : 1.6f);
        item->setSource(QUrl::fromLocalFile(firstPath));
        window->show();
        QTRY_VERIFY(item->outputReady());
        const auto center = [&] {
            const auto frame = window->grabWindow();
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };

        if (fault == "texture") {
            HdrImageItem::setTestFaults({1, false, false});
            item->setActive(true);
            QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), Status::Unavailable, 30000);
            QCOMPARE(HdrImageItem::testPendingTextureFailures(), 0);
            // The raster that could not become a texture is not kept.
            QTRY_COMPARE(pool->used().upload, quint64(0));
            QTRY_COMPARE(pool->used().retained, quint64(0));
            QVERIFY(!item->testUsage()["ready"].toBool());
            // Later frames do not bring the photo back without a new request.
            for (int frame = 0; frame < 5; ++frame) {
                window->update();
                QTest::qWait(30);
            }
            QCOMPARE(item->photoStatus(), Status::Unavailable);
            QCOMPARE(center(), fallback);
            item->setActive(false);
            item->setActive(true);
            QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), Status::Hdr, 30000);
            QCOMPARE(pool->used().upload, firstBytes);
            QVERIFY(center() != fallback);
        } else if (fault == "lost-fence") {
            item->setActive(true);
            QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), Status::Hdr, 30000);
            const auto first = center();
            const auto generation = item->testUsage()["generation"].toULongLong();
            HdrImageItem::setTestFaults({0, false, true});
            item->setSource(QUrl::fromLocalFile(secondPath));
            if (qEnvironmentVariable("QSG_RENDER_LOOP") == "threaded") {
                // The loss is reported once and the window rebuilds its graphics.
                QTRY_COMPARE_WITH_TIMEOUT(window->property("licasaHdrRecoveryState").toString(),
                                          QStringLiteral("rebound"), 30000);
                QTRY_VERIFY(hdrTargetReadbackReady(window.get()));
                QTRY_VERIFY(item->testUsage()["generation"].toULongLong() > generation);
            }
            // The basic loop refuses a live rebuild and keeps its device.
            QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), Status::Hdr, 30000);
            QVERIFY(window->isVisible());
            QTRY_COMPARE(pool->used().upload, secondBytes);
            QTRY_COMPARE(pool->used().retained, secondBytes);
            QCOMPARE(pool->used().targets, targets);
            const auto second = center();
            QVERIFY(second.isValid() && second != fallback && second != first);
        } else {
            item->setActive(true);
            QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), Status::Hdr, 30000);
            const auto first = center();
            QCOMPARE(pool->used().upload, firstBytes);
            HdrImageItem::setTestFaults({0, true, false});
            item->setSource(QUrl::fromLocalFile(secondPath));
            if (large) {
                // The next large photo cannot be admitted before the fence.
                QElapsedTimer held;
                held.start();
                while (held.elapsed() < 600) {
                    QTest::qWait(10);
                    QVERIFY(item->photoStatus() != Status::Hdr);
                    QVERIFY(pool->used().upload == firstBytes && pool->used().retained >= firstBytes);
                }
                QCOMPARE(item->photoStatus(), Status::Preparing);
            } else {
                // The small replacement fits beside the texture that still waits.
                QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), Status::Hdr, 30000);
                for (int frame = 0; frame < 10; ++frame) {
                    window->update();
                    QTest::qWait(20);
                    QCOMPARE(pool->used().upload, firstBytes + secondBytes);
                    QCOMPARE(pool->used().retained, firstBytes + secondBytes);
                }
                const auto second = center();
                QVERIFY(second.isValid() && second != fallback && second != first);
            }
            if (fault == "held-fence-close") {
                // Teardown waits for the real fence. It does not skip it.
                item.reset();
                window.reset();
                QTRY_COMPARE(pool->used().upload, quint64(0));
                QTRY_COMPARE(pool->used().retained, quint64(0));
                QCOMPARE(pool->used().targets, quint64(0));
            } else {
                HdrImageItem::setTestFaults({});
                QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), Status::Hdr, 30000);
                QTRY_COMPARE(pool->used().upload, secondBytes);
                QTRY_COMPARE(pool->used().retained, secondBytes);
                const auto second = center();
                QVERIFY(second.isValid() && second != fallback && second != first);
            }
        }
        item.reset();
        window.reset();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(60000));
        HdrImageItem observer;
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().targets, quint64(0));
        QTRY_COMPARE(observer.testUsage()["owners"].toInt(), 0);
    }
};

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) qputenv("QSG_RENDER_LOOP", "basic");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("LicasaHdrBudgetTests");
    QCoreApplication::setApplicationName("ViewerMemoryTests");
    ViewerMemoryTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_viewer_memory.moc"
