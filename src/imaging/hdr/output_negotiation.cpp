#include "output_negotiation.h"
#include "monitor_calibration.h"
#include "wayland_hdr.h"
#include <atomic>
#include "render_target_budget.h"
#include "../../app/application_settings.h"
#include "../../../tools/hdr-probe/surface_format.h"
#include <QColorSpace>
#include <QGuiApplication>
#include <QScreen>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <QEvent>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QDebug>
#include <rhi/qrhi.h>
#include <utility>
#include <vector>

namespace Licasa {
namespace {
class OutputTracker final : public QObject {
    Q_DECLARE_TR_FUNCTIONS(HdrOutput)
public:
    OutputTracker(QQuickWindow* window, ViewerPreferences* preferences
#ifdef LICASA_HDR_RENDER_TEST
                  , wl_display* testDisplay=nullptr
#endif
                  )
        : QObject(window), window_(window), state_(std::make_shared<HdrOutputState>()),
          preferences_(preferences ? preferences : new ViewerPreferences(this))
    {
        window_->setProperty("licasaHdrOutputTracker", QVariant::fromValue<QObject*>(this));
        watchScreen(window_->screen());
        window_->installEventFilter(this);
        qGuiApp->installEventFilter(this);
        const auto changed=[this] { waylandChanged(); };
#ifdef LICASA_HDR_RENDER_TEST
        if (testDisplay) wayland_=std::make_unique<WaylandHdrObserver>(testDisplay,changed);
        else
#endif
            wayland_=observeWaylandHdr(window_,changed);
        waylandSettle_.setSingleShot(true);
        waylandSettle_.setInterval(1000);
        waylandSettle_.setTimerType(Qt::PreciseTimer);
        connect(&waylandSettle_, &QTimer::timeout, this, [this] {
            if (closed_ || !state_->snapshot().calibrated) {
                awaitingWaylandReplacement_=false;
                waylandWait_.invalidate();
                return;
            }
            if (wayland_->report().state==WaylandHdrReport::State::Ready) {
                waylandChanged();
            } else {
                state_->requireNativeReview();
                invalidate();
            }
        });
        connect(window_, &QWindow::screenChanged, this, [this](QScreen* screen) {
            if (screen_ != screen) state_->resetNativeReview();
            watchScreen(screen); invalidate(true);
        });
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { invalidate(true); });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
            if (screen_ == screen) watchScreen(nullptr);
            invalidate(true); // A formerly ambiguous identity may become unique.
        });
        connect(preferences_, &ViewerPreferences::hdrMonitorCalibrationChanged, this, [this](const QString& key) {
            if (!key.isEmpty() && key == monitorKey()) {
                state_->resetNativeReview(); // An explicit saved choice confirms the new calibration.
                invalidate(true);
            }
        });
        connect(preferences_, &ViewerPreferences::maximumImageMemoryMiBChanged, this, [this] {
            if (budgetDenied_ && !closed_) { budgetDenied_ = false; tryDesired(); }
        });
        // QScreen has no portable HDR-mode signal. Observe the actual native
        // format and metadata each sync; throttle physical format queries.
        connect(window_, &QQuickWindow::beforeSynchronizing, this, [this] {
            if (hdrTargetRecovering(window_) || !window_->rhi() || window_->rhi()->isDeviceLost()
                || !window_->swapChain()) return;
            const auto format = nativeSwapchainFormat(window_->swapChain());
            if (!format) return;
            const auto surface = classifySurfaceFormat(*format);
            const auto calibration = state_->snapshot();
            if (surface == SurfaceFormat::PassThrough && awaitsSurfaceTag() && calibration.calibrated) {
                const auto report = wayland_->report();
                const auto request = std::make_pair(report.revision, calibration.revision);
                if (report.state == WaylandHdrReport::State::Ready
                    && waylandAcceptedRevision_.load() == report.revision && tagRequest_ != request) {
                    tagRequest_ = request;
                    QMetaObject::invokeMethod(this, [this, epoch = report.revision, revision = calibration.revision] {
                        if (!current(revision) || !waylandAllowed() || wayland_->report().revision != epoch) return;
                        if (window_->requestedFormat().colorSpace().isValid() || !wayland_->tagLinearSurface()) {
                            state_->requireNativeReview();
                            apply(state_->snapshot().revision);
                        }
                        window_->update();
                    }, Qt::QueuedConnection);
                }
                return;
            }
            const auto info = window_->swapChain()->hdrInfo();
            HdrOutputState::Native native;
            native.linear = surface == SurfaceFormat::LinearFp16
                || (surface == SurfaceFormat::PassThrough && surfaceTagged());
            const auto revision = calibration.revision;
            if (!supportTimer_.isValid() || supportTimer_.elapsed() >= 1000 || probedRevision_ != revision) {
                supported_ = wayland_ ? supportsTaggedWaylandHdrSwapchain(window_->swapChain(), appTagEnabled_)
                                     : window_->swapChain()->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear);
                supportTimer_.start(); probedRevision_ = revision;
            }
            native.supported = supported_;
            native.sceneReferred = info.luminanceBehavior == QRhiSwapChainHdrInfo::SceneReferred;
            native.limitsType = info.limitsType;
            auto finite = [](float n) { return std::isfinite(n) && n >= 0 ? n : 0.0f; };
            native.white = finite(info.sdrWhiteLevel);
            if (info.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits) {
                native.minimum = finite(info.limits.luminanceInNits.minLuminance);
                native.maximum = finite(info.limits.luminanceInNits.maxLuminance);
            } else {
                native.maximum = finite(info.limits.colorComponentValue.maxColorComponentValue);
                native.potential = finite(info.limits.colorComponentValue.maxPotentialColorComponentValue);
            }
            const bool changed = state_->observe(native);
            const auto token = state_->snapshot().revision;
            const bool reported = !observedNative_ || !(*observedNative_ == native) || reportedRevision_ != token;
            observedNative_ = native; reportedRevision_ = token;
            if (changed || reported) {
                QMetaObject::invokeMethod(this, [this, token, native, changed] {
                    if (closed_ || state_->snapshot().revision != token) return;
                    const bool firstNative = !nativeKnown_;
                    nativeKnown_ = true; nativeLinear_ = native.linear; nativeSupported_ = native.supported;
                    if (changed) apply(token);
                    else if (wayland_ && firstNative && native.supported && !state_->snapshot().calibrated) tryDesired();
                    publish();
                }, Qt::QueuedConnection);
            }
        }, Qt::DirectConnection);
        connect(window_, &QQuickWindow::sceneGraphInitialized, this, [this] {
            supportTimer_.invalidate(); probedRevision_ = 0; tagRequest_ = {}; observedNative_.reset();
        }, Qt::DirectConnection);
        timer_.setInterval(1000);
        connect(&timer_, &QTimer::timeout, this, [this] { if (window_->isVisible() && !closed_) window_->update(); });
        connect(window_, &QWindow::visibleChanged, this, [this](bool visible) {
            if (visible) { closed_ = false; timer_.start(); publish(); }
            else timer_.stop();
        });
    }
    bool start() {
        external_ = !qEnvironmentVariableIsEmpty("QSG_RHI_HDR")
            || !window_->property("_qt_sg_hdr_format").toByteArray().isEmpty();
        appTagEnabled_ = wayland_ && !external_ && waylandTaggedPassthroughEnabled()
            && !window_->requestedFormat().colorSpace().isValid();
        loadProfile();
        // Environment values are session-only and never overwrite a saved choice.
        if (profile().isEmpty()) {
            bool whiteOk=false, peakOk=false;
            const auto white=qEnvironmentVariable("LICASA_HDR_PAPER_WHITE_NITS").toFloat(&whiteOk);
            const auto peak=qEnvironmentVariable("LICASA_HDR_PEAK_NITS").toFloat(&peakOk);
            if (whiteOk && peakOk && calibratedHdrOutput(white,peak,true).headroom>1) { white_=white; peak_=peak; }
        }
        QSize logical; double dpr=0;
        for (auto* screen : QGuiApplication::screens()) {
            logical=logical.expandedTo(screen->geometry().size()); dpr=qMax(dpr,screen->devicePixelRatio());
        }
        bool requested=false;
        if (!external_) {
            if (wantsHdr() && !wayland_ && reserveHdrTargets(window_,logical,dpr)) {
                state_->calibrate(state_->snapshot().revision,white_,peak_);
                window_->setProperty("_qt_sg_hdr_format",QByteArrayLiteral("scrgb"));
                qInfo() << "[Licasa] HDR target envelope pixels/reserved bytes:"
                        << window_->property("licasaHdrTargetPixels").toSize() << window_->property("licasaHdrTargetBytes").toULongLong();
                qInfo() << "[Licasa] HDR adapter UUID/GPU cap/user total bytes:"
                        << window_->property("licasaHdrAdapter").toByteArray().toHex()
                        << window_->property("licasaHdrAdapterLimit").toULongLong() << viewerResourceBudget()->totalLimit();
                qInfo() << "[Licasa] HDR lab scRGB output requested; paper white/peak nits:" << white_ << peak_;
                requested=true;
            } else {
                budgetDenied_=wantsHdr() && !wayland_;
                if (budgetDenied_) qInfo() << "[Licasa] HDR target budget unavailable; retaining SDR output";
                targetsAvailable_=prepareHdrTargets(window_,logical,dpr);
                if (!targetsAvailable_) { budgetDenied_=false; external_=true; }
            }
        }
        window_->setProperty("licasaHdrPaperWhiteNits",state_->snapshot().white);
        window_->setProperty("licasaHdrPeakNits",state_->snapshot().peak);
        publish(); return requested;
    }
    std::shared_ptr<HdrOutputState> state() const { return state_; }
#ifdef LICASA_HDR_RENDER_TEST
    WaylandHdrObserver* testObserver() const { return wayland_.get(); }
    bool testCalibrate(float white,float peak) {
        nativeKnown_=nativeSupported_=true;
        return calibrate(state_->snapshot().revision,white,peak);
    }
#endif
    bool waylandAllowed() const {
        if (!wayland_) return true;
        const auto report=wayland_->report();
        return report.state==WaylandHdrReport::State::Ready && waylandAcceptedRevision_.load()==report.revision;
    }
    bool surfaceTagged() const { return wayland_ && wayland_->linearSurfaceTagged(); }
    bool awaitsSurfaceTag() const { return appTagEnabled_ && !surfaceTagged(); }
    QString monitorKey() const {
        if (!screen_ || screen_ != window_->screen() || !QGuiApplication::screens().contains(screen_)) return {};
        const auto key=hdrMonitorKey(screen_->manufacturer(),screen_->model(),screen_->serialNumber());
        if (key.isEmpty()) return {};
        int matches=0;
        for (auto* screen : QGuiApplication::screens())
            if (hdrMonitorKey(screen->manufacturer(),screen->model(),screen->serialNumber())==key) ++matches;
        return matches==1 ? key : QString();
    }
    QVariantMap settings() const {
        const auto snapshot=state_->snapshot();
        const auto saved=profile();
        QString code, message;
        const auto recovery=window_->property("licasaHdrRecoveryState").toByteArray();
        if (!screen_) { code="no-monitor"; message=tr("SDR — no monitor is connected."); }
        else if (disabled_) { code="disabled"; message=monitorKey().isEmpty()
            ? tr("SDR — HDR is turned off for this session.") : tr("SDR — HDR is turned off for this monitor."); }
        else if (external_) { code="output-unavailable"; message=tr("SDR — HDR is unavailable with the current display settings."); }
        else if (recovery=="unavailable" || recovery=="teardown-failed" || recovery=="timeout") {
            code="recovery-failed"; message=tr("SDR — display recovery could not finish. Reopen the viewer.");
        } else if (hdrTargetRecovering(window_)) { code="changing"; message=tr("Updating display output…"); }
        else if (budgetDenied_) { code="memory"; message=tr("SDR — HDR exceeds the current memory budget."); }
        else if (wayland_ && !waylandAllowed()) {
            const auto report=wayland_->report();
            const bool checking=report.state==WaylandHdrReport::State::Pending || waylandSettle_.isActive();
            code=checking ? "wayland-checking" : "wayland-sdr";
            message=checking
                ? tr("SDR — checking desktop HDR support.")
                : tr("SDR — HDR color management is unavailable for this output.");
        }
        else if (nativeKnown_ && !nativeSupported_) { code="unsupported"; message=tr("SDR — HDR output is unavailable on this display or desktop."); }
        else if (reopen_) { code="reopen"; message=tr("SDR — reopen this viewer to apply HDR settings."); }
        else if (!snapshot.calibrated) { code="calibration"; message=tr("SDR — review this monitor’s HDR calibration."); }
        else if (nativeKnown_ && nativeLinear_ && !window_->property("licasaHdrAdapter").toByteArray().isEmpty()) {
            code="hdr"; message=tr("HDR display output is active.");
        } else { code="checking"; message=tr("SDR — checking HDR display support."); }
        const auto key=monitorKey();
        QString name=screen_ ? screen_->model().trimmed() : QString();
        if (name.isEmpty() && screen_) name=screen_->name();
        return {{"revision",QString::number(snapshot.revision)},{"monitorKey",key},{"monitorName",name},
                {"canRemember",!key.isEmpty()},{"hasProfile",!saved.isEmpty()},{"enabled",!disabled_},
                {"paperWhite",white_>0 ? white_ : 200.0f},{"peak",peak_>0 ? peak_ : 800.0f},
                {"code",code},{"message",message},{"error",error_},
                {"identityMessage",key.isEmpty() ? tr("This monitor has no unique identity. Calibration can be used for this session.")
                                                 : tr("Saved settings apply only to this monitor.")}};
    }
    quint64 invalidate(bool restoreSaved=false) {
        awaitingWaylandReplacement_=false;
        waylandSettle_.stop();
        waylandWait_.invalidate();
        const auto revision=state_->invalidate();
        nativeKnown_=false; reopen_=false; error_.clear();
        window_->setProperty("licasaHdrOutputRevision",QVariant::fromValue(revision));
        QTimer::singleShot(0,this,[this,revision,restoreSaved] {
            if (closed_ || state_->snapshot().revision!=revision) return;
            if (restoreSaved) { loadProfile(); budgetDenied_=false; tryDesired(); }
            else apply(revision);
        });
        publish(); window_->update(); return revision;
    }
    bool calibrate(quint64 revision,float white,float peak) {
        if (!current(revision) || !waylandAllowed() || (wayland_ && (!nativeKnown_ || !nativeSupported_))
            || !state_->calibrate(revision,white,peak)) return false;
        white_=white; peak_=peak; disabled_=false; budgetDenied_=false;
        apply(state_->snapshot().revision); return true;
    }
    bool configure(quint64 revision,float white,float peak,bool remember) {
        if (!current(revision) || calibratedHdrOutput(white,peak,true).headroom<=1) return false;
        const auto key=monitorKey();
        if (remember) {
            if (key.isEmpty() || !preferences_ || !preferences_->setHdrMonitorCalibration(key,true,white,peak)) {
                error_=tr("Calibration could not be saved. Your previous settings are unchanged."); publish(); return false;
            }
        } else {
            white_=white; peak_=peak; disabled_=false; budgetDenied_=false; error_.clear();
            state_->resetNativeReview(); state_->invalidate(); tryDesired();
        }
        return true;
    }
    bool sdr(quint64 revision) {
        if (!current(revision)) return false;
        const auto key=monitorKey();
        if (!key.isEmpty() && preferences_) {
            if (!preferences_->setHdrMonitorCalibration(key,false,white_>0?white_:200,peak_>0?peak_:800)) {
                error_=tr("Display settings could not be saved."); publish(); return false;
            }
        } else { disabled_=true; state_->invalidate(); apply(state_->snapshot().revision); }
        return true;
    }
    bool forget(quint64 revision) {
        const auto key=monitorKey();
        if (!current(revision) || key.isEmpty() || !preferences_) return false;
        if (!preferences_->removeHdrMonitorCalibration(key)) { error_=tr("Saved calibration could not be removed."); publish(); return false; }
        return true;
    }
protected:
    bool eventFilter(QObject* watched,QEvent* event) override {
        if (event->type()==QEvent::LanguageChange) { publish(); return false; }
        if (watched!=window_) return false;
        if (event->type()==QEvent::DevicePixelRatioChange) invalidate(true);
        if (event->type()==QEvent::Close) { closed_=true; state_->invalidate(); timer_.stop(); }
        if (event->type()==QEvent::DynamicPropertyChange
            && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="licasaHdrRecoveryState") {
            const auto recovery=window_->property("licasaHdrRecoveryState").toByteArray();
            if (recovery=="sdr" && wantsHdr() && (pendingHdr_ || state_->snapshot().calibrated)) budgetDenied_=true;
            if (recovery=="sdr" || recovery=="rebound" || recovery=="cancelled") pendingHdr_=false;
            publish();
        }
        return false;
    }
private:
    bool current(quint64 revision) const {
        return !closed_ && screen_ && screen_==window_->screen() && QGuiApplication::screens().contains(screen_)
            && revision==state_->snapshot().revision;
    }
    bool wantsHdr() const { return !disabled_ && calibratedHdrOutput(white_,peak_,true).headroom>1; }
    QVariantMap profile() const { return preferences_ ? preferences_->hdrMonitorCalibration(monitorKey()) : QVariantMap{}; }
    void loadProfile() {
        const auto saved=profile();
        white_=saved.value("paperWhite").toFloat(); peak_=saved.value("peak").toFloat();
        disabled_=!saved.isEmpty() && !saved.value("enabled").toBool();
    }
    void tryDesired() {
        if (closed_) return;
        if (wantsHdr() && !external_ && waylandAllowed() && !state_->snapshot().nativeReview
            && (nativeKnown_ ? nativeSupported_ : !wayland_))
            state_->calibrate(state_->snapshot().revision,white_,peak_);
        apply(state_->snapshot().revision);
    }
    void waylandChanged() {
        const auto report=wayland_->report();
        if (closed_) { publish(); return; }
        const bool sameSurface=waylandSurfaceEpoch_==report.surfaceEpoch;
        const bool sameScreen=waylandScreen_==window_->screen();
        bool replacement = awaitingWaylandReplacement_ && !sameSurface && sameScreen;
        if (replacement && waylandWaitEpoch_ != report.surfaceEpoch) {
            // Each replacement owns its wait. A retired surface's timeout must
            // not revoke calibration while the new surface is still settling.
            waylandSettle_.stop();
            waylandWait_.invalidate();
            waylandWaitEpoch_ = report.surfaceEpoch;
        }
        if (replacement && waylandWait_.isValid() &&
            waylandWait_.elapsed() >= waylandSettle_.interval()) {
            // Socket feedback can be dispatched before an overdue Qt timer.
            // Accept the capability report, but require fresh calibration once
            // the current surface's wall-clock grace period has elapsed.
            state_->requireNativeReview();
            invalidate();
            replacement = false;
        }
        if (replacement
            && (report.state==WaylandHdrReport::State::Pending || report.state==WaylandHdrReport::State::Sdr)) {
            // A newly created toplevel may report unmapped SDR before its
            // first buffer places it on the same HDR output.
            waylandAcceptedRevision_=0;
            if (report.state == WaylandHdrReport::State::Sdr && !waylandWait_.isValid()) {
                waylandWait_.start();
                waylandSettle_.start();
            }
            publish(); window_->update();
            return;
        }
        if (!sameSurface) {
            awaitingWaylandReplacement_=false;
            waylandSettle_.stop();
            waylandWait_.invalidate();
        }
        if (report.state==WaylandHdrReport::State::Ready) {
            // The replacement can get a new identity for the same description.
            const bool descriptionChanged=sameScreen && waylandFingerprint_!=report.preferredFingerprint;
            const bool identityChanged=sameScreen && waylandIdentity_!=report.preferredIdentity;
            const bool review=waylandIdentity_
                && (descriptionChanged || (!replacement && (report.preferredChanged || identityChanged)));
            if (review) state_->requireNativeReview();
            waylandIdentity_=report.preferredIdentity; waylandFingerprint_=report.preferredFingerprint; waylandScreen_=window_->screen();
            waylandSurfaceEpoch_=report.surfaceEpoch;
            waylandAcceptedRevision_=report.revision;
            if (review || (!state_->snapshot().calibrated && !state_->snapshot().nativeReview)) {
                invalidate(); // Clear stale native-SDR observations before a new request.
                if (!review) tryDesired();
            }
        } else {
            waylandAcceptedRevision_=0;
            if (report.preferredChanged || report.state!=WaylandHdrReport::State::Pending) {
                if (waylandIdentity_ || state_->snapshot().calibrated) state_->requireNativeReview();
                invalidate();
            }
            // Own native teardown makes feedback temporarily pending. Keep the
            // requested format; drawing is masked until the new report is accepted.
        }
        publish(); window_->update();
    }
    void watchScreen(QScreen* screen) {
        for (const auto& c:screenConnections_) disconnect(c);
        screenConnections_.clear(); screen_=screen;
        if (!screen) return;
        auto changed=[this] { invalidate(true); };
        screenConnections_.push_back(connect(screen,&QScreen::geometryChanged,this,changed));
        screenConnections_.push_back(connect(screen,&QScreen::logicalDotsPerInchChanged,this,changed));
        screenConnections_.push_back(connect(screen,&QScreen::physicalDotsPerInchChanged,this,changed));
        screenConnections_.push_back(connect(screen,&QScreen::physicalSizeChanged,this,changed));
        screenConnections_.push_back(connect(screen,&QScreen::refreshRateChanged,this,changed));
        screenConnections_.push_back(connect(screen,&QScreen::orientationChanged,this,changed));
        screenConnections_.push_back(connect(screen,&QObject::destroyed,this,[this] { screen_=nullptr; invalidate(true); }));
    }
    void apply(quint64 revision) {
        const auto snapshot=state_->snapshot();
        if (closed_ || snapshot.revision!=revision) return;
        window_->setProperty("licasaHdrPaperWhiteNits",snapshot.white);
        window_->setProperty("licasaHdrPeakNits",snapshot.peak);
        window_->setProperty("licasaHdrOutputRevision",QVariant::fromValue(revision));
        window_->setProperty("licasaHdrOutputState",snapshot.calibrated?"calibrated":"sdr-needs-calibration");
        const bool requested=window_->property("_qt_sg_hdr_format").toByteArray()=="scrgb";
        if (!snapshot.calibrated) {
            awaitingWaylandReplacement_=false;
            waylandSettle_.stop();
            waylandWait_.invalidate();
        }
        if (!external_ && (requested!=snapshot.calibrated || hdrTargetRecovering(window_))) {
            state_->expectSurface();
            const bool expectReplacement=!requested && snapshot.calibrated && wayland_ && waylandAllowed();
            const bool accepted=requestHdrSurfaceChange(window_,snapshot.calibrated);
            if (accepted) {
                awaitingWaylandReplacement_ |= expectReplacement;
                pendingHdr_=snapshot.calibrated; nativeKnown_=false;
            }
            else if (snapshot.calibrated) reopen_=true;
        }
        publish(); window_->update();
    }
    void publish() {
        const auto value=settings();
        if (value==published_) return;
        published_=value;
        window_->setProperty("licasaHdrSettingsRevision",QVariant::fromValue(++publication_));
    }
    QQuickWindow* window_;
    std::shared_ptr<HdrOutputState> state_;
    QPointer<ViewerPreferences> preferences_;
    std::unique_ptr<WaylandHdrObserver> wayland_;
    std::atomic<quint64> waylandAcceptedRevision_{0};
    quint64 waylandSurfaceEpoch_=0;
    quint32 waylandIdentity_=0;
    QByteArray waylandFingerprint_;
    QPointer<QScreen> waylandScreen_;
    QPointer<QScreen> screen_;
    std::vector<QMetaObject::Connection> screenConnections_;
    QTimer timer_;
    QTimer waylandSettle_;
    QElapsedTimer waylandWait_;
    quint64 waylandWaitEpoch_ = 0;
    // Render-owner-only observation cache.
    QElapsedTimer supportTimer_;
    quint64 probedRevision_=0,reportedRevision_=0;
    std::pair<quint64,quint64> tagRequest_;
    bool supported_=false;
    std::optional<HdrOutputState::Native> observedNative_;
    // GUI-owner-only preferences/status.
    float white_=0,peak_=0;
    bool closed_=false,disabled_=false,external_=false,budgetDenied_=false,targetsAvailable_=true;
    bool appTagEnabled_=false;
    bool awaitingWaylandReplacement_=false;
    bool nativeKnown_=false,nativeLinear_=false,nativeSupported_=false,reopen_=false,pendingHdr_=false;
    QString error_;
    QVariantMap published_;
    quint64 publication_=0;
};
OutputTracker* tracker(const QQuickWindow* w) {
    return w?static_cast<OutputTracker*>(w->property("licasaHdrOutputTracker").value<QObject*>()):nullptr;
}
bool guiWindow(const QQuickWindow* w) { return w && QThread::currentThread()==w->thread(); }
}
std::shared_ptr<HdrOutputState> hdrOutputState(const QQuickWindow* w) {
    auto* output=tracker(w); return output?output->state():nullptr;
}
quint64 hdrOutputRevision(const QQuickWindow* w) {
    const auto output=hdrOutputState(w); return output?output->snapshot().revision:0;
}
bool hdrWaylandPermitsHdr(const QQuickWindow* w) {
    auto* output=tracker(w); return !output || output->waylandAllowed();
}
bool hdrWaylandSurfaceTagged(const QQuickWindow* w) {
    const auto* output=tracker(w); return output && output->surfaceTagged();
}
bool hdrWaylandAwaitsSurfaceTag(const QQuickWindow* w) {
    const auto* output=tracker(w);
    return output && output->awaitsSurfaceTag() && output->state()->snapshot().calibrated;
}
quint64 notifyHdrOutputChanged(QQuickWindow* w) { auto* t=guiWindow(w)?tracker(w):nullptr; return t?t->invalidate():0; }
bool recalibrateHdrOutput(QQuickWindow* w,quint64 revision,float white,float peak) {
    auto* t=guiWindow(w)?tracker(w):nullptr; return t && t->calibrate(revision,white,peak);
}
QVariantMap hdrMonitorSettings(const QQuickWindow* w) { auto* t=guiWindow(w)?tracker(w):nullptr; return t?t->settings():QVariantMap{}; }
bool setHdrMonitorSettings(QQuickWindow* w,quint64 revision,float white,float peak,bool remember) {
    auto* t=guiWindow(w)?tracker(w):nullptr; return t && t->configure(revision,white,peak,remember);
}
bool useSdrForMonitor(QQuickWindow* w,quint64 revision) { auto* t=guiWindow(w)?tracker(w):nullptr; return t && t->sdr(revision); }
bool forgetHdrMonitorSettings(QQuickWindow* w,quint64 revision) { auto* t=guiWindow(w)?tracker(w):nullptr; return t && t->forget(revision); }
bool requestHdrOutput(QQuickWindow* w,ViewerPreferences* preferences) {
    if (!guiWindow(w) || !w->screen() || w->isVisible() || w->handle() || tracker(w)
        || qstrcmp(qVersion(),QT_VERSION_STR) || QQuickWindow::graphicsApi()!=QSGRendererInterface::Vulkan) return false;
    return (new OutputTracker(w,preferences))->start();
}
#ifdef LICASA_HDR_RENDER_TEST
WaylandHdrObserver* attachWaylandOutputForTest(QQuickWindow* window, wl_display* display, wl_surface* surface) {
    auto* output=new OutputTracker(window,nullptr,display);
    output->start();
    auto* observer=output->testObserver();
    observer->setSurface(surface);
    return observer;
}
bool calibrateWaylandOutputForTest(QQuickWindow* window,float white,float peak) {
    return tracker(window)->testCalibrate(white,peak);
}
#endif
}
