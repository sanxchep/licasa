#include "diagnostics_common.h"
#include "imaging/async_image_provider.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QQuickWindow>
#include <QSizeF>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <cmath>
#include <utility>
#include <vector>

namespace LicasaDiagnostics {
namespace {
class NavigationTransitionCheck final : public QObject, public DiagnosticCheck {
    Q_OBJECT

  public:
    explicit NavigationTransitionCheck(QUrl nextImageUrl, bool duringFull)
        : nextImageUrl_(std::move(nextImageUrl)), duringFull_(duringFull)
    {}

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        viewport_ = window->findChild<QObject*>(QStringLiteral("imageViewport"));
        if (!viewport_) {
            return false;
        }
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &NavigationTransitionCheck::poll);
        connect(window_, &QQuickWindow::frameSwapped, this,
                &NavigationTransitionCheck::checkPresentedFrame, Qt::QueuedConnection);
        timer_.start();
        QTimer::singleShot(15000, this, [this] {
            if (!finished_) {
                finish(QStringLiteral("Navigation transition timed out"));
            }
        });
        return true;
    }

  private:
    double value(const char* name) const { return viewport_->property(name).toDouble(); }
    double rootValue(const char* name) const { return window_->property(name).toDouble(); }

    bool nextImageReady() const
    {
        return viewport_->property("imageStatus").toInt() == 1 &&
               window_->property("displayedFileName").toString() == nextImageUrl_.fileName();
    }

    QString oldGeometryError() const
    {
        struct Sample {
            QObject* object;
            const char* name;
            double expected;
        };
        const Sample samples[] = {{viewport_, "imageWidth", oldWidth_},
                                  {viewport_, "imageHeight", oldHeight_},
                                  {viewport_, "currentScale", oldScale_},
                                  {window_, "panX", oldPanX_},
                                  {window_, "panY", oldPanY_}};
        for (const auto& sample : samples) {
            const double actual = sample.object->property(sample.name).toDouble();
            if (std::abs(actual - sample.expected) > 0.01) {
                return QStringLiteral("Outgoing image geometry changed during navigation: %1 "
                                      "expected=%2 actual=%3")
                    .arg(QString::fromLatin1(sample.name))
                    .arg(sample.expected)
                    .arg(actual);
            }
        }
        return {};
    }

    void startNavigation()
    {
        // A browsed photo must retain even a user-adjusted view until the
        // replacement is ready; navigation then returns the new photo to fit.
        window_->setProperty("fitMode", false);
        window_->setProperty("currentScale", value("currentScale") * 1.25);
        window_->setProperty("panX", 18.0);
        window_->setProperty("panY", -12.0);
        oldWidth_ = value("imageWidth");
        oldHeight_ = value("imageHeight");
        oldScale_ = value("currentScale");
        oldPanX_ = rootValue("panX");
        oldPanY_ = rootValue("panY");
        navigating_ = true;
        navigationStartedNs_ = monotonicNs();
        const bool invoked = QMetaObject::invokeMethod(
            window_, "loadImageUrl", Qt::DirectConnection,
            Q_ARG(QVariant, QVariant::fromValue(nextImageUrl_)), Q_ARG(QVariant, QVariant(true)));
        if (!invoked) {
            finish(QStringLiteral("Could not start navigation"));
            return;
        }
        result_["navigation_call_ms"] = (monotonicNs() - navigationStartedNs_) / 1e6;
        const QSizeF target = window_->property("naturalImageSize").toSizeF();
        if (target.width() == oldWidth_ && target.height() == oldHeight_) {
            finish(QStringLiteral("Navigation fixture has the same dimensions"));
            return;
        }
        if (!nextImageReady()) {
            const QString error = oldGeometryError();
            if (!error.isEmpty()) {
                finish(error);
            }
        }
    }

    void poll()
    {
        if (finished_) {
            return;
        }
        if (!navigating_) {
            if (duringFull_) {
                const QString pending = viewport_->property("pendingRequestKey").toString();
                if (viewport_->property("imageStatus").toInt() == 1 &&
                    !window_->property("displayedFileName").toString().isEmpty() &&
                    (pending.contains(QStringLiteral("licasa_stage=full")) ||
                     pending.contains(QStringLiteral("licasa_stage=raw-interactive")))) {
                    startNavigation();
                }
                return;
            }
            if (viewport_->property("imageStatus").toInt() == 1 &&
                viewport_->property("pendingImageSlot").toInt() < 0) {
                if (firstReadyNs_ == 0) {
                    firstReadyNs_ = monotonicNs();
                } else if (monotonicNs() - firstReadyNs_ >= 150000000) {
                    startNavigation();
                }
            } else {
                firstReadyNs_ = 0;
            }
            return;
        }
        if (!nextImageReady()) {
            const QString error = oldGeometryError();
            if (!error.isEmpty()) {
                finish(error);
            }
            return;
        }
        if (!firstPixelsNs_) {
            firstPixelsNs_ = monotonicNs();
            result_["navigation_to_first_pixels_ms"] =
                (firstPixelsNs_ - navigationStartedNs_) / 1e6;
        }
        const QSizeF target = window_->property("naturalImageSize").toSizeF();
        if (!target.isValid() || target.isEmpty()) {
            return;
        }
        const QSizeF displayed = window_->property("displayedNaturalImageSize").toSizeF();
        if (displayed.isValid() && !displayed.isEmpty() &&
            (std::abs(value("imageWidth") - target.width()) > 0.01 ||
             std::abs(value("imageHeight") - target.height()) > 0.01)) {
            finish(QStringLiteral("New image did not adopt its own dimensions"));
            return;
        }
        if (!window_->property("fitMode").toBool() ||
            std::abs(window_->property("panX").toDouble()) > 0.01 ||
            std::abs(window_->property("panY").toDouble()) > 0.01) {
            finish(QStringLiteral("New image did not settle into fit mode"));
            return;
        }
        result_["outgoing_width"] = oldWidth_;
        result_["outgoing_height"] = oldHeight_;
        result_["incoming_width"] = value("imageWidth");
        result_["incoming_height"] = value("imageHeight");
        result_["outgoing_frames_checked"] = outgoingFrames_;
        result_["navigation_to_ready_ms"] = (monotonicNs() - navigationStartedNs_) / 1e6;
        finish({});
    }

    void checkPresentedFrame()
    {
        if (!navigating_ || finished_ || nextImageReady()) {
            return;
        }
        ++outgoingFrames_;
        const QString error = oldGeometryError();
        if (!error.isEmpty()) {
            finish(error);
        }
    }

    void finish(const QString& error)
    {
        if (finished_) {
            return;
        }
        finished_ = true;
        timer_.stop();
        if (!error.isEmpty()) {
            result_["error"] = error;
            result_["active_request_key"] = viewport_->property("activeRequestKey").toString();
            result_["pending_request_key"] = viewport_->property("pendingRequestKey").toString();
            result_["image_status"] = viewport_->property("imageStatus").toInt();
            result_["image_width"] = value("imageWidth");
            result_["preview_phase"] = window_->property("previewPhase").toBool();
            result_["rendered_source"] = window_->property("renderedProviderSource").toString();
            result_["displayed_file"] = window_->property("displayedFileName").toString();
        }
        printJson(result_);
        QCoreApplication::exit(error.isEmpty() ? 0 : 2);
    }

    QUrl nextImageUrl_;
    QQuickWindow* window_ = nullptr;
    QObject* viewport_ = nullptr;
    QTimer timer_;
    QJsonObject result_;
    bool navigating_ = false;
    bool duringFull_ = false;
    bool finished_ = false;
    int outgoingFrames_ = 0;
    qint64 firstReadyNs_ = 0;
    qint64 firstPixelsNs_ = 0;
    qint64 navigationStartedNs_ = 0;
    double oldWidth_ = 0;
    double oldHeight_ = 0;
    double oldScale_ = 0;
    double oldPanX_ = 0;
    double oldPanY_ = 0;
};

class NavigationSequenceCheck final : public QObject, public DiagnosticCheck {
  public:
    NavigationSequenceCheck(Licasa::AsyncImageProvider* provider, int presses, int intervalMs,
                            int warmupMs, int idleMs, int direction)
        : provider_(provider), presses_(presses), intervalMs_(intervalMs), warmupMs_(warmupMs),
          idleMs_(idleMs), direction_(direction)
    {}

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        viewport_ = window->findChild<QObject*>(QStringLiteral("imageViewport"));
        if (!viewport_) {
            return false;
        }
        window_->setProperty("fullScreenMode", true);
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
        connect(
            window_, &QQuickWindow::frameSwapped, this, [this] { presentedFrame(); },
            Qt::QueuedConnection);
        timer_.start();
        const int timeoutMs =
            std::min(120000, 15000 + warmupMs_ + presses_ * intervalMs_ + idleMs_);
        QTimer::singleShot(timeoutMs, this, [this] {
            if (!finished_) {
                finish(QStringLiteral("Navigation sequence timed out"));
            }
        });
        return true;
    }

  private:
    struct Press {
        QString fileName;
        int revision = -1;
        qint64 pressedNs = 0;
        qint64 readyNs = 0;
        qint64 presentedNs = 0;
        quint64 hitsBefore = 0;
        quint64 hitsAtReady = 0;
        qsizetype cacheCountBefore = 0;
        qsizetype cacheBytesBefore = 0;
        double selectionCallMs = 0;
        double timerLagMs = 0;
        bool fullPendingBefore = false;
    };

    bool activeMatches(const Press& press) const
    {
        return window_->property("displayedFileName").toString() == press.fileName &&
               viewport_->property("imageStatus").toInt() == 1 &&
               viewport_->property("activeRequestKey")
                   .toString()
                   .contains(QStringLiteral("/%1/").arg(press.revision));
    }

    void pressArrow()
    {
        const QUrl before = window_->property("residentImageUrl").toUrl();
        Press press;
        press.pressedNs = monotonicNs();
        press.timerLagMs = (press.pressedNs - nextPressNs_) / 1e6;
        press.hitsBefore = provider_->nearbyPreviewHitCount();
        press.cacheCountBefore = provider_->nearbyPreviewCount();
        press.cacheBytesBefore = provider_->nearbyPreviewBytes();
        const QString pending = viewport_->property("pendingRequestKey").toString();
        press.fullPendingBefore = pending.contains(QStringLiteral("licasa_stage=full")) ||
                                  pending.contains(QStringLiteral("licasa_stage=raw-interactive"));
        const int direction =
            std::abs(direction_) == 2 && steps_.size() % 2 != 0 ? -direction_ : direction_;
        const bool invoked =
            QMetaObject::invokeMethod(window_, "browseImage", Qt::DirectConnection,
                                      Q_ARG(QVariant, QVariant(direction > 0 ? 1 : -1)));
        press.selectionCallMs = (monotonicNs() - press.pressedNs) / 1e6;
        const QUrl after = window_->property("residentImageUrl").toUrl();
        if (!invoked || !after.isLocalFile() || after == before) {
            finish(QStringLiteral("Arrow press did not select another image"));
            return;
        }
        press.fileName = after.fileName();
        press.revision = window_->property("imageRevision").toInt();
        steps_.push_back(std::move(press));
    }

    void poll()
    {
        if (finished_) {
            return;
        }
        const qint64 now = monotonicNs();
        if (!initialReadyNs_) {
            if (viewport_->property("imageStatus").toInt() == 1 &&
                !window_->property("displayedFileName").toString().isEmpty()) {
                initialReadyNs_ = now;
                nextPressNs_ = now + qint64(warmupMs_) * 1000000;
            }
            return;
        }

        for (Press& press : steps_) {
            if (!press.readyNs && activeMatches(press)) {
                press.readyNs = now;
                press.hitsAtReady = provider_->nearbyPreviewHitCount();
            }
        }

        if (int(steps_.size()) < presses_ && now >= nextPressNs_) {
            pressArrow();
            nextPressNs_ += qint64(intervalMs_) * 1000000;
        }
        if (int(steps_.size()) == presses_ && !steps_.empty() && steps_.back().presentedNs) {
            if (!finalPresentedNs_) {
                finalPresentedNs_ = now;
            }
            if (now - finalPresentedNs_ >= qint64(idleMs_) * 1000000) {
                finish({});
            }
        }
    }

    void presentedFrame()
    {
        if (finished_) {
            return;
        }
        const qint64 now = monotonicNs();
        for (Press& press : steps_) {
            if (press.readyNs && !press.presentedNs && activeMatches(press)) {
                press.presentedNs = now;
            }
        }
    }

    void finish(const QString& error)
    {
        if (finished_) {
            return;
        }
        finished_ = true;
        timer_.stop();
        QJsonArray presses;
        for (const Press& press : steps_) {
            QJsonObject item{{"file", press.fileName},
                             {"revision", press.revision},
                             {"selection_call_ms", press.selectionCallMs},
                             {"timer_lag_ms", press.timerLagMs},
                             {"cache_count_before", int(press.cacheCountBefore)},
                             {"cache_bytes_before", double(press.cacheBytesBefore)},
                             {"full_pending_before", press.fullPendingBefore}};
            if (press.readyNs) {
                item["to_ready_ms"] = (press.readyNs - press.pressedNs) / 1e6;
                item["cache_hits_by_ready"] = double(press.hitsAtReady - press.hitsBefore);
            }
            if (press.presentedNs) {
                item["to_presented_ms"] = (press.presentedNs - press.pressedNs) / 1e6;
            }
            presses.append(item);
        }
        QJsonObject result{
            {"press_count", presses_},
            {"interval_ms", intervalMs_},
            {"warmup_ms", warmupMs_},
            {"idle_ms", idleMs_},
            {"direction", direction_},
            {"steps", presses},
            {"cache_count_final", int(provider_->nearbyPreviewCount())},
            {"cache_bytes_final", double(provider_->nearbyPreviewBytes())},
            {"cache_hits_final", double(provider_->nearbyPreviewHitCount())},
            {"preview_phase_final", window_->property("previewPhase").toBool()},
            {"browse_burst_active_final", window_->property("browseBurstActive").toBool()},
            {"active_request_key_final", viewport_->property("activeRequestKey").toString()},
            {"pending_request_key_final", viewport_->property("pendingRequestKey").toString()},
            {"process", processMetrics()}};
        if (!error.isEmpty()) {
            result["error"] = error;
            result["resident_file"] = window_->property("residentImageUrl").toUrl().fileName();
            result["displayed_file"] = window_->property("displayedFileName").toString();
            result["pending_request_key"] = viewport_->property("pendingRequestKey").toString();
        }
        printJson(result);
        QCoreApplication::exit(error.isEmpty() ? 0 : 2);
    }

    Licasa::AsyncImageProvider* provider_ = nullptr;
    QQuickWindow* window_ = nullptr;
    QObject* viewport_ = nullptr;
    QTimer timer_;
    std::vector<Press> steps_;
    int presses_ = 0;
    int intervalMs_ = 0;
    int warmupMs_ = 0;
    int idleMs_ = 0;
    int direction_ = 1;
    bool finished_ = false;
    qint64 initialReadyNs_ = 0;
    qint64 nextPressNs_ = 0;
    qint64 finalPresentedNs_ = 0;
};
} // namespace

std::unique_ptr<DiagnosticCheck> makeNavigationTransitionCheck(const QUrl& nextImageUrl,
                                                               bool duringFull)
{
    return std::make_unique<NavigationTransitionCheck>(nextImageUrl, duringFull);
}

std::unique_ptr<DiagnosticCheck> makeNavigationSequenceCheck(Licasa::AsyncImageProvider* provider,
                                                             int presses, int intervalMs,
                                                             int warmupMs, int idleMs,
                                                             int direction)
{
    return std::make_unique<NavigationSequenceCheck>(provider, presses, intervalMs, warmupMs,
                                                     idleMs, direction);
}
} // namespace LicasaDiagnostics

#include "diagnostics_navigation_transition.moc"
