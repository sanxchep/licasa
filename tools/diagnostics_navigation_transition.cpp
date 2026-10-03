#include "diagnostics_common.h"

#include <QCoreApplication>
#include <QQuickWindow>
#include <QSizeF>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <cmath>
#include <utility>

namespace LicasaDiagnostics {
namespace {
class NavigationTransitionCheck final : public QObject, public DiagnosticCheck {
    Q_OBJECT

  public:
    explicit NavigationTransitionCheck(QUrl nextImageUrl) : nextImageUrl_(std::move(nextImageUrl))
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
        const bool invoked = QMetaObject::invokeMethod(
            window_, "loadImageUrl", Qt::DirectConnection,
            Q_ARG(QVariant, QVariant::fromValue(nextImageUrl_)), Q_ARG(QVariant, QVariant(true)));
        if (!invoked) {
            finish(QStringLiteral("Could not start navigation"));
            return;
        }
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
        const QSizeF target = window_->property("naturalImageSize").toSizeF();
        if (std::abs(value("imageWidth") - target.width()) > 0.01 ||
            std::abs(value("imageHeight") - target.height()) > 0.01) {
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
    bool finished_ = false;
    int outgoingFrames_ = 0;
    qint64 firstReadyNs_ = 0;
    double oldWidth_ = 0;
    double oldHeight_ = 0;
    double oldScale_ = 0;
    double oldPanX_ = 0;
    double oldPanY_ = 0;
};
} // namespace

std::unique_ptr<DiagnosticCheck> makeNavigationTransitionCheck(const QUrl& nextImageUrl)
{
    return std::make_unique<NavigationTransitionCheck>(nextImageUrl);
}
} // namespace LicasaDiagnostics

#include "diagnostics_navigation_transition.moc"
