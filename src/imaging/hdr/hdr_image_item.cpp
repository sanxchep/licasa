#include "hdr_image_item.h"
#include <QQuickWindow>
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
#include "../../../tools/gainmap-lab/async_decoder.h"
#include "../../../tools/hdr-probe/gainmap_photo.h"
#include "../../../tools/hdr-probe/surface_format.h"
#include "vulkan_frame_fence.h"
#include "output_policy.h"
#include "output_negotiation.h"
#include "render_target_budget.h"
#include "adapter_budget.h"
#include "render_generation.h"
#include "../presented_still_sources.h"
#include <QDir>
#include <QtGui/qrgbafloat.h>
#include <cstring>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGTexture>
#include <QTimer>
#include <QThread>
#include <QPointer>
#include <QDynamicPropertyChangeEvent>
#include <rhi/qrhi.h>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
#include <QJsonDocument>
#include <QJsonObject>
#endif
#endif

namespace Licasa {
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
namespace Lab = GainmapLab;
namespace {
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
std::atomic<QQuickWindow*> diagnosticWindow{nullptr};
std::atomic_bool holdDiagnosticFence{false};
std::atomic_int diagnosticRefusals{0};

void recordDiagnostic(QQuickWindow* window, const char* event, QJsonObject details = {})
{
    const auto pool = viewerResourceBudget();
    const auto used = pool->used();
    details["event"] = QLatin1String(event);
    details["window"] = QString::number(quintptr(window), 16);
    details["revision"] = QString::number(hdrOutputRevision(window));
    details["budget_revision"] = QString::number(pool->revision());
    details["decode"] = qint64(used.decode);
    details["retained"] = qint64(used.retained);
    details["upload"] = qint64(used.upload);
    details["targets"] = qint64(used.targets);
    qInfo("LICASA_RASTER_RETRY %s", QJsonDocument(details).toJson(QJsonDocument::Compact).constData());
}

// Delay collection on one window. Complete is reported only by the actual
// submitted Vulkan fence. Error handling and teardown still wait on it.
struct DiagnosticFence final : Lab::GpuFence {
    QQuickWindow* window;
    std::shared_ptr<Lab::GpuFence> real;
    bool completed = false;
    DiagnosticFence(QQuickWindow* target, std::shared_ptr<Lab::GpuFence> fence)
        : window(target), real(std::move(fence)) {}
    Status poll() noexcept override
    {
        if (holdDiagnosticFence.load()) {
            return Status::Pending;
        }
        const auto status = real->poll();
        if (status == Status::Complete) {
            completed = true;
            recordDiagnostic(window, "fence_complete");
        }
        return status;
    }
    bool wait() noexcept override { return real->wait(); }
};
#endif
#ifdef LICASA_HDR_RENDER_TEST
std::atomic<int> testTextureFailures{0};
std::atomic_bool testHoldFences{false}, testLoseFence{false};
// Stands in front of a submitted fence. Waiting always defers to the real
// fence, so teardown still proves that the GPU finished.
struct TestFence final : Lab::GpuFence {
    std::shared_ptr<Lab::GpuFence> real;
    const bool lost;
    TestFence(std::shared_ptr<Lab::GpuFence> fence, bool lose) : real(std::move(fence)), lost(lose) {}
    Status poll() noexcept override {
        if (lost) return Status::DeviceLost;
        return testHoldFences.load() ? Status::Pending : real->poll();
    }
    bool wait() noexcept override { return real->wait(); }
};
#endif
// Render-thread-only; window connections keep it alive after an item disappears.
struct RenderState : std::enable_shared_from_this<RenderState> {
    static inline std::atomic<int> live{0};
    QMetaObject::Connection frameConnection;
    QMetaObject::Connection invalidationConnection;
    std::shared_ptr<RenderState> keepAlive;
    RenderState() { ++live; }
    ~RenderState() {
        QObject::disconnect(frameConnection);
        QObject::disconnect(invalidationConnection);
        --live;
    }
    QQuickWindow* window = nullptr;
    QRhi* rhi = nullptr;
    Lab::GpuRetirement retired;
    std::vector<std::shared_ptr<void>> pending;
    std::function<void()> reportPhotoFrame;
    bool stopped = false, lossReported = false;
    std::weak_ptr<HdrRenderGeneration> generation;
    quint64 generationToken = 0;
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
    std::shared_ptr<DiagnosticFence> diagnosticFence;
#endif
    void retire(std::shared_ptr<void> resource) {
        if (stopped) return; // Invalidation already established device quiescence.
        keepAlive = shared_from_this();
        pending.push_back(std::move(resource));
    }
    void afterFrame() {
        if (auto report = std::exchange(reportPhotoFrame, {})) report();
        if (!retired.collect() && (!waitForHdrDevice(rhi) || !retired.shutdown()))
            qFatal("HDR fence polling could not establish safe teardown");
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
        if (diagnosticFence && diagnosticFence->completed) {
            recordDiagnostic(window, "lease_released");
            diagnosticFence.reset();
        }
#endif
        if (retired.deviceLost() && !lossReported) {
            lossReported = true;
            QMetaObject::invokeMethod(window, [target = window, weak = generation, token = generationToken] {
                if (auto current = weak.lock(); current && current->matches(token) && !hdrTargetRecovering(target))
                    requestHdrDeviceRecovery(target);
            }, Qt::QueuedConnection);
        }
        if (pending.empty()) {
            if (retired.pending()) QMetaObject::invokeMethod(window, [target = window] {
                QTimer::singleShot(16, target, &QQuickWindow::update);
            }, Qt::QueuedConnection);
            else keepAlive.reset();
            return;
        }
        auto group = std::make_shared<std::vector<std::shared_ptr<void>>>();
        group->swap(pending);
        auto fence = submitFrameFence(rhi);
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
        if (fence && window == diagnosticWindow.load() && holdDiagnosticFence.load() &&
            !diagnosticFence) {
            diagnosticFence = std::make_shared<DiagnosticFence>(window, std::move(fence));
            fence = diagnosticFence;
            recordDiagnostic(window, "fence_submitted");
        }
#endif
#ifdef LICASA_HDR_RENDER_TEST
        if (fence) fence = std::make_shared<TestFence>(std::move(fence), testLoseFence.exchange(false));
#endif
        if (!fence) QMetaObject::invokeMethod(window, &QQuickWindow::update, Qt::QueuedConnection);
        if (!fence || !retired.retire(group, fence)) {
            // Bounded pressure/error fallback. Never free queued GPU resources
            // merely because retirement admission failed.
            if (!waitForHdrDevice(rhi) || !retired.shutdown())
                qFatal("HDR render retirement could not drain");
        }
        if (retired.pending()) QMetaObject::invokeMethod(window, [target = window] {
            QTimer::singleShot(16, target, &QQuickWindow::update);
        }, Qt::QueuedConnection);
        else keepAlive.reset();
    }
    void invalidate() {
        reportPhotoFrame = {};
        if (!stopped && rhi) {
            if (!waitForHdrDevice(rhi) || !retired.shutdown())
                qFatal("HDR scene graph teardown could not drain");
        }
        stopped = true;
        pending.clear();
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
        diagnosticFence.reset();
#endif
        rhi = nullptr;
        keepAlive.reset();
    }
};
HdrOutputPolicy outputPolicy(QQuickWindow* target)
{
    if (!target || qstrcmp(qVersion(), QT_VERSION_STR) || !target->rhi() || target->rhi()->backend() != QRhi::Vulkan
        || target->rhi()->isDeviceLost() || hdrTargetRecovering(target)) return {};
    const auto format = nativeSwapchainFormat(target->swapChain());
    if (!format) return {};
    const auto surface = classifySurfaceFormat(*format);
    if (surface != SurfaceFormat::LinearFp16
        && !(surface == SurfaceFormat::PassThrough && hdrWaylandSurfaceTagged(target))) return {};
    // The actual FP16 surface still requires linear SDR composition when HDR
    // admission/calibration is lost, including basic-loop rebind refusal.
    const HdrOutputPolicy sdrOnLinear{0, 1};
    if (!hdrWaylandPermitsHdr(target)) return sdrOnLinear;
    if (!hdrTargetsCover(target, target->swapChain()->currentPixelSize())
        || nativeSwapchainBufferCount(target->swapChain()) > 8
        || target->swapChain()->sampleCount() != 1) return sdrOnLinear;
    const auto adapter = describeHdrAdapter(target->rhi());
    if (!adapter || adapter->key != target->property("licasaHdrAdapter").toByteArray()) return sdrOnLinear;
    const auto info = target->swapChain()->hdrInfo();
    if (info.luminanceBehavior != QRhiSwapChainHdrInfo::SceneReferred &&
        info.luminanceBehavior != QRhiSwapChainHdrInfo::DisplayReferred) return {};
    if (const auto output = hdrOutputState(target))
        return hdrSurfacePolicy(true, info.luminanceBehavior == QRhiSwapChainHdrInfo::SceneReferred, output->snapshot());
    const auto calibration = calibratedHdrOutput(target->property("licasaHdrPaperWhiteNits").toFloat(),
                               target->property("licasaHdrPeakNits").toFloat(),
                               info.luminanceBehavior == QRhiSwapChainHdrInfo::SceneReferred);
    return calibration.headroom > 1 ? calibration : sdrOnLinear;
}
struct DrawStats {
    std::atomic<quint64> frames{0}, visibleFrames{0};
    std::atomic_bool threaded{false};
};
struct ImageNode final : QSGOpacityNode {
    std::shared_ptr<DrawStats> stats;
    QQuickWindow* window;
    HdrOutputPolicy policy;
    bool testOutput;
    std::shared_ptr<HdrRenderGeneration> generation;
    quint64 generationToken, outputRevision;
    std::shared_ptr<RenderState> render;
    std::shared_ptr<Lab::LeasedResource<QSGTexture>> texture;
    std::shared_ptr<Lab::RequestCoordinator::Upload> upload;
    QSGImageNode* image;
    std::function<void()> firstPhotoFrame;
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
    QString diagnosticSource, diagnosticRequest;
#endif
    ImageNode(QQuickWindow* window, std::shared_ptr<RenderState> state,
              std::shared_ptr<Lab::RequestCoordinator::Upload> source, QSGTexture* tex, HdrOutputPolicy value, bool test, std::shared_ptr<DrawStats> observation, std::shared_ptr<HdrRenderGeneration> gen, quint64 token,
              std::function<void()> report)
        : stats(std::move(observation)), window(window), policy(value), testOutput(test), generation(std::move(gen)), generationToken(token), outputRevision(hdrOutputRevision(window)), render(std::move(state)),
          texture(std::make_shared<Lab::LeasedResource<QSGTexture>>(source, std::unique_ptr<QSGTexture>(tex))),
          upload(std::move(source)), image(window->createImageNode()), firstPhotoFrame(std::move(report)) {
        setFlag(UsePreprocess);
        image->setOwnsTexture(false);
        image->setTexture(tex);
        appendChildNode(image);
    }
    void preprocess() override {
        ++stats->frames;
        stats->threaded = QThread::currentThread() != QCoreApplication::instance()->thread();
        const bool visible = hdrOutputRevision(window) == outputRevision
            && generation->matches(generationToken) && !render->stopped && render->rhi
            && !render->rhi->isDeviceLost() && (testOutput || outputPolicy(window) == policy);
        setOpacity(visible ? 1 : 0);
        if (visible) {
            ++stats->visibleFrames;
            if (firstPhotoFrame && !image->rect().isEmpty())
                render->reportPhotoFrame = std::move(firstPhotoFrame);
        }
    }
    ~ImageNode() override
    {
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
        if (window == diagnosticWindow.load() && holdDiagnosticFence.load()) {
            recordDiagnostic(window, "texture_retired",
                             {{"source", diagnosticSource}, {"request", diagnosticRequest}});
        }
#endif
        render->retire(std::move(texture));
    }
};
}
struct HdrImageItem::State {
    std::shared_ptr<DrawStats> stats = std::make_shared<DrawStats>();
    Lab::AsyncDecoder decoder{{}, {}, nullptr, viewerResourceBudget()};
    std::shared_ptr<Lab::RequestCoordinator::Upload> upload;
    std::shared_ptr<RenderState> render;
    std::vector<QMetaObject::Connection> connections;
    QPointer<QQuickWindow> observedWindow;
    QTimer retry;
    int retries = 0;
    HdrOutputPolicy policy;
    bool outputReady = false;
    PhotoStatus photoStatus = PhotoStatus::None;
    quint64 serial = 0;
    quint64 epoch = 0;
    quint64 revision = 0;
    quint64 budgetRevision = 0;
    std::shared_ptr<HdrRenderGeneration> generation = std::make_shared<HdrRenderGeneration>();
    quint64 acceptedGeneration = 0, acceptedOutputRevision = 0;
    std::optional<ExternalFileIdentity> expected;
#ifdef LICASA_HDR_RENDER_TEST
    HdrOutputPolicy testPolicy;
#endif
};
#else
struct HdrImageItem::State {};
#endif

HdrImageItem::HdrImageItem(QQuickItem* parent) : QQuickItem(parent), state_(std::make_unique<State>())
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    setFlag(ItemHasContents);
    connect(this, &QQuickItem::windowChanged, this, &HdrImageItem::observeWindow);
    state_->retry.setSingleShot(true);
    state_->retry.setInterval(250);
    connect(&state_->retry, &QTimer::timeout, this, [this] {
        const int retries = state_->retries + 1;
        if (state_->decoder.coordinator().image().isNull()) {
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
            recordRetryDiagnostic("retry_executed", retries);
#endif
            refresh();
            state_->retries = retries;
        } else {
            state_->retries = retries;
            tryUpload();
        }
    });
    connect(&state_->decoder, &Lab::AsyncDecoder::settled, this,
            [this](quint64 serial, bool accepted, const QString&, Lab::DecodeFailure failure) {
        if (serial != state_->serial || !active_
            || state_->acceptedOutputRevision != hdrOutputRevision(window())
            || !state_->generation->matches(state_->acceptedGeneration)) return;
        if (accepted) {
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
            recordRetryDiagnostic("raster_admitted", state_->retries);
#endif
            tryUpload();
        } else if (failure == Lab::DecodeFailure::NoGainMap) {
            setPhotoStatus(PhotoStatus::Sdr);
        } else {
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
            if (failure == Lab::DecodeFailure::Memory && window() && window() == diagnosticWindow.load()) {
                recordRetryDiagnostic("raster_refused", state_->retries);
                ++diagnosticRefusals;
            }
#endif
            // The raster allowance is known only after the header is read, so
            // a refusal there is pressure even when a default request would fit.
            // The usual holder is this item's previous texture awaiting its fence.
            const bool pressure = failure == Lab::DecodeFailure::Memory
                || !viewerResourceBudget()->canAcquire(Lab::RequestCoordinator::decodeAllowance({}));
            if (pressure && state_->retries < 8 &&
                externalFileIdentityAtPath(source_.toLocalFile()) == state_->expected) {
                state_->retry.start();
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
                recordRetryDiagnostic("retry_scheduled", state_->retries + 1);
#endif
            } else {
                setPhotoStatus(PhotoStatus::Unavailable);
            }
        }
    });
    if (window()) observeWindow(window());
#endif
}
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
void HdrImageItem::armRetirementDiagnostic(QQuickWindow* target)
{
    diagnosticRefusals.store(0);
    holdDiagnosticFence.store(target != nullptr);
    diagnosticWindow.store(target);
}

void HdrImageItem::releaseDiagnosticFence()
{
    holdDiagnosticFence.store(false);
}

int HdrImageItem::diagnosticRasterRefusals()
{
    return diagnosticRefusals.load();
}

void HdrImageItem::recordRetryDiagnostic(const char* event, int retry) const
{
    if (!window() || window() != diagnosticWindow.load()) {
        return;
    }
    recordDiagnostic(window(), event, {{"source", source_.toLocalFile()},
                                       {"request", presentedRequest_},
                                       {"serial", qint64(state_->serial)},
                                       {"retry", retry},
                                       {"default_fits", viewerResourceBudget()->canAcquire(
                                            Lab::RequestCoordinator::decodeAllowance({}))}});
}
#endif
HdrImageItem::~HdrImageItem()
{
    disconnect(this, nullptr, this, nullptr);
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    if (state_->observedWindow) state_->observedWindow->removeEventFilter(this);
    for (const auto& connection : state_->connections) disconnect(connection);
#endif
}
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
quint64 HdrImageItem::visibleHdrFrames() const
{
    return state_->stats->visibleFrames.load();
}
#endif
void HdrImageItem::setSource(const QUrl& source)
{
    if (source_ == source) return;
    source_ = source;
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    pinSource();
#endif
    refresh();
    emit sourceChanged();
}
void HdrImageItem::setPresentedRequest(const QString& request)
{
    if (presentedRequest_ == request) return;
    presentedRequest_ = request;
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    // A later stage of the same file keeps the current rendition.
    if (pinSource()) refresh();
#endif
    emit presentedRequestChanged();
}
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
bool HdrImageItem::pinSource()
{
    // Pin the file once for the presented base. A calibration, visibility or
    // output change must not pick up a replacement that SDR has not loaded.
    std::optional<ExternalFileIdentity> pinned;
    if (source_.isLocalFile()) {
        const QString path = QDir::cleanPath(source_.toLocalFile());
        if (!presentedRequest_.isEmpty()) {
            const QString id = QUrl(presentedRequest_).toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
            const QString revision = QStringLiteral("licasa-revision-") + id.section(QLatin1Char('/'), 0, 0);
            // Both bindings follow one request. Ignore the moment between them.
            if (source_.fragment() == revision) {
                const auto receipt = PresentedStillSources::instance().find(id);
                if (receipt && QDir::cleanPath(receipt->path) == path) pinned = receipt->identity;
            }
        }
#ifdef LICASA_HDR_RENDER_TEST
        else pinned = externalFileIdentityAtPath(path); // Probes drive the item without a provider.
#endif
    }
    if (pinned == state_->expected) return false;
    state_->expected = pinned;
    return true;
}
#endif
void HdrImageItem::setActive(bool active)
{
    if (active_ == active) return;
    active_ = active;
    refresh();
    emit activeChanged();
}
float HdrImageItem::outputWhiteScale() const
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    return state_->policy.whiteScale;
#else
    return 0;
#endif
}
bool HdrImageItem::outputReady() const
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    return state_->outputReady;
#else
    return false;
#endif
}
QVariantMap HdrImageItem::outputSettings() const
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    return hdrMonitorSettings(window());
#else
    return {};
#endif
}
HdrImageItem::PhotoStatus HdrImageItem::photoStatus() const
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    return state_->photoStatus;
#else
    return PhotoStatus::None;
#endif
}
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
void HdrImageItem::setPhotoStatus(PhotoStatus status)
{
    if (state_->photoStatus == status) return;
    state_->photoStatus = status;
    emit photoStatusChanged();
}
#endif
bool HdrImageItem::saveHdrCalibration(const QString& revision, float white, float peak, bool remember)
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    bool ok=false; const auto token=revision.toULongLong(&ok);
    return ok && setHdrMonitorSettings(window(),token,white,peak,remember);
#else
    Q_UNUSED(revision); Q_UNUSED(white); Q_UNUSED(peak); Q_UNUSED(remember); return false;
#endif
}
bool HdrImageItem::useSdr(const QString& revision)
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    bool ok=false; const auto token=revision.toULongLong(&ok);
    return ok && useSdrForMonitor(window(),token);
#else
    Q_UNUSED(revision); return false;
#endif
}
bool HdrImageItem::forgetHdrCalibration(const QString& revision)
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    bool ok=false; const auto token=revision.toULongLong(&ok);
    return ok && forgetHdrMonitorSettings(window(),token);
#else
    Q_UNUSED(revision); return false;
#endif
}
void HdrImageItem::refresh()
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    ++state_->revision;
    state_->retry.stop();
    state_->retries = 0;
    state_->upload.reset();
    state_->decoder.cancel();
    state_->serial = 0;
    auto status = active_ && !source_.isEmpty() ? PhotoStatus::Sdr : PhotoStatus::None;
    bool showing = window() && window()->isVisible();
#ifdef LICASA_HDR_RENDER_TEST
    showing |= state_->testPolicy.headroom > 1;
#endif
    if (active_ && state_->acceptedOutputRevision == hdrOutputRevision(window()) && state_->generation->matches(state_->acceptedGeneration) && !hdrTargetRecovering(window())
        && state_->policy.headroom > 1 && showing && window() && isVisible() && source_.isLocalFile()) {
        const QString path = source_.toLocalFile();
        const auto identity = state_->expected;
        if (identity) {
            Lab::Limits limits;
            limits.rasterPixels = Lab::maximumRasterPixels;
            state_->serial = state_->decoder.submit(path, *identity, state_->policy.headroom, limits);
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
            recordRetryDiagnostic("decode_submitted", 0);
#endif
        }
        status = state_->serial ? PhotoStatus::Preparing : PhotoStatus::Unavailable;
    }
    setPhotoStatus(status);
    update();
#endif
}
void HdrImageItem::tryUpload()
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    if (!active_ || state_->upload || state_->acceptedOutputRevision != hdrOutputRevision(window()) || !state_->generation->matches(state_->acceptedGeneration)
        || hdrTargetRecovering(window())) return;
    const auto adapter = window() ? window()->property("licasaHdrAdapter").toByteArray() : QByteArray();
    const bool accepted = state_->decoder.withUpload([this](QImage& image) {
        QString primaries, error;
        if (!GainmapPhotoProbe::prepareUpload(image, primaries, error)) return false;
        // Reconstruction is relative to paper white. Scene-referred scRGB uses
        // 80 nits per unit; scale RGB only, preserving opaque alpha and negatives.
        const float scale = state_->policy.whiteScale;
        if (!std::isfinite(scale) || scale <= 0) return false;
        if (scale != 1) for (int y = 0; y < image.height(); ++y) {
            auto* row = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                qfloat16 pixel[4];
                std::memcpy(pixel, row + x * 8, 8);
                for (int c = 0; c < 3; ++c) pixel[c] = qfloat16(float(pixel[c]) * scale);
                std::memcpy(row + x * 8, pixel, 8);
            }
        }
        state_->upload = state_->decoder.uploadLease();
        return true;
    }, adapter);
    if (!accepted) {
        const auto pool = viewerResourceBudget();
        const quint64 bytes = state_->decoder.coordinator().retainedBytes();
        if (state_->retries < 8 && bytes && !pool->canAcquire({0, 0, bytes}, adapter)) {
            state_->retry.start();
        } else {
            setPhotoStatus(PhotoStatus::Unavailable);
        }
    }
    update();
#endif
}
void HdrImageItem::observeWindow(QQuickWindow* target)
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    if (state_->observedWindow) state_->observedWindow->removeEventFilter(this);
    state_->observedWindow = target;
    if (target) target->installEventFilter(this);
    for (const auto& connection : state_->connections) disconnect(connection);
    state_->connections.clear();
    const auto epoch = ++state_->epoch;
    state_->generation = std::make_shared<HdrRenderGeneration>();
    state_->acceptedGeneration = 0;
    const auto generation = state_->generation;
    state_->policy = {};
    state_->outputReady = false;
    emit outputWhiteScaleChanged();
    emit outputSettingsChanged();
    state_->render.reset();
    refresh();
    if (!target) return;
    state_->connections.push_back(connect(target, &QQuickWindow::sceneGraphInitialized, this,
        [generation] { generation->advance(); }, Qt::DirectConnection));
    // This callback only queues GUI state updates; no decoder is touched on the
    // render thread. A moved item rejects callbacks from its previous window.
    state_->connections.push_back(connect(target, &QQuickWindow::beforeSynchronizing, this, [this, target, epoch, generation] {
        const auto token = generation->current();
        const auto outputRevision = hdrOutputRevision(target);
        const auto revision = state_->revision;
        const auto budgetRevision = viewerResourceBudget()->revision();
        HdrOutputPolicy policy = outputPolicy(target);
        // Let the GUI reveal content only after a real surface (including an
        // SDR fallback) is known. A request alone cannot choose a UI transfer.
        const auto format = nativeSwapchainFormat(target->swapChain());
        const bool rendererReady = target->rendererInterface()->graphicsApi() == QSGRendererInterface::Software
            || (target->rhi() && !target->rhi()->isDeviceLost()
                && (target->rhi()->backend() != QRhi::Vulkan || format.has_value()));
        const bool tagPending = format && classifySurfaceFormat(*format) == SurfaceFormat::PassThrough
            && hdrWaylandAwaitsSurfaceTag(target);
        const bool ready = !hdrTargetRecovering(target) && rendererReady && !tagPending;
#ifdef LICASA_HDR_RENDER_TEST
        if (target->rhi() && target->rhi()->backend() == QRhi::Vulkan && state_->testPolicy.headroom > 1)
            policy = state_->testPolicy;
#endif
        QMetaObject::invokeMethod(this, [this, target, epoch, generation, token, revision, budgetRevision, policy, ready, outputRevision] {
            if (state_->epoch != epoch || state_->revision != revision || window() != target || !generation->matches(token) || hdrOutputRevision(target) != outputRevision) return;
            const bool policyChanged = state_->policy != policy || state_->outputReady != ready
                || state_->acceptedGeneration != token || state_->acceptedOutputRevision != outputRevision;
            state_->acceptedGeneration = token;
            state_->acceptedOutputRevision = outputRevision;
            const bool budgetChanged = state_->budgetRevision != budgetRevision;
            state_->budgetRevision = budgetRevision;
            if (policyChanged) {
                state_->policy = policy;
                state_->outputReady = ready;
                emit outputWhiteScaleChanged();
                refresh();
            } else if (budgetChanged && !state_->upload) {
                state_->retries = 0;
                if (!state_->decoder.coordinator().image().isNull()) tryUpload();
                else if (!state_->decoder.busy()) refresh();
            }
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection));
    state_->connections.push_back(connect(target, &QQuickWindow::visibleChanged, this, &HdrImageItem::refresh));
    const QPointer<HdrImageItem> guarded(this);
    state_->connections.push_back(connect(target, &QQuickWindow::sceneGraphInvalidated, target, [target, guarded, epoch, generation] {
        const auto token = generation->advance();
        // The render thread touches only a shared token/window. The item may
        // disappear independently while an actual device loss invalidates Qt.
        QMetaObject::invokeMethod(target, [guarded, epoch, generation, token] {
            auto* item = guarded.data();
            if (!item || item->state_->epoch != epoch || !generation->matches(token)) return;
            item->state_->policy = {}; item->state_->outputReady = false;
            item->state_->acceptedGeneration = 0;
            emit item->outputWhiteScaleChanged();
            item->refresh();
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection));
    state_->connections.push_back(connect(this, &QQuickItem::visibleChanged, this, &HdrImageItem::refresh));
#else
    Q_UNUSED(target);
#endif
}
bool HdrImageItem::eventFilter(QObject* watched, QEvent* event)
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    if (watched == window() && event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "licasaHdrSettingsRevision")
        emit outputSettingsChanged();
    if (watched == window() && event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "licasaHdrOutputRevision") {
        state_->policy = {}; state_->outputReady = false;
        state_->acceptedOutputRevision = 0;
        emit outputWhiteScaleChanged();
        refresh();
    }
#endif
    return QQuickItem::eventFilter(watched, event);
}
void HdrImageItem::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickItem::geometryChange(now, before);
    update();
}
QSGNode* HdrImageItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    auto* node = static_cast<ImageNode*>(old);
    HdrOutputPolicy current = outputPolicy(window());
#ifdef LICASA_HDR_RENDER_TEST
    if (window() && window()->rhi() && window()->rhi()->backend() == QRhi::Vulkan && state_->testPolicy.headroom > 1)
        current = state_->testPolicy;
#endif
    if (state_->acceptedOutputRevision != hdrOutputRevision(window()) || !state_->generation->matches(state_->acceptedGeneration) || !state_->upload || !active_ || !window() || current != state_->policy || current.headroom <= 1) { delete node; return nullptr; }
    if (!node || node->upload != state_->upload) {
        if (!state_->expected || externalFileIdentityAtPath(source_.toLocalFile()) != state_->expected) {
            delete node;
            const auto revision = state_->revision;
            QMetaObject::invokeMethod(this, [this, revision] {
                if (state_->revision != revision) return;
                state_->upload.reset();
                state_->decoder.cancel();
                state_->serial = 0;
                setPhotoStatus(PhotoStatus::Unavailable);
                update();
            }, Qt::QueuedConnection);
            return nullptr;
        }
        delete node;
        if (!state_->render || state_->render->stopped) {
            auto render = std::make_shared<RenderState>();
            render->window = window();
            render->rhi = window()->rhi();
            render->generation = state_->generation;
            render->generationToken = state_->acceptedGeneration;
            state_->render = render;
            const std::weak_ptr<RenderState> weak = render;
            render->frameConnection = connect(window(), &QQuickWindow::afterFrameEnd, window(), [weak] {
                if (const auto owner = weak.lock()) owner->afterFrame();
            }, Qt::DirectConnection);
            render->invalidationConnection = connect(window(), &QQuickWindow::sceneGraphInvalidated, window(), [weak] {
                if (const auto owner = weak.lock()) owner->invalidate();
            }, Qt::DirectConnection);
        }
        QSGTexture* texture = nullptr;
#ifdef LICASA_HDR_RENDER_TEST
        int failures = testTextureFailures.load();
        while (failures > 0 && !testTextureFailures.compare_exchange_weak(failures, failures - 1)) {}
        if (failures <= 0)
#endif
        texture = window()->createTextureFromImage(state_->upload->raster->image,
                                                   QQuickWindow::TextureHasAlphaChannel);
        const auto revision = state_->revision;
        const auto outputRevision = state_->acceptedOutputRevision;
        const auto generation = state_->generation;
        const auto token = state_->acceptedGeneration;
        const QPointer<HdrImageItem> guarded(this);
        const auto report = [target = window(), guarded, revision, outputRevision, generation, token](PhotoStatus status) {
            QMetaObject::invokeMethod(target, [guarded, revision, outputRevision, generation, token, status] {
                auto* item = guarded.data();
                if (item && item->state_->revision == revision && generation->matches(token)
                    && hdrOutputRevision(item->window()) == outputRevision)
                    item->setPhotoStatus(status);
            }, Qt::QueuedConnection);
        };
        if (!texture) {
            // Keep neither the raster nor its GPU allowance for a photo that
            // cannot be drawn. A new request starts again from the file.
            QMetaObject::invokeMethod(this, [this, revision] {
                if (state_->revision != revision) return;
                ++state_->revision; // A later frame's report belongs to no request.
                state_->upload.reset();
                state_->decoder.cancel();
                state_->serial = 0;
                setPhotoStatus(PhotoStatus::Unavailable);
                update();
            }, Qt::QueuedConnection);
            return nullptr;
        }
        bool testOutput = false;
#ifdef LICASA_HDR_RENDER_TEST
        testOutput = state_->testPolicy.headroom > 1;
#endif
        node = new ImageNode(window(), state_->render, state_->upload, texture, state_->policy, testOutput, state_->stats, generation, token,
                             [report] { report(PhotoStatus::Hdr); });
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
        node->diagnosticSource = source_.toLocalFile();
        node->diagnosticRequest = presentedRequest_;
#endif
    }
    node->image->setRect(boundingRect());
    node->image->setFiltering(QSGTexture::Linear);
    return node;
#else
    delete old;
    return nullptr;
#endif
}
#ifdef LICASA_HDR_RENDER_TEST
void HdrImageItem::setTestHeadroom(float headroom)
{
    state_->testPolicy = headroom > 1 ? HdrOutputPolicy{headroom, 1} : HdrOutputPolicy{};
    state_->policy = state_->testPolicy;
    state_->acceptedGeneration = state_->generation->current();
    state_->acceptedOutputRevision = hdrOutputRevision(window());
    emit outputWhiteScaleChanged();
    refresh();
}
void HdrImageItem::setTestPaperWhite(float white, float peak)
{
    state_->testPolicy = calibratedHdrOutput(white, peak, true);
    state_->policy = state_->testPolicy;
    state_->acceptedGeneration = state_->generation->current();
    state_->acceptedOutputRevision = hdrOutputRevision(window());
    emit outputWhiteScaleChanged();
    refresh();
}
QVariantMap HdrImageItem::testUsage() const
{
    const auto bytes = viewerResourceBudget()->used();
    return {{"decode", qulonglong(bytes.decode)}, {"retained", qulonglong(bytes.retained)},
            {"gpu", qulonglong(bytes.upload)}, {"targets", qulonglong(bytes.targets)}, {"ready", bool(state_->upload)}, {"decoded", !state_->decoder.coordinator().image().isNull()}, {"frames", qulonglong(state_->stats->frames.load())},
            {"threaded", state_->stats->threaded.load()}, {"owners", RenderState::live.load()}, {"generation", qulonglong(state_->acceptedGeneration)}};
}

void HdrImageItem::setTestFaults(TestFaults faults)
{
    testTextureFailures = faults.textureFailures;
    testHoldFences = faults.holdFences;
    testLoseFence = faults.loseFence;
}
int HdrImageItem::testPendingTextureFailures() { return testTextureFailures.load(); }

std::shared_ptr<void> HdrImageItem::testHoldGpu(quint64 bytes)
{
    return viewerResourceBudget()->acquire({0, 0, bytes}, window() ? window()->property("licasaHdrAdapter").toByteArray() : QByteArray());
}

#endif
}
