#include "../../src/imaging/hdr/hdr_image_item.h"
#include "../../src/imaging/hdr/output_policy.h"
#include "../../src/imaging/hdr/output_negotiation.h"
#include "surface_format.h"
#include "../../src/imaging/hdr/render_target_budget.h"
#include "../../src/imaging/hdr/adapter_budget.h"
#include "../../src/imaging/hdr/target_handoff.h"
#include "../../src/imaging/hdr/render_generation.h"
#include "../../src/imaging/hdr/vulkan_frame_fence.h"
#include "../../src/imaging/hdr/viewer_memory_policy.h"
#include "../../src/imaging/hdr/monitor_calibration.h"
#include "../../src/io/external_file_identity.h"
#include "../../src/imaging/presented_still_sources.h"
#include "wayland_test_client.h"
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <QAbstractEventDispatcher>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QEventLoop>
#include <QSettings>
#include <QTranslator>
#include <QScreen>
#include <QScopeGuard>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickGraphicsConfiguration>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QQuickGraphicsDevice>
#include <limits>
#include <QSGRendererInterface>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QTimer>
#include <QVulkanInstance>
#include <QtCore/qfloat16.h>
#include <QtTest>
#include <rhi/qrhi.h>
#include <cmath>
#include <cstring>

namespace {
struct SamePathGainMaps {
    QTemporaryDir directory;
    const QString path = directory.filePath("photo.jpg");
    const QDateTime modified = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
    QByteArray replacement;

    bool initialize()
    {
        QFile gray(GAINMAP_FIXTURE);
        QFile red(QFileInfo(gray).dir().filePath("jpeg-gainmap-p3-red.jpg"));
        if (!directory.isValid() || !gray.open(QIODevice::ReadOnly) || !red.open(QIODevice::ReadOnly))
            return false;
        auto original = gray.readAll();
        replacement = red.readAll();
        if (original.isEmpty() || replacement.isEmpty()) return false;
        const auto length = qMax(original.size(), replacement.size());
        original.append(QByteArray(length - original.size(), '\0'));
        replacement.append(QByteArray(length - replacement.size(), '\0'));
        return write(original);
    }

    bool replace()
    {
        const auto before = Licasa::externalFileIdentityAtPath(path);
        if (!before || !write(replacement)) return false;
        const auto after = Licasa::externalFileIdentityAtPath(path);
        return after && before->inode != after->inode && before->size == after->size
            && before->modifiedSeconds == after->modifiedSeconds
            && before->modifiedNanoseconds == after->modifiedNanoseconds;
    }

    QUrl source(int revision) const
    {
        auto url = QUrl::fromLocalFile(path);
        url.setFragment(QStringLiteral("licasa-revision-") + QString::number(revision));
        return url;
    }

private:
    bool write(const QByteArray& bytes)
    {
        QSaveFile target(path);
        if (!target.open(QIODevice::WriteOnly) || target.write(bytes) != bytes.size() || !target.commit())
            return false;
        QFile file(path);
        return file.open(QIODevice::ReadWrite) && file.setFileTime(modified, QFileDevice::FileModificationTime);
    }
};
}

class ViewerHdrTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { Licasa::configureHdrMemoryBudget(8192); }
    void memoryPreferenceKeepsLiveReservations()
    {
        using namespace Licasa;
        constexpr quint64 MiB = 1024 * 1024;
        auto reset = qScopeGuard([] { configureHdrMemoryBudget(8192); });
        ViewerPreferences preferences;
        preferences.setMaximumImageMemoryMiB(512);
        bindHdrMemoryPolicy(preferences);
        auto pool = viewerResourceBudget();
        QCOMPARE(pool->totalLimit(), 512 * MiB);
        pool->setAdapterLimit("policy-test", 1024 * MiB);
        auto live = pool->acquire({300 * MiB, 0, 0, 200 * MiB}, "policy-test");
        QVERIFY(live);
        preferences.setMaximumImageMemoryMiB(384);
        QCOMPARE(pool->totalLimit(), 384 * MiB);
        QCOMPARE(pool->used().targets, 200 * MiB);
        QVERIFY(!pool->acquire({1, 0, 0}));
        QVERIFY(!pool->acquire({0, 0, 1}, "policy-test"));
        auto gpu = live->split({0, 0, 0, 200 * MiB});
        QVERIFY(gpu);
        live.reset();
        QCOMPARE(pool->adapterUsed("policy-test"), 200 * MiB);
        auto next = pool->acquire({100 * MiB, 0, 0});
        QVERIFY(next);
        preferences.setMaximumImageMemoryMiB(8192);
        QCOMPARE(pool->totalLimit(), 8192 * MiB);
        gpu.reset(); next.reset();
        QCOMPARE(pool->adapterUsed("policy-test"), quint64(0));
    }
    void generationHandoffAndDeviceLoss()
    {
        using namespace Licasa;
        using Pool = GainmapLab::ResourceBudget;
        HdrRenderGeneration generation;
        const auto stale = generation.current();
        generation.advance();
        QVERIFY(!generation.matches(stale));
        auto pool = std::make_shared<Pool>(Pool::Bytes{0,0,20,80}, 100, 100, true);
        pool->setAdapterLimit("A",80); pool->setAdapterLimit("B",80);
        HdrTargetHandoff handoff(pool,60,"A",pool->acquire({0,0,0,60},"A"));
        const auto old = handoff.request("B");
        const auto latest = handoff.request("B");
        QCOMPARE(pool->adapterUsed("A"),quint64(60));
        QVERIFY(!handoff.detached(old));
        QVERIFY(!pool->acquire({0,0,0,60},"B"));
        QVERIFY(handoff.detached(latest));
        QVERIFY(handoff.admit(latest));
        QCOMPARE(pool->adapterUsed("A"),quint64(0));
        QCOMPARE(pool->adapterUsed("B"),quint64(60));
        QVERIFY(!handoff.detached(old)); // Old completions cannot release B.
        pool->setAdapterLimit("A",10);
        const auto denied = handoff.request("A");
        QVERIFY(handoff.detached(denied));
        QVERIFY(!handoff.admit(denied));
        QCOMPARE(handoff.phase(),HdrTargetHandoff::Phase::Sdr);
        QCOMPARE(pool->adapterUsed("B"),quint64(0));
        QCOMPARE(hdrFenceStatus(VK_ERROR_DEVICE_LOST),GainmapLab::GpuFence::Status::DeviceLost);
        QCOMPARE(hdrFenceStatus(VK_ERROR_OUT_OF_HOST_MEMORY),GainmapLab::GpuFence::Status::Failed);
        QVERIFY(hdrDeviceQuiescent(VK_ERROR_DEVICE_LOST));
        QVERIFY(!hdrDeviceQuiescent(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        struct Fence : GainmapLab::GpuFence {
            bool proof = false;
            Status poll() noexcept override { return Status::DeviceLost; }
            bool wait() noexcept override { return proof; }
        };
        auto fence = std::make_shared<Fence>();
        GainmapLab::GpuRetirement queue;
        auto lease = pool->acquire({0,0,1},"B");
        QVERIFY(queue.retire(lease,fence)); lease.reset();
        const bool collected = queue.collect();
        const bool unproven = queue.shutdown();
        const auto held = pool->adapterUsed("B");
        fence->proof = true; // Legal loss/teardown proof is scoped to this fence.
        QVERIFY(!collected); QVERIFY(!unproven); QCOMPARE(held,quint64(1));
        QVERIFY(queue.deviceLost());
        QVERIFY(queue.shutdown());
        QCOMPARE(pool->adapterUsed("B"),quint64(0));
    }
    void nativeRecovery()
    {
        using namespace Licasa;
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1,1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        auto window = std::make_unique<QQuickWindow>();
        window->setVulkanInstance(&instance); window->resize(64,64);
        QVERIFY(reserveHdrTargets(window.get(),QSize(1280,720),1));
        window->setProperty("reviewEditState",QStringLiteral("unsaved adjustments"));
        auto item = std::make_unique<HdrImageItem>(window->contentItem());
        item->setSize(QSizeF(64,64)); item->setTestHeadroom(4);
        item->setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE)); item->setActive(true);
        window->show();
        QTRY_VERIFY(item->testUsage()["frames"].toULongLong()>0);
        QTRY_VERIFY(hdrTargetReadbackReady(window.get()));
        const auto key = window->property("licasaHdrAdapter").toByteArray();
        const auto before = item->testUsage()["generation"].toULongLong();
        const auto bytes = viewerResourceBudget()->used().targets;
        QVERIFY(!requestHdrAdapterMigration(window.get(),QByteArray("unknown")));
        QVERIFY(!hdrTargetRecovering(window.get()));
        if (qEnvironmentVariable("QSG_RENDER_LOOP") == "basic") {
            QVERIFY(!requestHdrDeviceRecovery(window.get()));
            QVERIFY(window->isVisible());
            QCOMPARE(viewerResourceBudget()->used().targets,bytes);
            item.reset(); window.reset();
            QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
            QTRY_COMPARE(viewerResourceBudget()->used().targets,quint64(0));
            return;
        }
        item->setTestHeadroom(1.5f); // A replacement decode may overlap teardown.
        QCoreApplication::processEvents(QEventLoop::AllEvents,1);
        QVERIFY(requestHdrDeviceRecovery(window.get()));
        QVERIFY(hdrTargetKeepsWindowAlive(window.get())); // Hiding for graphics is not an idle-app exit.
        QVERIFY(requestHdrAdapterMigration(window.get(),key)); // Latest intent coalesces.
        QVERIFY(!hdrTargetReadbackReady(window.get()));
        QTRY_COMPARE(window->property("licasaHdrRecoveryState").toString(),QStringLiteral("rebound"));
        QTRY_VERIFY(hdrTargetReadbackReady(window.get()));
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QTRY_VERIFY(item->testUsage()["generation"].toULongLong()>before);
        QCOMPARE(viewerResourceBudget()->used().targets,bytes);
        QCOMPARE(window->property("reviewEditState").toString(),QStringLiteral("unsaved adjustments"));
        const auto recovered = window->grabWindow();
        QVERIFY(!recovered.isNull());
        QVERIFY(recovered.pixelColor(0,0).redF() > 0.7f && recovered.pixelColor(0,0).redF() < 0.9f);
        // A lower user budget can deny the destination after old native teardown.
        configureHdrMemoryBudget(384);
        auto reset = qScopeGuard([] { configureHdrMemoryBudget(8192); });
        QVERIFY(requestHdrDeviceRecovery(window.get()));
        QTRY_COMPARE(window->property("licasaHdrRecoveryState").toString(),QStringLiteral("sdr"));
        QTRY_VERIFY(hdrTargetReadbackReady(window.get()));
        QCOMPARE(viewerResourceBudget()->used().targets,quint64(0));
        QVERIFY(window->property("_qt_sg_hdr_format").toByteArray().isEmpty());
        configureHdrMemoryBudget(8192);
        QVERIFY(requestHdrDeviceRecovery(window.get()));
        QTRY_COMPARE(window->property("licasaHdrRecoveryState").toString(),QStringLiteral("rebound"));
        QTRY_VERIFY(hdrTargetReadbackReady(window.get()));
        QCOMPARE(viewerResourceBudget()->used().targets,bytes);
        QVERIFY(requestHdrDeviceRecovery(window.get()));
        item.reset(); window.reset(); // Pending callbacks must not revive a closed window.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QTRY_COMPARE(viewerResourceBudget()->used().targets,quint64(0));
        QTRY_COMPARE(viewerResourceBudget()->used().upload,quint64(0));
    }
    void adapterSelectionRejectsOverrides()
    {
        QCOMPARE(Licasa::hdrAdapterLimit(0), quint64(0));
        QCOMPARE(Licasa::hdrAdapterLimit(1024), quint64(256));
        QCOMPARE(Licasa::hdrAdapterLimit(std::numeric_limits<quint64>::max()), quint64(1024) * 1024 * 1024);
        QQuickWindow window;
        QVERIFY(!Licasa::describeHdrAdapter(nullptr, VK_NULL_HANDLE));
        qputenv("QT_VK_PHYSICAL_DEVICE_INDEX", "9999");
        const auto invalid = Licasa::selectHdrAdapter(&window);
        qunsetenv("QT_VK_PHYSICAL_DEVICE_INDEX");
        QVERIFY(!invalid);
        QVERIFY(window.graphicsDevice().isNull());
        const auto selection = Licasa::selectHdrAdapter(&window);
        QVERIFY(selection);
        QVERIFY(selection->adapter.limit > 0);
        QVERIFY(selection->adapter.localHeapBytes >= selection->adapter.limit * 4);
        qInfo() << "Adapter preflight" << selection->adapter.key.toHex()
                << selection->adapter.name << selection->adapter.localHeapBytes << selection->adapter.limit;
        window.setGraphicsDevice(QQuickGraphicsDevice::fromPhysicalDevice(selection->physicalDevice));
        QVERIFY(!Licasa::selectHdrAdapter(&window)); // Preserve an externally selected device.
    }
    void targetBudgetAdmissionAndResize()
    {
        using namespace Licasa;
        QVERIFY(!hdrTargetPlan(QSize(0,32), 1));
        QVERIFY(!hdrTargetPlan(QSize(32769,32), 1));
        QVERIFY(!hdrTargetPlan(QSize(32,32), qInf()));
        QVERIFY(!hdrTargetPlan(QSize(32,32), qQNaN()));
        QVERIFY(!hdrTargetPlan(QSize(32,32), 0));
        QVERIFY(!hdrTargetPlan(QSize(INT_MAX,INT_MAX), 100));
        const auto plan = hdrTargetPlan(QSize(64,64), 1);
        const auto hidpi = hdrTargetPlan(QSize(64,64), 2);
        QVERIFY(plan && hidpi);
        QCOMPARE(hidpi->pixelLimit, QSize(128,128));
        QVERIFY(hidpi->bytes > plan->bytes);
        auto pool = std::make_shared<GainmapLab::ResourceBudget>(
            GainmapLab::ResourceBudget::Bytes{0, 0, plan->bytes, 2 * plan->bytes}, 2 * plan->bytes);
        QQuickWindow multisampled;
        auto format = multisampled.format(); format.setSamples(4);
        multisampled.setFormat(format);
        QVERIFY(!reserveHdrTargets(&multisampled, QSize(64,64), 1, pool));
        QCOMPARE(pool->used().targets, quint64(0));
        qputenv("QSG_SAMPLES", "4");
        QQuickWindow overridden;
        const bool overrideAdmitted = reserveHdrTargets(&overridden, QSize(64,64), 1, pool);
        qunsetenv("QSG_SAMPLES");
        QVERIFY(!overrideAdmitted);
        auto first = std::make_unique<QQuickWindow>();
        first->resize(96,96);
        QVERIFY(reserveHdrTargets(first.get(), QSize(64,64), 1, pool));
        QCOMPARE(first->size(), QSize(64,64));
        QCOMPARE(pool->used().targets, plan->bytes);
        QVERIFY(hdrTargetsCover(first.get(), QSize(64,64)));
        QVERIFY(!hdrTargetsCover(first.get(), QSize(65,64)));
        QVERIFY(!reserveHdrTargets(first.get(), QSize(64,64), 1, pool));
        first->resize(256,256); // Explicit sizes must obey the admitted envelope too.
        QCOMPARE(first->size(), QSize(64,64));
        first->resize(16,16);
        QCOMPARE(pool->used().targets, plan->bytes); // Shrink/hide do not free Qt caches.
        first->hide(); first->releaseResources();
        QCOMPARE(pool->used().targets, plan->bytes);
        auto upload = pool->acquire({0, 0, 1, 0});
        QVERIFY(upload);
        QQuickWindow denied;
        denied.resize(96,96);
        const QSize originalMax = denied.maximumSize();
        QVERIFY(!reserveHdrTargets(&denied, QSize(64,64), 1, pool));
        QCOMPARE(denied.maximumSize(), originalMax);
        QCOMPARE(denied.size(), QSize(96,96));
        QVERIFY(!denied.property("licasaHdrTargetPixels").isValid());
        upload.reset();
        auto second = std::make_unique<QQuickWindow>();
        QVERIFY(reserveHdrTargets(second.get(), QSize(64,64), 1, pool));
        QCOMPARE(pool->used().targets, 2 * plan->bytes);
        first.reset();
        QCOMPARE(pool->used().targets, plan->bytes);
        QVERIFY(reserveHdrTargets(&denied, QSize(64,64), 1, pool)); // Capacity reused.
        second.reset();
        QCOMPARE(pool->used().targets, plan->bytes);
    }
    void outputEpochAndFallback()
    {
        using namespace Licasa;
        HdrOutputState output;
        auto token = output.snapshot().revision;
        QVERIFY(output.calibrate(token,200,800));
        auto snapshot = output.snapshot();
        QCOMPARE(hdrSurfacePolicy(true,true,snapshot).whiteScale,2.5f);
        QCOMPARE(hdrSurfacePolicy(true,true,snapshot).headroom,4.0f);
        HdrOutputState::Native native{true,true,true};
        native.white = 80; native.maximum = 1000;
        QVERIFY(!output.observe(native));
        token = snapshot.revision;
        native.white = 100; // A compositor SDR-white/output metadata change.
        QVERIFY(output.observe(native));
        QVERIFY(!output.calibrate(token,200,800));
        QVERIFY(output.snapshot().nativeReview);
        output.invalidate(); // A later topology notification must retain native review.
        QVERIFY(output.snapshot().nativeReview);
        snapshot = output.snapshot();
        QCOMPARE(hdrSurfacePolicy(true,true,snapshot).headroom,0.0f);
        QCOMPARE(hdrSurfacePolicy(true,true,snapshot).whiteScale,1.0f);
        QCOMPARE(hdrSurfacePolicy(false,true,snapshot).whiteScale,0.0f);
        QVERIFY(output.calibrate(snapshot.revision,100,1000));
        QVERIFY(!output.snapshot().nativeReview);
        QCOMPARE(hdrSurfacePolicy(true,true,output.snapshot()).whiteScale,1.25f);
        QCOMPARE(hdrSurfacePolicy(true,true,output.snapshot()).headroom,8.0f);
        native.supported = false; native.linear = false;
        QVERIFY(output.observe(native));
        QVERIFY(!output.calibrate(output.snapshot().revision,200,800));
        native.supported = true; // Returning HDR does not revive old calibration.
        QVERIFY(output.observe(native));
        QVERIFY(!output.snapshot().calibrated);
        QVERIFY(output.calibrate(output.snapshot().revision,500,1000));
        output.expectSurface(); native.linear = true;
        QVERIFY(!output.observe(native)); // Expected fresh native surface.
        QCOMPARE(hdrSurfacePolicy(true,true,output.snapshot()).whiteScale,6.25f);
        for (float bad : {0.0f,79.0f,501.0f,float(qQNaN()),float(qInf())})
            QVERIFY(!output.calibrate(output.snapshot().revision,bad,1000));
        for (int i=0; i<100; ++i) {
            const auto stale = output.invalidate();
            output.invalidate(); // A-B-A/output notifications must not reuse epochs.
            QVERIFY(!output.calibrate(stale,200,800));
        }
    }
    void monitorNotificationsAndRecalibration()
    {
        using namespace Licasa;
        qputenv("LICASA_HDR_PAPER_WHITE_NITS","200"); qputenv("LICASA_HDR_PEAK_NITS","800");
        auto env = qScopeGuard([] { qunsetenv("LICASA_HDR_PAPER_WHITE_NITS"); qunsetenv("LICASA_HDR_PEAK_NITS"); });
        auto window = std::make_unique<QQuickWindow>();
        window->resize(64,64); window->setMaximumSize(QSize(64,64));
        QVERIFY(requestHdrOutput(window.get()));
        const auto state = hdrOutputState(window.get());
        QVERIFY(state && state->snapshot().calibrated);
        const auto initial = hdrOutputRevision(window.get());
        auto* screen = window->screen(); QVERIFY(screen);
        // Emit notifications only inside this isolated test process. No desktop
        // monitor/output configuration or physical hot-plug is performed.
        screen->refreshRateChanged(screen->refreshRate());
        QVERIFY(hdrOutputRevision(window.get())>initial);
        QVERIFY(!state->snapshot().calibrated);
        QVERIFY(!recalibrateHdrOutput(window.get(),initial,200,800));
        QVERIFY(recalibrateHdrOutput(window.get(),hdrOutputRevision(window.get()),100,1000));
        QCOMPARE(state->snapshot().white,100.0f);
        const auto old = hdrOutputRevision(window.get());
        window->screenChanged(nullptr);
        QVERIFY(!recalibrateHdrOutput(window.get(),hdrOutputRevision(window.get()),200,800));
        window->screenChanged(screen);
        QVERIFY(!recalibrateHdrOutput(window.get(),old,200,800));
        QVERIFY(recalibrateHdrOutput(window.get(),hdrOutputRevision(window.get()),200,800));
        qGuiApp->screenAdded(screen); // Conservative topology invalidation.
        QVERIFY(!state->snapshot().calibrated);
        const auto topology = hdrOutputRevision(window.get());
        QEvent dpr(QEvent::DevicePixelRatioChange); QCoreApplication::sendEvent(window.get(),&dpr);
        QVERIFY(hdrOutputRevision(window.get())>topology);
        QVERIFY(recalibrateHdrOutput(window.get(),hdrOutputRevision(window.get()),200,800));
        qGuiApp->screenRemoved(screen);
        QVERIFY(!state->snapshot().calibrated);
        QVERIFY(!recalibrateHdrOutput(window.get(),hdrOutputRevision(window.get()),200,800));
        window->screenChanged(screen);
        // The final pre-native intent is SDR; initial target debit may drain only
        // through the same proven native handoff machinery.
        QTRY_COMPARE(window->property("_qt_sg_hdr_format").toByteArray(),QByteArray());
        QTRY_COMPARE(viewerResourceBudget()->used().targets,quint64(0));
        QVERIFY(!window->isVisible()); // Pre-native fallback must not expose it.
        QVERIFY(recalibrateHdrOutput(window.get(),hdrOutputRevision(window.get()),200,800));
        QVERIFY(hdrTargetRecovering(window.get()));
        notifyHdrOutputChanged(window.get()); // Latest SDR intent supersedes pending HDR admission.
        QTRY_VERIFY(!hdrTargetRecovering(window.get()));
        QCOMPARE(window->property("_qt_sg_hdr_format").toByteArray(),QByteArray());
        QCOMPARE(viewerResourceBudget()->used().targets,quint64(0));
        window->setProperty("reviewEditState",QStringLiteral("preserved"));
        auto observer = std::make_unique<HdrImageItem>(window->contentItem());
        observer->setSize(QSizeF(64,64)); observer->setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE)); observer->setActive(true);
        window->show();
        QTRY_VERIFY(hdrTargetReadbackReady(window.get()));
        QTRY_VERIFY(observer->outputReady());
        QVERIFY(window->isVisible());
        QCOMPARE(window->property("reviewEditState").toString(),QStringLiteral("preserved"));
        // A live notification clears calibration immediately in either render
        // loop. Threaded may recreate; basic keeps a safe SDR composition policy.
        const auto live = notifyHdrOutputChanged(window.get());
        QVERIFY(live>0); QVERIFY(!state->snapshot().calibrated);
        QVERIFY(!observer->outputReady()); // GUI notification immediately gates stale composition.
        QVERIFY(!observer->testUsage()["ready"].toBool());
        QVERIFY(!recalibrateHdrOutput(window.get(),live-1,200,800));
        QTRY_VERIFY(hdrTargetReadbackReady(window.get()));
        QVERIFY(!window->grabWindow().isNull());
        notifyHdrOutputChanged(window.get());
        window->close();
        QVERIFY(!hdrTargetKeepsWindowAlive(window.get()));
        QVERIFY(!recalibrateHdrOutput(window.get(),hdrOutputRevision(window.get()),200,800));
        window->show(); // An explicit later reopen is allowed; it stays SDR.
        QTRY_VERIFY(observer->outputReady());
        QVERIFY(!state->snapshot().calibrated);
        observer.reset();
        window.reset(); // Pending notifications/poll timers cannot reopen it.
        QCoreApplication::processEvents();
        QCOMPARE(viewerResourceBudget()->used().targets,quint64(0));
    }
    void waylandReplacementFeedback_data()
    {
        QTest::addColumn<QString>("action");
        for (const char* action :
             {"match", "timeout", "changed-description", "retired-reply", "use-sdr", "close",
              "screen-move", "active-surface-loss", "match-before-deadline", "match-after-deadline",
              "second-replacement"}) {
            QTest::newRow(action) << QString::fromLatin1(action);
        }
    }
    void waylandReplacementFeedback()
    {
        using namespace Licasa;
        using State = WaylandHdrReport::State;
        QFETCH(QString, action);
        struct SecondScreen final : QPlatformScreen {
            QRect geometry() const override { return {0,0,64,64}; }
            int depth() const override { return 32; }
            QImage::Format format() const override { return QImage::Format_RGB32; }
        };
        auto* second = new SecondScreen;
        QWindowSystemInterface::handleScreenAdded(second);
        const auto removeScreen = qScopeGuard([&] { QWindowSystemInterface::handleScreenRemoved(second); });
        WaylandTest::Client client({}, false);
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1,1));
        QVERIFY(instance.create());
        QQuickWindow window;
        window.setVulkanInstance(&instance);
        window.resize(32,32);
        window.setMaximumSize({64,64});
        const auto* initialScreen = window.screen();
        QVERIFY(initialScreen && initialScreen != second->screen());
        auto* observer = attachWaylandOutputForTest(&window,client.display,client.surface);
        const auto output = hdrOutputState(&window);
        const auto waitForFeedback = [](auto ready) {
            if (ready()) return true;
            // Run the dispatcher so the observer can drain its own queue
            // before blocking, as it does in the application event loop.
            QEventLoop loop;
            QTimer poll;
            QObject::connect(&poll,&QTimer::timeout,&loop,[&] { if (ready()) loop.quit(); });
            poll.start(5);
            QTimer::singleShot(2500,&loop,&QEventLoop::quit);
            loop.exec();
            return ready();
        };
        const auto replaceSurface = [&](bool holdFirstReply = false) {
            observer->setSurface(nullptr);
            wl_surface_destroy(client.surface);
            client.surface=nullptr;
            // Let the compositor retire the old feedback before changing the
            // preference for the new surface.
            if (wl_display_roundtrip(client.display)<0) return false;
            client.server.holdNextInfo=holdFirstReply;
            client.server.nextMaximum=holdFirstReply ? 1000 : 80;
            if (!waitForFeedback([&] { return client.server.nextMaximum.load()==-1; })) return false;
            client.surface=wl_compositor_create_surface(client.compositor);
            observer->setSurface(client.surface);
            return true;
        };
        QVERIFY(waitForFeedback([&] { return hdrWaylandPermitsHdr(&window); }));
        const auto original = observer->report();
        QVERIFY(calibrateWaylandOutputForTest(&window,200,800));
        QTRY_COMPARE(window.property("_qt_sg_hdr_format").toByteArray(), QByteArray("scrgb"));
        const auto calibrated = output->snapshot().revision;
        QVERIFY(output->snapshot().calibrated);

        if (action == "active-surface-loss") {
            client.server.nextMaximum=80;
            QTRY_COMPARE(observer->report().state,State::Sdr);
            QTRY_VERIFY(output->snapshot().nativeReview);
            QVERIFY(!output->snapshot().calibrated);
            QVERIFY(!hdrWaylandPermitsHdr(&window));
            QCOMPARE(observer->report().surfaceEpoch, original.surfaceEpoch);
            QCOMPARE(hdrMonitorSettings(&window).value("code").toString(), QStringLiteral("wayland-sdr"));
            return;
        }

        if (action == "retired-reply") {
            QVERIFY(replaceSurface(true));
            QVERIFY(waitForFeedback([&] { return client.server.heldInfos.load()==1; }));
            QCOMPARE(observer->report().state,State::Pending);
            QVERIFY(!hdrWaylandPermitsHdr(&window));
        }
        QVERIFY(replaceSurface());
        QVERIFY(waitForFeedback([&] { return observer->report().state==State::Sdr
            && hdrMonitorSettings(&window).value("code")=="wayland-checking"; }));
        QVERIFY(!hdrWaylandPermitsHdr(&window));
        QCOMPARE(output->snapshot().revision,calibrated);
        QVERIFY(output->snapshot().calibrated);
        QVERIFY(observer->report().surfaceEpoch > original.surfaceEpoch);

        if (action == "match-before-deadline") {
            QTest::qWait(650);
            QVERIFY(output->snapshot().calibrated);
            QVERIFY(!hdrWaylandPermitsHdr(&window));
        } else if (action == "match-after-deadline") {
            // Queue matching feedback after the wall-clock deadline while the
            // GUI is busy. Dispatch the protocol and its queued notification
            // before Qt delivers the overdue timer, as a socket wakeup can do.
            QTest::qSleep(1100);
            client.server.nextMaximum = 1000;
            for (int i = 0; i < 100 && client.server.nextMaximum.load() != -1; ++i) {
                QTest::qSleep(5);
            }
            QCOMPARE(client.server.nextMaximum.load(), -1);
            for (int i = 0; i < 100 && observer->report().state != State::Ready; ++i) {
                QVERIFY(wl_display_roundtrip(client.display) >= 0);
                QAbstractEventDispatcher::instance()->aboutToBlock();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            }
            QCOMPARE(observer->report().state, State::Ready);
            QVERIFY(output->snapshot().nativeReview);
            QVERIFY(!output->snapshot().calibrated);
        } else if (action == "second-replacement") {
            QTest::qWait(650);
            const auto firstReplacement = observer->report().surfaceEpoch;
            QVERIFY(replaceSurface());
            QVERIFY(waitForFeedback([&] { return observer->report().state == State::Sdr; }));
            QVERIFY(observer->report().surfaceEpoch > firstReplacement);
            // This surface has its own grace period. The first surface's
            // timeout must not invalidate the replacement's calibration.
            QTest::qWait(650);
            QVERIFY(output->snapshot().calibrated);
            QVERIFY(!output->snapshot().nativeReview);
            QVERIFY(!hdrWaylandPermitsHdr(&window));
        }

        if (action == "timeout") {
            QTRY_VERIFY_WITH_TIMEOUT(output->snapshot().nativeReview,2000);
            QTRY_VERIFY(window.property("_qt_sg_hdr_format").toByteArray().isEmpty());
            QVERIFY(!hdrWaylandPermitsHdr(&window));
        } else if (action == "retired-reply") {
            const auto replacement = observer->report();
            client.server.releaseLate=true;
            QVERIFY(waitForFeedback([&] { return client.server.heldInfos.load()==0; }));
            QVERIFY(wl_display_roundtrip(client.display)>=0);
            QCoreApplication::processEvents();
            QCOMPARE(observer->report().revision,replacement.revision);
            QVERIFY(!hdrWaylandPermitsHdr(&window));
            QCOMPARE(output->snapshot().revision,calibrated);
        } else if (action == "use-sdr") {
            QVERIFY(useSdrForMonitor(&window,calibrated));
            QTRY_VERIFY(window.property("_qt_sg_hdr_format").toByteArray().isEmpty());
        } else if (action == "close") {
            QCloseEvent close;
            QCoreApplication::sendEvent(&window,&close);
        } else if (action == "screen-move") {
            window.setScreen(second->screen());
            QCOMPARE(window.screen(),second->screen());
        }

        const bool changed = action == "changed-description";
        if (changed) client.server.nextIdentity=original.preferredIdentity;
        client.server.nextMaximum=changed ? 600 : 1000;
        QVERIFY(waitForFeedback([&] { return observer->report().state==State::Ready; }));
        if (action == "close") {
            QCoreApplication::processEvents();
            QVERIFY(!hdrWaylandPermitsHdr(&window));
        } else {
            QVERIFY(waitForFeedback([&] { return hdrWaylandPermitsHdr(&window); }));
        }
        if (action == "match" || action == "retired-reply" || action == "match-before-deadline" ||
            action == "second-replacement") {
            QCOMPARE(output->snapshot().revision,calibrated);
            QVERIFY(output->snapshot().calibrated);
            QCOMPARE(output->snapshot().white,200.0f);
            QCOMPARE(output->snapshot().peak,800.0f);
        } else {
            QVERIFY(output->snapshot().revision > calibrated);
            QVERIFY(!output->snapshot().calibrated);
            if (changed || action == "timeout" || action == "match-after-deadline") {
                QVERIFY(output->snapshot().nativeReview);
                QVERIFY(!recalibrateHdrOutput(&window,calibrated,200,800));
                QVERIFY(calibrateWaylandOutputForTest(&window,200,800));
                QVERIFY(output->snapshot().calibrated);
            }
        }
        if (changed) {
            QCOMPARE(observer->report().preferredIdentity,original.preferredIdentity);
            QVERIFY(observer->report().preferredFingerprint != original.preferredFingerprint);
        }
        if (action == "use-sdr") QCOMPARE(hdrMonitorSettings(&window).value("code").toString(), QStringLiteral("disabled"));
    }
    void monitorCalibrationPersistence()
    {
        using namespace Licasa;
        const auto a=hdrMonitorKey("Licasa Test","HDR panel","profile-A");
        const auto b=hdrMonitorKey("Licasa Test","HDR panel","profile-B");
        QVERIFY(validHdrMonitorKey(a)); QVERIFY(a!=b);
        QCOMPARE(a,hdrMonitorKey(" Licasa Test ","HDR panel","profile-A"));
        QVERIFY(hdrMonitorKey("","panel","serial").isEmpty());
        QVERIFY(hdrMonitorKey("Maker","panel","00000").isEmpty());
        QVERIFY(hdrMonitorKey("Maker","panel","unknown").isEmpty());
        ViewerPreferences prefs;
        auto cleanup=qScopeGuard([&] { prefs.removeHdrMonitorCalibration(a); prefs.removeHdrMonitorCalibration(b); });
        QSignalSpy changed(&prefs,&ViewerPreferences::hdrMonitorCalibrationChanged);
        QVERIFY(prefs.setHdrMonitorCalibration(a,true,200,800));
        QVERIFY(prefs.setHdrMonitorCalibration(b,true,100,1000));
        ViewerPreferences reopened;
        QCOMPARE(reopened.hdrMonitorCalibration(a).value("paperWhite").toFloat(),200.0f);
        QCOMPARE(reopened.hdrMonitorCalibration(b).value("paperWhite").toFloat(),100.0f);
        QVERIFY(!prefs.setHdrMonitorCalibration(a,true,qQNaN(),800));
        QVERIFY(!prefs.setHdrMonitorCalibration(a,true,200,200));
        QVERIFY(!prefs.setHdrMonitorCalibration("../../bad",true,200,800));
        QCOMPARE(changed.count(),2);
        QCOMPARE(prefs.hdrMonitorCalibration(a).value("paperWhite").toFloat(),200.0f);
        QVERIFY(prefs.setHdrMonitorCalibration(a,false,200,800));
        QVERIFY(!ViewerPreferences().hdrMonitorCalibration(a).value("enabled").toBool());
        QSettings raw;
        raw.setValue("viewer/hdr/monitors/"+a,QVariantMap{{"version",999},{"enabled",true},{"paperWhite",200},{"peak",800}});
        raw.sync();
        QVERIFY(ViewerPreferences().hdrMonitorCalibration(a).isEmpty());
        QVERIFY(prefs.removeHdrMonitorCalibration(a));
        QVERIFY(ViewerPreferences().hdrMonitorCalibration(a).isEmpty());
        QVERIFY(!prefs.hdrMonitorCalibration(b).isEmpty());
    }
    void failedMonitorSaveKeepsPreviousProfile()
    {
        using namespace Licasa;
        const auto oldName=QCoreApplication::applicationName();
        QCoreApplication::setApplicationName("HdrUnwritableProfile");
        auto appName=qScopeGuard([&] { QCoreApplication::setApplicationName(oldName); });
        const auto key=hdrMonitorKey("Licasa Test","HDR panel","failed-save");
        ViewerPreferences prefs;
        QVERIFY(prefs.setHdrMonitorCalibration(key,true,100,600));
        QSettings raw;
        const auto filename=raw.fileName(); const auto directory=QFileInfo(filename).absolutePath();
        const auto fileMode=QFile::permissions(filename),directoryMode=QFile::permissions(directory);
        auto permissions=qScopeGuard([&] { QFile::setPermissions(directory,directoryMode); QFile::setPermissions(filename,fileMode); });
        QVERIFY(QFile::setPermissions(filename,QFile::ReadOwner));
        QVERIFY(QFile::setPermissions(directory,QFile::ReadOwner|QFile::ExeOwner));
        QSignalSpy errors(&prefs,&ViewerPreferences::errorOccurred);
        QSignalSpy changed(&prefs,&ViewerPreferences::hdrMonitorCalibrationChanged);
        QVERIFY(!prefs.setHdrMonitorCalibration(key,true,200,800));
        QCOMPARE(errors.count(),1); QCOMPARE(changed.count(),0);
        QCOMPARE(prefs.hdrMonitorCalibration(key).value("paperWhite").toFloat(),100.0f);
        QVERIFY(!prefs.removeHdrMonitorCalibration(key));
        QCOMPARE(prefs.hdrMonitorCalibration(key).value("peak").toFloat(),600.0f);
    }
    void monitorProfilesFollowRealScreenObjects()
    {
        using namespace Licasa;
        struct Screen final : QPlatformScreen {
            QString serial,port;
            Screen(QString s,QString p):serial(s),port(p) {}
            QRect geometry() const override { return {0,0,64,64}; }
            int depth() const override { return 32; }
            QImage::Format format() const override { return QImage::Format_RGB32; }
            QString manufacturer() const override { return "Licasa Test"; }
            QString model() const override { return "HDR panel"; }
            QString serialNumber() const override { return serial; }
            QString name() const override { return port; }
        };
        auto* a=new Screen("profile-A","port-A"); auto* b=new Screen("profile-B","port-B");
        QWindowSystemInterface::handleScreenAdded(a); QWindowSystemInterface::handleScreenAdded(b);
        auto screens=qScopeGuard([&] { QWindowSystemInterface::handleScreenRemoved(b); QWindowSystemInterface::handleScreenRemoved(a); });
        const auto keyA=hdrMonitorKey(a->manufacturer(),a->model(),a->serial);
        const auto keyB=hdrMonitorKey(b->manufacturer(),b->model(),b->serial);
        ViewerPreferences prefs;
        auto cleanup=qScopeGuard([&] { prefs.removeHdrMonitorCalibration(keyA); prefs.removeHdrMonitorCalibration(keyB); });
        QVERIFY(prefs.setHdrMonitorCalibration(keyA,true,200,800));
        QVERIFY(prefs.setHdrMonitorCalibration(keyB,true,100,1000));
        auto make=[&](QScreen* screen) {
            auto w=std::make_unique<QQuickWindow>(); w->setScreen(screen); w->resize(32,32); w->setMaximumSize({64,64});
            return w;
        };
        // Private QPA screens exist only in this isolated process. No native
        // surface is created on a synthetic platform screen.
        auto wa=make(a->screen()),wb=make(b->screen());
        QVERIFY(requestHdrOutput(wa.get(),&prefs)); QVERIFY(requestHdrOutput(wb.get(),&prefs));
        HdrImageItem ui(wa->contentItem());
        QCOMPARE(ui.outputSettings().value("monitorKey").toString(),keyA);
        QVERIFY(ui.outputSettings().value("canRemember").toBool());
        QVERIFY(!ui.saveHdrCalibration("malformed",200,800,true));
        QVERIFY(ui.saveHdrCalibration(ui.outputSettings().value("revision").toString(),300,1200,true));
        QTRY_COMPARE(hdrOutputState(wa.get())->snapshot().white,300.0f);
        QCOMPARE(hdrOutputState(wb.get())->snapshot().white,100.0f);
        QQmlEngine engine;
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(SDR_COMPONENT_DIR "/../HdrCalibrationSettings.qml")));
        std::unique_ptr<QObject> card(component.createWithInitialProperties({{"controller",QVariant::fromValue<QObject*>(&ui)},{"width",300}}));
        QVERIFY2(card,qPrintable(component.errorString()));
        HdrOutputState::Native metadata{true,true,true}; metadata.white=80;
        QVERIFY(!hdrOutputState(wa.get())->observe(metadata));
        metadata.white=100;
        QVERIFY(hdrOutputState(wa.get())->observe(metadata));
        a->screen()->geometryChanged(a->screen()->geometry()); // Overlaps the native metadata invalidation.
        QCoreApplication::processEvents();
        QVERIFY(hdrOutputState(wa.get())->snapshot().nativeReview);
        QVERIFY(!hdrOutputState(wa.get())->snapshot().calibrated); // Saved profile must not bypass review.
        const auto beforeUiSave=hdrOutputRevision(wa.get());
        auto* save=card->findChild<QObject*>("hdrSaveMonitor"); QVERIFY(save);
        QVERIFY(QMetaObject::invokeMethod(save,"activate")); // Real QML -> real item -> shared settings/store -> output.
        QTRY_VERIFY(hdrOutputRevision(wa.get())>beforeUiSave);
        QTRY_COMPARE(hdrOutputState(wa.get())->snapshot().white,300.0f);
        QVERIFY(!hdrOutputState(wa.get())->snapshot().nativeReview); // Explicit UI confirmation clears it.
        auto wc=make(a->screen()); QVERIFY(requestHdrOutput(wc.get(),&prefs));
        QCOMPARE(hdrOutputState(wc.get())->snapshot().white,300.0f); // New window restores persisted profile, without env.
        const auto stale=ui.outputSettings().value("revision").toString();
        auto* duplicate=new Screen("profile-A","port-duplicate");
        QWindowSystemInterface::handleScreenAdded(duplicate);
        QVERIFY(!ui.outputSettings().value("canRemember").toBool());
        QVERIFY(!ui.saveHdrCalibration(stale,400,1600,true));
        QCoreApplication::processEvents();
        QVERIFY(!hdrOutputState(wa.get())->snapshot().calibrated);
        QCOMPARE(prefs.hdrMonitorCalibration(keyA).value("paperWhite").toFloat(),300.0f);
        QWindowSystemInterface::handleScreenRemoved(duplicate);
        QTRY_COMPARE(hdrOutputState(wa.get())->snapshot().white,300.0f);
        wa->setScreen(b->screen());
        QTRY_COMPARE(ui.outputSettings().value("monitorKey").toString(),keyB);
        QTRY_COMPARE(hdrOutputState(wa.get())->snapshot().white,100.0f);
        QVERIFY(!ui.saveHdrCalibration(stale,500,2000,true));
        QVERIFY(ui.saveHdrCalibration(ui.outputSettings().value("revision").toString(),400,1600,false));
        QCOMPARE(prefs.hdrMonitorCalibration(keyB).value("paperWhite").toFloat(),100.0f);
        auto* sdr=card->findChild<QObject*>("hdrUseSdr"); QVERIFY(sdr);
        QVERIFY(QMetaObject::invokeMethod(sdr,"activate"));
        QTRY_VERIFY(!hdrOutputState(wb.get())->snapshot().calibrated);
        QTRY_COMPARE(hdrMonitorSettings(wb.get()).value("code").toString(),QStringLiteral("disabled"));
        auto* forget=card->findChild<QObject*>("hdrForgetMonitor"); QVERIFY(forget);
        QVERIFY(QMetaObject::invokeMethod(forget,"activate"));
        QTRY_VERIFY(prefs.hdrMonitorCalibration(keyB).isEmpty());
        card.reset();
        QVERIFY(!prefs.hdrMonitorCalibration(keyA).isEmpty());
        // Detach the stack item before the window owns/destroys its content tree.
        ui.setParentItem(nullptr); ui.setParent(nullptr);
        wa.reset(); wb.reset(); wc.reset();
        QCoreApplication::processEvents();
        QTRY_COMPARE(viewerResourceBudget()->used().targets,quint64(0));
    }
    void fallbackStatusRefreshesWithLanguage()
    {
        using namespace Licasa;
        QQuickWindow window; window.resize(32,32); window.setMaximumSize({64,64});
        requestHdrOutput(&window);
        HdrImageItem observer(window.contentItem());
        const auto original=observer.outputSettings().value("message").toString();
        QVERIFY(!original.isEmpty());
        struct Translator final : QTranslator {
            bool isEmpty() const override { return false; }
            QString translate(const char* context,const char* source,const char*,int) const override {
                return QByteArray(context)=="HdrOutput" ? QStringLiteral("Översatt: ")+QString::fromUtf8(source) : QString();
            }
        } translator;
        QSignalSpy changed(&observer,&HdrImageItem::outputSettingsChanged);
        QVERIFY(QCoreApplication::installTranslator(&translator));
        auto reset=qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
        QTRY_VERIFY(changed.count()>0);
        QVERIFY(observer.outputSettings().value("message").toString().startsWith(QStringLiteral("Översatt: ")));
        QVERIFY(QCoreApplication::removeTranslator(&translator));
        QTRY_COMPARE(observer.outputSettings().value("message").toString(),original);
    }
    void outputCalibration()
    {
        const auto scene = Licasa::calibratedHdrOutput(200, 800, true);
        QCOMPARE(scene.headroom, 4.0f);
        QCOMPARE(scene.whiteScale, 2.5f);
        const auto display = Licasa::calibratedHdrOutput(200, 800, false);
        QCOMPARE(display.whiteScale, 1.0f);
        QCOMPARE(display.headroom, scene.headroom);
        for (float bad : {0.0f, -80.0f, 79.0f, 501.0f, float(qQNaN()), float(qInf())})
            QCOMPARE(Licasa::calibratedHdrOutput(bad, 1000, true).headroom, 0.0f);
        for (float bad : {0.0f, 199.0f, 200.0f, 10001.0f, float(qQNaN()), float(qInf())})
            QCOMPARE(Licasa::calibratedHdrOutput(200, bad, true).headroom, 0.0f);
        QCOMPARE(Licasa::calibratedHdrOutput(80, 10000, true).headroom, 8.0f);
    }
    void outputRequestAndFallback()
    {
        // Scoped environment: the test executable uses an isolated lab process.
        qunsetenv("LICASA_HDR_PAPER_WHITE_NITS"); qunsetenv("LICASA_HDR_PEAK_NITS");
        QQuickWindow noCalibration;
        QVERIFY(!Licasa::requestHdrOutput(&noCalibration));
        QVERIFY(noCalibration.property("_qt_sg_hdr_format").isNull());
        qputenv("LICASA_HDR_PAPER_WHITE_NITS", "200");
        qputenv("LICASA_HDR_PEAK_NITS", "800");
        QQuickWindow requested;
        requested.setMaximumSize(QSize(32,32));
        QVERIFY(Licasa::requestHdrOutput(&requested));
        QCOMPARE(requested.property("_qt_sg_hdr_format").toByteArray(), QByteArray("scrgb"));
        QVERIFY(!Licasa::requestHdrOutput(&requested)); // Preserve an existing request.
        auto targetPool = Licasa::viewerResourceBudget();
        const auto adapterKey = requested.property("licasaHdrAdapter").toByteArray();
        QVERIFY(!adapterKey.isEmpty());
        const auto available = qMin(targetPool->limits().targets - targetPool->used().targets,
                                   targetPool->adapterLimit(adapterKey) - targetPool->adapterUsed(adapterKey));
        auto pressure = targetPool->acquire({0, 0, 0, available}, adapterKey);
        QVERIFY(pressure);
        QQuickWindow pressureFallback;
        pressureFallback.setMaximumSize(QSize(32,32));
        QVERIFY(!Licasa::requestHdrOutput(&pressureFallback));
        QVERIFY(pressureFallback.property("_qt_sg_hdr_format").isNull());
        QVERIFY(pressureFallback.property("licasaHdrTargetPixels").isValid());
        QVERIFY(pressureFallback.graphicsDevice().isNull());
        QCOMPARE(Licasa::hdrMonitorSettings(&pressureFallback).value("code").toString(),QStringLiteral("memory"));
        pressure.reset();
        qputenv("QSG_RHI_HDR", "hdr10");
        QQuickWindow override;
        QVERIFY(!Licasa::requestHdrOutput(&override));
        qunsetenv("QSG_RHI_HDR");
        requested.resize(32,32);
        requested.show();
        QTest::qWait(100);
        // The request can fall back to SDR on llvmpipe/XCB. Never infer HDR
        // success from the dynamic property; ordinary drawing must stay usable.
        QVERIFY(!requested.grabWindow().isNull());
        Licasa::HdrImageItem candidate(requested.contentItem());
        candidate.setSize(QSizeF(32,32));
        candidate.setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE));
        candidate.setActive(true);
        requested.grabWindow();
        QTRY_VERIFY(candidate.outputReady());
        const auto actual = nativeSwapchainFormat(requested.swapChain());
        if (!actual || classifySurfaceFormat(*actual) != SurfaceFormat::LinearFp16) {
            QCOMPARE(candidate.outputWhiteScale(), 0.0f);
            QVERIFY(!candidate.testUsage()["ready"].toBool());
            QCOMPARE(candidate.testUsage()["decode"].toULongLong(), qulonglong(0));
        }
        QVERIFY(!Licasa::requestHdrOutput(&requested));
        requested.hide();
        qunsetenv("LICASA_HDR_PAPER_WHITE_NITS"); qunsetenv("LICASA_HDR_PEAK_NITS");
    }
    void windowCloseAndMove()
    {
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        auto first = std::make_unique<QQuickWindow>();
        auto second = std::make_unique<QQuickWindow>();
        first->setVulkanInstance(&instance);
        second->setVulkanInstance(&instance);
        first->resize(64,64); second->resize(64,64);
        const auto targetBytes = Licasa::hdrTargetPlan(QSize(64,64), 1)->bytes;
        QVERIFY(Licasa::reserveHdrTargets(first.get(), QSize(64,64), 1));
        QVERIFY(Licasa::reserveHdrTargets(second.get(), QSize(64,64), 1));
        QCOMPARE(Licasa::viewerResourceBudget()->used().targets, 2 * targetBytes);
        auto item = std::make_unique<Licasa::HdrImageItem>();
        item->setParentItem(first->contentItem());
        item->setSize(QSizeF(64,64));
        item->setTestHeadroom(4);
        item->setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE));
        item->setActive(true);
        first->show();
        QTRY_VERIFY(item->testUsage()["frames"].toULongLong() > 0);
        QCOMPARE(item->testUsage()["threaded"].toBool(), qEnvironmentVariable("QSG_RENDER_LOOP") == "threaded");
        std::atomic_int oversizedFrames{0}, frameWidth{0}, frameHeight{0};
        std::atomic_bool adapterMatches{true};
        const auto selectedAdapter = first->property("licasaHdrAdapter").toByteArray();
        auto* firstWindow = first.get();
        connect(firstWindow, &QQuickWindow::beforeRendering, firstWindow, [firstWindow, selectedAdapter, &adapterMatches, &oversizedFrames, &frameWidth, &frameHeight] {
            if (!firstWindow->swapChain()) return;
            const auto actual = Licasa::describeHdrAdapter(firstWindow->rhi());
            if (!actual || actual->key != selectedAdapter) adapterMatches = false;
            const QSize pixels = firstWindow->swapChain()->currentPixelSize();
            frameWidth = pixels.width(); frameHeight = pixels.height();
            if (!pixels.isEmpty() && !Licasa::hdrTargetsCover(firstWindow, pixels)) ++oversizedFrames;
        }, Qt::DirectConnection);
        // Observe normal renders on their owner thread. Forcing grabWindow()
        // while XCB resize events are pending can use a stale Qt framebuffer.
        first->resize(16,16);
        QVERIFY(!Licasa::hdrTargetReadbackReady(first.get()));
        QTRY_COMPARE(frameWidth.load(), 16);
        QTRY_COMPARE(frameHeight.load(), 16);
        QTRY_VERIFY(Licasa::hdrTargetReadbackReady(first.get()));
        QVERIFY(!first->grabWindow().isNull());
        first->resize(128,128);
        QVERIFY(!Licasa::hdrTargetReadbackReady(first.get()));
        QTRY_COMPARE(first->size(), QSize(64,64));
        QTRY_COMPARE(frameWidth.load(), 64);
        QTRY_COMPARE(frameHeight.load(), 64);
        QTRY_VERIFY(Licasa::hdrTargetReadbackReady(first.get()));
        QVERIFY(!first->grabWindow().isNull());
        QCOMPARE(Licasa::viewerResourceBudget()->used().targets, 2 * targetBytes);
        QCOMPARE(oversizedFrames.load(), 0);
        QVERIFY(adapterMatches.load());
        first->hide();
        QVERIFY(!Licasa::hdrTargetReadbackReady(first.get()));
        QCOMPARE(Licasa::viewerResourceBudget()->used().targets, 2 * targetBytes);
        first->show();
        QVERIFY(!Licasa::hdrTargetReadbackReady(first.get()));
        QTRY_VERIFY(Licasa::hdrTargetReadbackReady(first.get()));
        QVERIFY(!first->grabWindow().isNull());
        item->setParentItem(second->contentItem());
        second->show();
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        second->grabWindow();
        first.reset();
        QCOMPARE(Licasa::viewerResourceBudget()->used().targets, targetBytes);
        item->setSource(QUrl::fromLocalFile(QStringLiteral("/nonexistent-close.jpg")));
        item.reset();
        Licasa::HdrImageItem liveObserver;
        QTRY_COMPARE(liveObserver.testUsage()["gpu"].toULongLong(), qulonglong(0));
        QTRY_COMPARE(liveObserver.testUsage()["owners"].toInt(), 0);
        second.reset();
        QCOMPARE(Licasa::viewerResourceBudget()->used().targets, quint64(0));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        Licasa::HdrImageItem observer;
        QTRY_COMPARE(observer.testUsage()["decode"].toULongLong(), qulonglong(0));
        QTRY_COMPARE(observer.testUsage()["retained"].toULongLong(), qulonglong(0));
        QTRY_COMPARE(observer.testUsage()["gpu"].toULongLong(), qulonglong(0));
    }
    void twoWindowsKeepIndependentPhotoHeadroom()
    {
        using namespace Licasa;
        const auto pool = viewerResourceBudget();
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        auto createWindow = [&] {
            auto window = std::make_unique<QQuickWindow>();
            window->setVulkanInstance(&instance);
            window->resize(64, 64);
            return window;
        };
        auto first = createWindow();
        auto second = createWindow();
        QVERIFY(reserveHdrTargets(first.get(), QSize(64, 64), 1));
        QVERIFY(reserveHdrTargets(second.get(), QSize(64, 64), 1));
        const auto source = QUrl::fromLocalFile(GAINMAP_FIXTURE);
        const auto makePhoto = [&source](QQuickWindow* window, float headroom) {
            auto item = std::make_unique<HdrImageItem>(window->contentItem());
            item->setSize(QSizeF(64, 64));
            item->setTestHeadroom(headroom);
            item->setSource(source);
            item->setActive(true);
            window->show();
            return item;
        };
        auto low = makePhoto(first.get(), 1.2f);
        auto high = makePhoto(second.get(), 1.6f);
        QTRY_COMPARE(low->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        QTRY_COMPARE(high->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        const auto lowFrame = first->grabWindow(), highFrame = second->grabWindow();
        QVERIFY(!lowFrame.isNull() && !highFrame.isNull());
        const auto red = [](const QImage& frame) {
            return frame.pixelColor(frame.width() / 2, frame.height() / 2).red();
        };
        QVERIFY(red(lowFrame) > 0);
        QVERIFY(red(highFrame) > red(lowFrame) + 12);
        const auto expected = highFrame.pixelColor(highFrame.width() / 2, highFrame.height() / 2);
        const auto onePhotoBytes = quint64(32 * 32 * 8);
        QCOMPARE(pool->used().upload, 2 * onePhotoBytes);

        low.reset();
        first.reset();
        QTRY_COMPARE(pool->used().upload, onePhotoBytes);
        QCOMPARE(high->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        QCOMPARE(second->grabWindow().pixelColor(highFrame.width() / 2, highFrame.height() / 2), expected);
        first = createWindow();
        QVERIFY(reserveHdrTargets(first.get(), QSize(64, 64), 1));
        low = makePhoto(first.get(), 1.2f);
        QTRY_COMPARE(low->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        QCOMPARE(red(first->grabWindow()), red(lowFrame));
        low.reset();
        high.reset();
        first.reset();
        second.reset();
        QTRY_COMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().retained, quint64(0));
        QCOMPARE(pool->used().targets, quint64(0));
    }
    void replaceWhilePeerWaitsForUpload_data()
    {
        QTest::addColumn<quint32>("seed");
        QTest::newRow("seed-8") << quint32(8);
        QTest::newRow("seed-42") << quint32(42);
        QTest::newRow("seed-21496") << quint32(21496);
        QTest::newRow("seed-20261110") << quint32(20261110);
    }
    void replaceWhilePeerWaitsForUpload()
    {
        QFETCH(quint32, seed);
        using namespace Licasa;
        SamePathGainMaps files;
        QVERIFY(files.initialize());
        const auto pool = viewerResourceBudget();
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        const QColor fallback(19, 59, 29);
        const auto newWindow = [&] {
            auto window = std::make_unique<QQuickWindow>();
            window->setVulkanInstance(&instance);
            window->resize(64, 64);
            window->setColor(fallback);
            return window;
        };
        const auto open = [&](QQuickWindow& window, int revision, float headroom) {
            auto photo = std::make_unique<HdrImageItem>(window.contentItem());
            photo->setSize(QSizeF(64, 64));
            photo->setTestHeadroom(headroom);
            photo->setSource(files.source(revision));
            photo->setActive(true);
            window.show();
            return photo;
        };
        const auto center = [](QQuickWindow& window) {
            const auto frame = window.grabWindow();
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };

        auto survivor = newWindow();
        QVERIFY(reserveHdrTargets(survivor.get(), QSize(64, 64), 1));
        auto resident = open(*survivor, 1, 1.6f);
        QTRY_COMPARE(resident->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        const auto gray = center(*survivor);
        QVERIFY(gray.isValid() && gray != fallback);
        const auto onePhotoBytes = quint64(32 * 32 * 8);
        QCOMPARE(pool->used().upload, onePhotoBytes);
        const auto limits = pool->limits();
        const auto gpuLimit = pool->gpuLimit();
        const auto totalLimit = pool->totalLimit();
        const auto restoreLimit = [&] { pool->setLimits(limits, gpuLimit, totalLimit); };
        const auto reset = qScopeGuard(restoreLimit);
        auto blocked = limits;
        blocked.upload = onePhotoBytes;
        pool->setLimits(blocked, gpuLimit, totalLimit);

        auto peer = newWindow();
        QVERIFY(reserveHdrTargets(peer.get(), QSize(64, 64), 1));
        auto pending = open(*peer, 1, 1.2f);
        QTRY_VERIFY(pending->testUsage()["decoded"].toBool());
        QVERIFY(!pending->testUsage()["ready"].toBool());
        QCOMPARE(center(*peer), fallback);
        QVERIFY(files.replace());
        // Close the first peer with an old raster waiting for GPU admission.
        peer->close();
        pending.reset();
        peer.reset();
        QTRY_COMPARE(pool->used().retained, onePhotoBytes);
        QCOMPARE(resident->source(), files.source(1));
        QCOMPARE(resident->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        QCOMPARE(center(*survivor), gray);

        peer = newWindow();
        QVERIFY(reserveHdrTargets(peer.get(), QSize(64, 64), 1));
        pending = open(*peer, 2, 1.2f);
        QTRY_VERIFY(pending->testUsage()["decoded"].toBool());
        QVERIFY(!pending->testUsage()["ready"].toBool());

        // Replay bursts on the reopened peer while the other window continues
        // to present revision 1. End every seed on the same known revision.
        QRandomGenerator random(seed);
        const auto missing = QUrl::fromLocalFile(files.directory.filePath("missing.jpg"));
        for (int revision = 3; revision < 15; ++revision) {
            pending->setSource(random.bounded(3) == 0 ? missing : files.source(revision));
            QTest::qWait(random.bounded(8));
            QVERIFY(!pending->testUsage()["ready"].toBool());
            QCOMPARE(center(*peer), fallback);
        }
        pending->setSource(files.source(15));
        QTRY_VERIFY(pending->testUsage()["decoded"].toBool());
        QVERIFY(!pending->testUsage()["ready"].toBool());
        QCOMPARE(pending->source(), files.source(15));
        QCOMPARE(center(*peer), fallback);
        QCOMPARE(center(*survivor), gray);
        restoreLimit();
        QTRY_COMPARE(pending->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        const auto red = center(*peer);
        QVERIFY(red.isValid() && red.red() > gray.red() && red.green() + 50 < gray.green());
        QCOMPARE(center(*survivor), gray);
        QCOMPARE(pool->used().upload, 2 * onePhotoBytes);

        pending.reset();
        peer.reset();
        resident.reset();
        survivor.reset();
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().targets, quint64(0));
    }
    void presentedRevisionRejectsReplacedSource_data()
    {
        QTest::addColumn<QString>("trigger");
        QTest::addColumn<bool>("receipt");
        for (const bool receipt : {false, true})
            for (const char* trigger : {"headroom", "item-visibility", "activation", "recreated-item"}) {
                // Without a provider receipt a new probe item pins the file it is given.
                if (!receipt && qstrcmp(trigger, "recreated-item") == 0) continue;
                QTest::addRow("%s-%s", receipt ? "provider-receipt" : "probe-pin", trigger)
                    << QString::fromLatin1(trigger) << receipt;
            }
    }
    void presentedRevisionRejectsReplacedSource()
    {
        // A window that has not reloaded still presents the previous file's
        // SDR base. Re-deriving HDR must not draw the replacement over it.
        QFETCH(QString, trigger);
        QFETCH(bool, receipt);
        using namespace Licasa;
        SamePathGainMaps files;
        QVERIFY(files.initialize());
        const auto pool = viewerResourceBudget();
        auto& receipts = PresentedStillSources::instance();
        receipts.setEnabled(true);
        const auto disable = qScopeGuard([&] { receipts.setEnabled(false); });
        // Stand in for the SDR provider: each request names the file it decoded.
        const auto request = [&](int revision) {
            return QStringLiteral("image://licasa/%1/%2?licasa_stage=full").arg(revision)
                .arg(QString::fromLatin1(QUrl::toPercentEncoding(files.path)));
        };
        const auto present = [&](HdrImageItem& item, int revision) {
            if (receipt) {
                // Qt hands the provider this form of the request.
                receipts.record(QUrl(request(revision)).toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1),
                                files.path, externalFileIdentityAtPath(files.path));
                item.setPresentedRequest(request(revision));
            }
            item.setSource(files.source(revision));
        };
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        const QColor fallback(19, 59, 29);
        auto window = std::make_unique<QQuickWindow>();
        window->setVulkanInstance(&instance);
        window->resize(64, 64);
        window->setColor(fallback);
        QVERIFY(reserveHdrTargets(window.get(), QSize(64, 64), 1));
        const auto create = [&] {
            auto item = std::make_unique<HdrImageItem>(window->contentItem());
            item->setSize(QSizeF(64, 64));
            item->setTestHeadroom(1.6f);
            return item;
        };
        auto photo = create();
        present(*photo, 1);
        photo->setActive(true);
        window->show();
        const auto center = [&] {
            const auto frame = window->grabWindow();
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };
        QTRY_COMPARE(photo->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        const auto gray = center();
        QVERIFY(gray.isValid() && gray != fallback && qAbs(gray.red() - gray.green()) < 3);

        QVERIFY(files.replace());
        if (trigger == "headroom") {
            photo->setTestHeadroom(1.2f);
        } else if (trigger == "item-visibility") {
            photo->setVisible(false);
            photo->setVisible(true);
        } else if (trigger == "activation") {
            photo->setActive(false);
            photo->setActive(true);
        } else {
            // The viewer's loader builds a new item for the same presented base.
            // Only the provider's receipt can still name that base's file.
            photo.reset();
            photo = create();
            if (receipt) photo->setPresentedRequest(request(1));
            photo->setSource(files.source(1));
            photo->setActive(true);
        }
        QTRY_COMPARE(photo->photoStatus(), HdrImageItem::PhotoStatus::Unavailable);
        QVERIFY(!photo->testUsage()["ready"].toBool());
        QCOMPARE(center(), fallback);
        QTest::qWait(300); // No retry may admit the replacement under revision 1.
        QCOMPARE(photo->photoStatus(), HdrImageItem::PhotoStatus::Unavailable);
        QCOMPARE(center(), fallback);
        if (receipt) {
            // A request the provider never answered names no file.
            photo->setPresentedRequest(request(7));
            photo->setSource(files.source(7));
            QTest::qWait(100);
            QCOMPARE(photo->photoStatus(), HdrImageItem::PhotoStatus::Unavailable);
            QCOMPARE(center(), fallback);
        }

        present(*photo, 2);
        QTRY_COMPARE(photo->photoStatus(), HdrImageItem::PhotoStatus::Hdr);
        const auto red = center();
        QVERIFY(red.isValid() && red.red() > gray.red() && red.green() + 50 < gray.green());

        photo.reset();
        window.reset();
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().targets, quint64(0));
    }
    void cameraSizedGainMap()
    {
        using namespace Licasa;
        const auto path = qEnvironmentVariable("LICASA_HDR_COMPOUND_CAMERA_FIXTURE");
        if (path.isEmpty()) QSKIP("The retained Pixel 9 Pro XL fixture is optional.");
        const auto pool = viewerResourceBudget();
        const auto reset = qScopeGuard([] { configureHdrMemoryBudget(8192); });
        configureHdrMemoryBudget(384);
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        QQuickWindow window;
        window.setVulkanInstance(&instance);
        window.resize(128, 96);
        QVERIFY(reserveHdrTargets(&window, QSize(128, 96), 1));
        auto item = std::make_unique<HdrImageItem>(window.contentItem());
        item->setSize(QSizeF(128, 96));
        item->setSource(QUrl::fromLocalFile(path));
        item->setTestHeadroom(4);
        item->setActive(true);
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), HdrImageItem::PhotoStatus::Unavailable, 12000);
        QCOMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, quint64(0));
        QCOMPARE(pool->used().upload, quint64(0));

        configureHdrMemoryBudget(8192);
        window.update();
        QTRY_COMPARE_WITH_TIMEOUT(item->photoStatus(), HdrImageItem::PhotoStatus::Hdr, 12000);
        QCOMPARE(pool->used().retained, quint64(2268) * 4032 * 8);
        QCOMPARE(pool->used().upload, pool->used().retained);
        item.reset();
        window.releaseResources();
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QTRY_COMPARE(pool->used().retained, quint64(0));
        QTRY_COMPARE(pool->used().upload, quint64(0));
    }
    void renderNavigationAndRetirement()
    {
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
        QVERIFY(instance.create());
        QQuickRenderControl control;
        QQuickWindow window(&control);
        window.setVulkanInstance(&instance);
        window.resize(32, 32);
        window.contentItem()->setSize(QSizeF(32,32)); // Redirected windows are never shown.
        window.setColor(Qt::black);
        QVERIFY(Licasa::reserveHdrTargets(&window, QSize(32,32), 1));
        const auto reservedTargets = Licasa::viewerResourceBudget()->used().targets;
        QVERIFY(reservedTargets > 0);
        QVERIFY(control.initialize());
        auto* rhi = control.rhi();
        QVERIFY(rhi && rhi->backend() == QRhi::Vulkan);
        auto texture = std::unique_ptr<QRhiTexture>(rhi->newTexture(QRhiTexture::RGBA16F, QSize(32,32), 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        QVERIFY(texture->create());
        // Match a window's depth attachment so Qt Quick's front-to-back opaque
        // pass has valid depth ordering when a layer is disabled on fallback.
        auto depth = std::unique_ptr<QRhiRenderBuffer>(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(32,32)));
        QVERIFY(depth->create());
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(texture.get())};
        description.setDepthStencilBuffer(depth.get());
        auto target = std::unique_ptr<QRhiTextureRenderTarget>(rhi->newTextureRenderTarget(description));
        auto pass = std::unique_ptr<QRhiRenderPassDescriptor>(target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        QVERIFY(target->create());
        window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));
        auto item = std::make_unique<Licasa::HdrImageItem>(window.contentItem());
        item->setSize(QSizeF(32,32));
        auto frame = [&](bool deliver = true) {
            control.polishItems();
            control.beginFrame();
            control.sync();
            control.render();
            QRhiReadbackResult result;
            auto* updates = rhi->nextResourceUpdateBatch();
            updates->readBackTexture(QRhiReadbackDescription(texture.get()), &result);
            control.commandBuffer()->resourceUpdate(updates);
            control.endFrame();
            if (deliver) QCoreApplication::processEvents();
            return result.data;
        };
        auto firstRed = [](const QByteArray& bytes) {
            if (bytes.size() < 8) return -1.0f;
            qfloat16 channel;
            std::memcpy(&channel, bytes.constData(), 2);
            return float(channel);
        };
        item->setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE));
        item->setActive(true);
        QCOMPARE(firstRed(frame()), 0.0f); // No native HDR surface: no work/admission.
        QCOMPARE(item->testUsage()["decode"].toULongLong(), qulonglong(0));
        QVERIFY(!item->testUsage()["ready"].toBool());
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Sdr);
        item->setTestHeadroom(4);
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Preparing);
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Preparing);
        const float four = firstRed(frame());
        QVERIFY(four > 2 && four < 2.2);
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Hdr);

        QTemporaryDir statusFixtures;
        QVERIFY(statusFixtures.isValid());
        const auto ordinary = statusFixtures.filePath("ordinary.jpg");
        QImage sdr(32, 32, QImage::Format_RGB32);
        sdr.fill(Qt::red);
        QVERIFY(sdr.save(ordinary));
        const auto malformed = statusFixtures.filePath("malformed.jpg");
        QFile fixture(GAINMAP_FIXTURE);
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        auto bytes = fixture.readAll();
        const auto signature = bytes.indexOf(QByteArray("MPF\0", 4));
        QVERIFY(signature >= 0 && signature + 4 < bytes.size());
        bytes[signature + 4] = 'X';
        QFile damaged(malformed);
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        QCOMPARE(damaged.write(bytes), bytes.size());
        damaged.close();

        // A completed render can still be queued for GUI delivery when the
        // next source takes ownership of the photo status.
        item->setTestHeadroom(2);
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QVERIFY(firstRed(frame(false)) > 1);
        item->setSource(QUrl::fromLocalFile(ordinary));
        QCoreApplication::processEvents();
        QVERIFY(item->photoStatus() != Licasa::HdrImageItem::PhotoStatus::Hdr);
        QTRY_COMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Sdr);
        item->setSource(QUrl::fromLocalFile(malformed));
        QTRY_COMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Unavailable);
        item->setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE));
        item->setTestHeadroom(4);
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QVERIFY(firstRed(frame()) > 2);
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Hdr);
        // A real preference signal wakes a request previously denied by total
        // user memory, while existing target leases stay fully charged.
        {
            Licasa::ViewerPreferences preferences;
            preferences.setMaximumImageMemoryMiB(384);
            Licasa::bindHdrMemoryPolicy(preferences);
            auto reset = qScopeGuard([] { Licasa::configureHdrMemoryBudget(8192); });
            item->setActive(false); frame(); frame();
            QTRY_COMPARE(item->testUsage()["gpu"].toULongLong(), qulonglong(0));
            const auto key = window.property("licasaHdrAdapter").toByteArray();
            auto pressure = Licasa::viewerResourceBudget()->acquire({0, 0, 0, 64 * 1024 * 1024}, key);
            QVERIFY(pressure);
            item->setActive(true); frame();
            QTest::qWait(100);
            QVERIFY(!item->testUsage()["ready"].toBool());
            preferences.setMaximumImageMemoryMiB(8192);
            frame();
            QTRY_VERIFY(item->testUsage()["ready"].toBool());
            QVERIFY(std::abs(firstRed(frame()) - four) < 0.01f);
        }
        // Use the actual reusable QML layer and shipped shader over a resident
        // HDR photo. Read the GPU result rather than mirroring the shader code.
        QCOMPARE(item->outputWhiteScale(), 1.0f);
        auto base = frame();
        auto channel = [](const QByteArray& bytes, int c) {
            qfloat16 value;
            std::memcpy(&value, bytes.constData() + c * 2, 2);
            return float(value);
        };
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import "."
            SdrCompositionLayer {
                width: 32; height: 32
                property color sampleColor: "white"
                Rectangle { anchors.fill: parent; color: parent.sampleColor }
            }
        )", QUrl::fromLocalFile(QStringLiteral(SDR_COMPONENT_DIR "/readback.qml")));
        std::unique_ptr<QObject> overlay(component.create());
        QVERIFY2(overlay, qPrintable(component.errorString()));
        auto* ui = qobject_cast<QQuickItem*>(overlay.get());
        QVERIFY(ui);
        ui->setParentItem(window.contentItem());
        auto verifyUi = [&](QColor color, float white, float opacity) {
            ui->setProperty("sampleColor", color);
            ui->setProperty("whiteScale", white);
            ui->setOpacity(opacity);
            frame(); // Layer creation/release and texture updates settle here.
            const auto bytes = frame();
            // Qt Quick's ordinary rectangle material has 8-bit premultiplied
            // vertex colors. Account for that real source quantization before
            // testing the layer's transfer function at FP16 precision.
            const QRgb premultiplied = qPremultiply(color.rgba());
            const float sourceAlpha = qAlpha(premultiplied) / 255.0f;
            const float alpha = sourceAlpha * opacity;
            for (int c = 0; c < 3; ++c) {
                const float premult = (c == 0 ? qRed(premultiplied) : c == 1 ? qGreen(premultiplied) : qBlue(premultiplied)) / 255.0f;
                const float encoded = sourceAlpha > 0 ? premult / sourceAlpha : 0;
                const float value = white > 0
                    ? (encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f)) * white
                    : encoded;
                const float expected = value * alpha + channel(base, c) * (1 - alpha);
                if (std::abs(channel(bytes, c) - expected) > 0.00015f + std::abs(expected) * 0.002f) {
                    qWarning() << "SDR blend channel" << c << "actual" << channel(bytes,c) << "expected" << expected
                               << "color" << color << "scale" << white << "opacity" << opacity;
                    return false;
                }
            }
            const float expectedAlpha = alpha + channel(base,3) * (1 - alpha);
            return std::abs(channel(bytes,3) - expectedAlpha) < 0.002f;
        };
        // White tracks 80/200/500-nit calibration; gray and the two sides of
        // the sRGB knee catch gamma shortcuts. Alpha and group opacity catch
        // applying the transfer function to premultiplied input/output.
        for (float white : {1.0f, 2.5f, 6.25f}) {
            QVERIFY(verifyUi(QColor(255,255,255), white, 1));
            QVERIFY(verifyUi(QColor(128,10,11), white, 1));
            QVERIFY(verifyUi(QColor(128,64,204,128), white, 1));
            QVERIFY(verifyUi(QColor(128,64,204,128), white, 0.6f));
            QVERIFY(verifyUi(QColor(255,255,255,0), white, 1));
        }
        QVERIFY(verifyUi(QColor(128,64,204,128), 0, 1)); // No conversion on SDR fallback.
        QVERIFY(verifyUi(QColor(128,64,204), 0, 1));
        // Exercise the real dialog too: its captured scene is already linear
        // and may exceed 1.0. Only its SDR controls pass through the conversion.
        ui->setProperty("sampleColor", QColor(Qt::white));
        ui->setProperty("whiteScale", 6.25);
        ui->setOpacity(1);
        frame(); frame();
        QQuickItem backdropScene(window.contentItem());
        backdropScene.setSize(QSizeF(32,32));
        ui->setParentItem(&backdropScene); // Match Main's unlayered viewerScene container.
        QQmlComponent dialogComponent(&engine,
            QUrl::fromLocalFile(QStringLiteral(SDR_COMPONENT_DIR "/../ShortcutsDialog.qml")));
        std::unique_ptr<QObject> dialog(dialogComponent.createWithInitialProperties({
            {"backdropSource", QVariant::fromValue(&backdropScene)}, {"sdrWhiteScale", 2.5}, {"smoothMotionEnabled", false}}));
        QVERIFY2(dialog, qPrintable(dialogComponent.errorString()));
        auto* dialogItem = qobject_cast<QQuickItem*>(dialog.get());
        QVERIFY(dialogItem);
        dialogItem->setParentItem(window.contentItem());
        dialogItem->setZ(3000); // Match Main.qml's shortcuts Loader.
        dialogItem->setSize(QSizeF(32,32));
        frame(); frame();
        const float scrimTransmission = 1 - 166 / 255.0f; // #A6000000
        const auto dialogFrame = frame();
        const float hdrBackdrop = firstRed(dialogFrame);
        QVERIFY2(std::abs(hdrBackdrop - 6.25f * scrimTransmission) < 0.015f,
                 qPrintable(QString("HDR backdrop actual %1 expected %2; root opacity %3 blur %4")
                    .arg(hdrBackdrop).arg(6.25f * scrimTransmission).arg(dialogItem->opacity())
                    .arg(dialogItem->property("blurAvailable").toBool())));
        ui->setProperty("whiteScale", 0);
        dialogItem->setProperty("sdrWhiteScale", 0);
        frame(); frame();
        QVERIFY(std::abs(firstRed(frame()) - scrimTransmission) < 0.005f);
        dialog.reset();
        QQmlComponent overlayComponent(&engine,
            QUrl::fromLocalFile(QStringLiteral(SDR_COMPONENT_DIR "/../HdrOverlayCompositionHost.qml")));
        std::unique_ptr<QObject> overlayPolicy(overlayComponent.createWithInitialProperties({{"whiteScale", 2.5}}));
        QVERIFY2(overlayPolicy, qPrintable(overlayComponent.errorString()));
        auto* overlayPolicyItem = qobject_cast<QQuickItem*>(overlayPolicy.get());
        QVERIFY(overlayPolicyItem);
        overlayPolicyItem->setParentItem(window.contentItem());
        QQmlComponent popupComponent(&engine);
        popupComponent.setData(R"(
            import QtQuick
            import QtQuick.Controls
            Item {
                width: 32; height: 32
                function openPopup() { popup.open() }
                Popup {
                    id: popup; width: 32; height: 32; padding: 0; margins: 0
                    popupType: Popup.Item
                    enter: null; exit: null
                    background: Rectangle { color: Qt.rgba(128/255,128/255,128/255,1) }
                }
            }
        )", QUrl());
        std::unique_ptr<QObject> popup(popupComponent.create());
        QVERIFY2(popup, qPrintable(popupComponent.errorString()));
        auto* popupItem = qobject_cast<QQuickItem*>(popup.get());
        QVERIFY(popupItem);
        popupItem->setParentItem(window.contentItem());
        QVERIFY(QMetaObject::invokeMethod(popup.get(), "openPopup"));
        frame(); frame();
        const float gray = std::pow((128 / 255.0f + 0.055f) / 1.055f, 2.4f);
        const float popupRed = firstRed(frame());
        QVERIFY2(std::abs(popupRed - gray * 2.5f) < 0.003f,
                 qPrintable(QString("Popup actual %1 expected %2").arg(popupRed).arg(gray * 2.5f)));
        overlayPolicy.reset(); // Restores the ordinary SDR overlay while still open.
        frame(); frame();
        const float restoredPopup = firstRed(frame());
        QVERIFY2(std::abs(restoredPopup - 128 / 255.0f) < 0.003f,
                 qPrintable(QString("Restored popup actual %1").arg(restoredPopup)));
        popup.reset();
        ui->setParentItem(window.contentItem());
        // Preserve alpha on transparent window pixels too; an opaque HDR
        // background alone would hide an incorrect output alpha channel.
        item->setActive(false);
        window.setColor(Qt::transparent);
        ui->setVisible(false);
        frame(); base = frame();
        QCOMPARE(channel(base,3), 0.0f);
        ui->setVisible(true);
        QVERIFY(verifyUi(QColor(128,64,204,128), 2.5f, 0.6f));
        QVERIFY(verifyUi(QColor(255,255,255,0), 2.5f, 1));
        Licasa::HdrOutputState changedOutput;
        QVERIFY(changedOutput.calibrate(changedOutput.snapshot().revision,200,800));
        changedOutput.invalidate();
        const auto fallback = Licasa::hdrSurfacePolicy(true,true,changedOutput.snapshot());
        QCOMPARE(fallback.headroom,0.0f);
        QVERIFY(verifyUi(QColor(128,64,204), fallback.whiteScale, 1)); // SDR on a retained FP16 surface.
        overlay.reset();
        window.setColor(Qt::black);
        item->setActive(true);
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        frame(); frame();
        QVERIFY(std::abs(firstRed(frame()) - four) < 0.01f); // HDR was never gamma-converted.
        item->setTestPaperWhite(200, 800); // Same 4x headroom, different paper white.
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        const float scaled = firstRed(frame());
        QVERIFY(std::abs(scaled - four * 2.5f) < 0.02f);
        item->setTestPaperWhite(80, 320);
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QVERIFY(std::abs(firstRed(frame()) - four) < 0.01f);
        item->setTestHeadroom(2);
        QCOMPARE(firstRed(frame()), 0.0f); // SDR remains underneath during replacement.
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        const float two = firstRed(frame());
        QVERIFY(std::abs(two * 2 - four) < 0.01f);
        // Rapid navigation coalesces and a rejected source cannot revive old HDR.
        for (int i = 0; i < 100; ++i) {
            item->setSource(QUrl::fromLocalFile(QStringLiteral("/nonexistent-hdr-image.jpg")));
            item->setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE));
        }
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QVERIFY(firstRed(frame()) > 1);
        item->setSource(QUrl::fromLocalFile(QStringLiteral("/nonexistent-hdr-image.jpg")));
        QCOMPARE(firstRed(frame()), 0.0f);
        item->setActive(false);
        frame(); frame();
        QTRY_COMPARE(item->testUsage()["gpu"].toULongLong(), qulonglong(0));
        auto held = item->testHoldGpu(Licasa::viewerResourceBudget()->limits().upload);
        QVERIFY(held);
        item->setSource(QUrl::fromLocalFile(GAINMAP_FIXTURE));
        item->setActive(true);
        QTRY_VERIFY(item->testUsage()["decoded"].toBool());
        QVERIFY(!item->testUsage()["ready"].toBool());
        QCOMPARE(firstRed(frame()), 0.0f);
        held.reset();
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QVERIFY(firstRed(frame()) > 1);
        item->setTestHeadroom(0);
        QCOMPARE(firstRed(frame()), 0.0f);
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Sdr);
        // Reject a source changed after decode but before render-thread upload.
        SamePathGainMaps changed;
        QVERIFY(changed.initialize());
        item->setSource(changed.source(1));
        item->setTestHeadroom(4);
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QVERIFY(changed.replace());
        QCOMPARE(firstRed(frame()), 0.0f);
        QTRY_COMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Unavailable);
        QVERIFY(!item->testUsage()["ready"].toBool());

        SamePathGainMaps replacement;
        QVERIFY(replacement.initialize());
        item->setSource(replacement.source(1));
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Preparing);
        QVERIFY(std::abs(firstRed(frame(false)) - four) < 0.01f);
        QVERIFY(replacement.replace());
        item->setSource(replacement.source(2));
        QCOMPARE(firstRed(frame(false)), 0.0f);
        // Deliver the previous frame only after revision 2 owns the item.
        QCoreApplication::processEvents();
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Preparing);
        QTRY_VERIFY(item->testUsage()["ready"].toBool());
        const auto red = frame();
        QVERIFY(std::abs(channel(red, 0) - 4.85616f) < 0.025f);
        QVERIFY(std::abs(channel(red, 1) + 0.16673f) < 0.01f);
        QVERIFY(std::abs(channel(red, 2) + 0.07785f) < 0.01f);
        QCOMPARE(item->photoStatus(), Licasa::HdrImageItem::PhotoStatus::Hdr);

        item->setActive(false);
        item.reset();
        frame(); frame(); // Retire after final use, while the window stays alive.
        Licasa::HdrImageItem liveObserver;
        QTRY_COMPARE(liveObserver.testUsage()["owners"].toInt(), 0);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        control.invalidate(); // Drain on the render owner before device destruction.
        QCOMPARE(Licasa::viewerResourceBudget()->used().targets, reservedTargets); // Native/window lifetime, not item lifetime.
        window.setRenderTarget(QQuickRenderTarget());
        target.reset(); pass.reset(); depth.reset(); texture.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        // A fresh observer shares the process pool and sees all previous leases.
        Licasa::HdrImageItem observer;
        QTRY_COMPARE(observer.testUsage()["decode"].toULongLong(), qulonglong(0));
        QTRY_COMPARE(observer.testUsage()["retained"].toULongLong(), qulonglong(0));
        QTRY_COMPARE(observer.testUsage()["gpu"].toULongLong(), qulonglong(0));
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
    QCoreApplication::setApplicationName("ViewerHdrTests");
    ViewerHdrTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_viewer_hdr.moc"
