#include "render_target_budget.h"
#include "adapter_budget.h"
#include "output_negotiation.h"
#include "target_handoff.h"
#include "render_generation.h"
#include <QTimer>
#include <QDebug>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QVariant>
#include <QThread>
#include <QEvent>
#include <rhi/qrhi.h>

namespace Licasa {
namespace {
class TargetWindowGuard final : public QObject {
public:
    TargetWindowGuard(QQuickWindow* window, QSize logical, QSize pixels,
                      std::shared_ptr<GainmapLab::ResourceBudget> pool, quint64 bytes,
                      QByteArray adapter, GainmapLab::ResourceBudget::Lease lease)
        : QObject(window), window_(window), logical_(logical), pixels_(pixels), pool_(pool),
          handoff_(pool, bytes, adapter, std::move(lease)), currentAdapter_(std::move(adapter))
    {
        window_->setProperty("licasaHdrTargetGuard", QVariant::fromValue<QObject*>(this));
        window_->installEventFilter(this);
        connect(window_, &QWindow::widthChanged, this, [this] { ++epoch_; clamp(); });
        connect(window_, &QWindow::heightChanged, this, [this] { ++epoch_; clamp(); });
        connect(window_, &QWindow::screenChanged, this, [this] { ++epoch_; clamp(); });
        connect(window_, &QWindow::visibleChanged, this, [this] {
            ++epoch_;
            if (window_->isVisible() && !busy_.load()
                && window_->property("licasaHdrRecoveryState").toByteArray() == "cancelled") blocked_ = false;
        });
        connect(window_, &QQuickWindow::sceneGraphInitialized, this, [this] { generation_.advance(); }, Qt::DirectConnection);
        connect(window_, &QQuickWindow::beforeSynchronizing, this, [this] {
            threaded_ = QThread::currentThread() != window_->thread();
            syncedEpoch_ = epoch_; // GUI is blocked during scene-graph sync.
            syncedGeneration_ = generation_.current();
            if (busy_.load() || !window_->rhi() || window_->rhi()->isDeviceLost()) return;
            const auto actual = describeHdrAdapter(window_->rhi());
            if (actual && actual->key != currentAdapter_) {
                if (!blocked_.exchange(true)) {
                    const auto token = generation_.current();
                    QMetaObject::invokeMethod(this, [this, token, key = actual->key] {
                        if (generation_.matches(token) && !busy_.load()) request(key);
                    }, Qt::QueuedConnection);
                }
            } else if (actual && actual->key == currentAdapter_) blocked_ = false;
        }, Qt::DirectConnection);
        connect(window_, &QQuickWindow::afterFrameEnd, this, [this] {
            const QSize pixels = window_->swapChain() ? window_->swapChain()->currentPixelSize() : QSize();
            const auto epoch = syncedEpoch_;
            const auto generation = syncedGeneration_;
            QMetaObject::invokeMethod(this, [this, pixels, epoch, generation] {
                if (epoch_ != epoch || !generation_.matches(generation) || blocked_.load()) return;
                lastFrameGeneration_ = generation;
                lastFrame_ = pixels;
                lastFrameEpoch_ = epoch;
            }, Qt::QueuedConnection);
        }, Qt::DirectConnection);
        connect(window_, &QQuickWindow::sceneGraphInvalidated, this, [this] {
            const auto token = generation_.advance();
            const bool lost = window_->rhi() && window_->rhi()->isDeviceLost();
            if (lost) blocked_ = true;
            QMetaObject::invokeMethod(this, [this, token, lost] {
                if (!generation_.matches(token)) return;
                ++epoch_; lastFrame_ = {}; lastFrameEpoch_ = 0;
                if (lost && !busy_.load() && automaticRetries_++ < 2) request(currentAdapter_);
            }, Qt::QueuedConnection);
        }, Qt::DirectConnection);
        clamp();
    }
    bool readbackReady() const {
        return !recovering() && generation_.matches(lastFrameGeneration_)
            && window_->isVisible() && window_->isExposed() && window_->isSceneGraphInitialized()
            && lastFrameEpoch_ == epoch_ && !lastFrame_.isEmpty()
            && lastFrame_ == window_->size() * window_->devicePixelRatio();
    }
    bool recovering() const { return blocked_.load() || busy_.load(); }
    bool keepsWindowAlive() const { return restoring_ && originalVisibility_ != QWindow::Hidden; }
    bool recover() { return request(busy_.load() && wanted_ ? wanted_->adapter.key : currentAdapter_); }
    bool surface(bool hdr) { return request(busy_.load() && wanted_ ? wanted_->adapter.key : currentAdapter_, hdr); }
    bool request(const QByteArray& adapter, std::optional<bool> hdrIntent = {}) {
        // Qt's basic loop only tears its QRhi down on actual loss/window deletion.
        // Never pretend a hide/cache release made a live basic-loop device safe.
        if (!busy_.load() && ((!window_->handle() && !hdrIntent.has_value())
            || (window_->isSceneGraphInitialized() && !threaded_.load()))) return false;
        const auto format = window_->property("_qt_sg_hdr_format").toByteArray();
        if (!format.isEmpty() && format != "scrgb") return false;
        const auto selection = findHdrAdapter(window_, adapter);
        if (!selection) return false;
        wanted_ = *selection;
        if (hdrIntent || hdrOutputState(window_)) hdrIntent_ = true;
        if (hdrIntent) hdrRequested_ = *hdrIntent;
        else if (const auto output = hdrOutputState(window_)) hdrRequested_ = output->snapshot().calibrated;
        else hdrRequested_ |= format == "scrgb";
        handoff_.request(adapter); // Replaces only the intent; old lease stays charged.
        if (busy_.exchange(true)) return true;
        blocked_ = true;
        generation_.advance();
        originalVisibility_ = window_->visibility();
        restoring_ = true;
        polls_ = 0;
        window_->setProperty("licasaHdrRecoveryState", "quiescing");
        window_->setPersistentSceneGraph(false);
        window_->setPersistentGraphics(false);
        window_->hide();
        window_->releaseResources();
        QTimer::singleShot(0, this, [this] { finish(); });
        return true;
    }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Close && (busy_.load() || restoring_)) {
            busy_ = false; blocked_ = true; restoring_ = false; generation_.advance();
            window_->setProperty("licasaHdrRecoveryState", "cancelled");
        }
        if (event->type() == QEvent::PlatformSurface) ++epoch_;
        if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::Resize) { ++epoch_; clamp(); }
        return false;
    }
private:
    void finish() {
        if (!busy_.load()) return;
        if (window_->isSceneGraphInitialized()) {
            if (++polls_ < 200) { QTimer::singleShot(10, this, [this] { finish(); }); return; }
            // No teardown proof: keep every debit and the existing device.
            busy_ = false; blocked_ = false;
            window_->setProperty("licasaHdrRecoveryState", "timeout");
            window_->setVisibility(originalVisibility_);
            restoring_ = false;
            return;
        }
        // Both pinned Qt render loops synchronize native/swapchain cleanup here.
        // No device change is made while the old scene graph can submit work.
        window_->destroy();
        if (window_->handle() || window_->isSceneGraphInitialized()) {
            busy_ = false; restoring_ = false;
            window_->setProperty("licasaHdrRecoveryState", "teardown-failed");
            return; // Keep lease; do not resurrect an unproven device.
        }
        const auto token = handoff_.token();
        if (!handoff_.detached(token)) return;
        // Re-query after teardown: device disappearance/limit changes cannot be
        // hidden by the selection snapshot captured when the request began.
        const auto fresh = findHdrAdapter(window_, wanted_->adapter.key);
        bool admitted = false;
        if (fresh) {
            pool_->setAdapterLimit(fresh->adapter.key, fresh->adapter.limit);
            admitted = (!hdrIntent_ || hdrRequested_) && handoff_.admit(token);
            applyHdrAdapter(window_, *fresh);
            currentAdapter_ = fresh->adapter.key;
        }
        if (!admitted) window_->setProperty("licasaHdrAdapter", QByteArray());
        window_->setProperty("_qt_sg_hdr_format", admitted && hdrRequested_ ? QByteArray("scrgb") : QByteArray());
        ++epoch_;
        if (!fresh) {
            busy_ = false; blocked_ = true; restoring_ = false;
            window_->setProperty("licasaHdrRecoveryState", "unavailable");
            qWarning() << "[Licasa] HDR recovery destination disappeared; graphics remain stopped";
            return;
        }
        // Old invalidation callbacks must run before the first new exposure.
        QTimer::singleShot(0, this, [this, token, admitted] {
            if (!busy_.load()) return;
            if (handoff_.token() != token) { finish(); return; }
            busy_ = false; blocked_ = false;
            window_->setProperty("licasaHdrRecoveryState", admitted ? "rebound" : "sdr");
            window_->setVisibility(originalVisibility_);
            restoring_ = false;
            window_->update();
        });
    }
    void clamp() {
        if (clamping_) return;
        clamping_ = true;
        const double dpr = window_->devicePixelRatio();
        if (std::isfinite(dpr) && dpr > 0) {
            const QSize limit = logical_.boundedTo(QSize(
                qMax(1, int(std::floor(qMin(double(logical_.width()), pixels_.width() / dpr)))),
                qMax(1, int(std::floor(qMin(double(logical_.height()), pixels_.height() / dpr))))));
            window_->setMaximumSize(limit);
            if (window_->width() > limit.width()) window_->setWidth(limit.width());
            if (window_->height() > limit.height()) window_->setHeight(limit.height());
        }
        clamping_ = false;
    }
    QQuickWindow* window_;
    QSize logical_, pixels_;
    bool clamping_ = false;
    quint64 epoch_ = 1, syncedEpoch_ = 0, lastFrameEpoch_ = 0;
    QSize lastFrame_;
    HdrRenderGeneration generation_;
    quint64 syncedGeneration_ = 0, lastFrameGeneration_ = 0;
    std::shared_ptr<GainmapLab::ResourceBudget> pool_;
    HdrTargetHandoff handoff_;
    QByteArray currentAdapter_;
    std::optional<HdrAdapterSelection> wanted_;
    std::atomic_bool blocked_{false}, busy_{false}, threaded_{false};
    QWindow::Visibility originalVisibility_ = QWindow::Hidden;
    bool hdrRequested_ = false, hdrIntent_ = false, restoring_ = false;
    int polls_ = 0, automaticRetries_ = 0;
};
}
static bool configureTargets(QQuickWindow* window, QSize logicalLimit, double maxDpr,
                       std::shared_ptr<GainmapLab::ResourceBudget> pool, bool admit)
{
    if (!window || !pool || QThread::currentThread() != window->thread()
        || window->isVisible() || window->handle()
        || window->property("licasaHdrTargetPixels").isValid()
        || window->requestedFormat().samples() > 1 || QSurfaceFormat::defaultFormat().samples() > 1
        || qEnvironmentVariableIsSet("QSG_SAMPLES") || window->format().stereo()) return false;
    logicalLimit = logicalLimit.boundedTo(window->maximumSize());
    // The guard clamps an oversized initial window before it gets a native
    // surface. A minimum size outside the envelope cannot be clamped.
    if (window->minimumWidth() > logicalLimit.width() || window->minimumHeight() > logicalLimit.height()) return false;
    const auto plan = hdrTargetPlan(logicalLimit, maxDpr);
    if (!plan) return false;
    const auto selection = selectHdrAdapter(window);
    if (!selection) return false;
    pool->setAdapterLimit(selection->adapter.key, selection->adapter.limit);
    GainmapLab::ResourceBudget::Lease lease;
    if (admit) {
        lease = pool->acquire({0, 0, 0, plan->bytes}, selection->adapter.key);
        if (!lease) return false; // No output/property/geometry mutations on denial.
        applyHdrAdapter(window, *selection);
    }
    new TargetWindowGuard(window, logicalLimit, plan->pixelLimit, pool, plan->bytes, selection->adapter.key, std::move(lease));
    window->setProperty("licasaHdrTargetPixels", plan->pixelLimit);
    window->setProperty("licasaHdrTargetBytes", QVariant::fromValue(plan->bytes));
    return true;
}
bool reserveHdrTargets(QQuickWindow* window, QSize logical, double dpr, std::shared_ptr<GainmapLab::ResourceBudget> pool)
{ return configureTargets(window, logical, dpr, std::move(pool), true); }
bool prepareHdrTargets(QQuickWindow* window, QSize logical, double dpr, std::shared_ptr<GainmapLab::ResourceBudget> pool)
{ return configureTargets(window, logical, dpr, std::move(pool), false); }
bool hdrTargetRecovering(const QQuickWindow* window)
{
    if (!window) return false;
    auto* guard = static_cast<TargetWindowGuard*>(window->property("licasaHdrTargetGuard").value<QObject*>());
    return guard && guard->recovering();
}
bool hdrTargetKeepsWindowAlive(const QQuickWindow* window)
{
    if (!window || QThread::currentThread() != window->thread()) return false;
    auto* guard = static_cast<TargetWindowGuard*>(window->property("licasaHdrTargetGuard").value<QObject*>());
    return guard && guard->keepsWindowAlive();
}
bool requestHdrAdapterMigration(QQuickWindow* window, const QByteArray& adapter)
{
    if (!window || QThread::currentThread() != window->thread()) return false;
    auto* guard = static_cast<TargetWindowGuard*>(window->property("licasaHdrTargetGuard").value<QObject*>());
    return guard && guard->request(adapter);
}
bool requestHdrDeviceRecovery(QQuickWindow* window)
{
    if (!window || QThread::currentThread() != window->thread()) return false;
    auto* guard = static_cast<TargetWindowGuard*>(window->property("licasaHdrTargetGuard").value<QObject*>());
    return guard && guard->recover();
}
bool requestHdrSurfaceChange(QQuickWindow* window, bool hdr)
{
    if (!window || QThread::currentThread() != window->thread()) return false;
    auto* guard = static_cast<TargetWindowGuard*>(window->property("licasaHdrTargetGuard").value<QObject*>());
    return guard && guard->surface(hdr);
}
bool hdrTargetReadbackReady(const QQuickWindow* window)
{
    if (!window || QThread::currentThread() != window->thread()) return false;
    auto* guard = static_cast<TargetWindowGuard*>(window->property("licasaHdrTargetGuard").value<QObject*>());
    return !guard || guard->readbackReady();
}
bool hdrTargetsCover(const QQuickWindow* window, QSize physicalSize)
{
    if (!window || physicalSize.isEmpty()) return false;
    const QSize limit = window->property("licasaHdrTargetPixels").toSize();
    return physicalSize.width() <= limit.width() && physicalSize.height() <= limit.height();
}
}
