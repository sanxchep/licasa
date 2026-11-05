#include "diagnostics_common.h"

#include "imaging/compute/edit_compute.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrlQuery>

namespace LicasaDiagnostics {
namespace {
class ViewerMeasurement final : public QObject, public DiagnosticCheck {
    Q_OBJECT
  public:
    ViewerMeasurement(qint64 started, QUrl imageUrl, int cycles)
        : started_(started), imageUrl_(imageUrl), cycles_(cycles)
    {}

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        viewport_ = window_->findChild<QObject*>(QStringLiteral("imageViewport"));
        if (!viewport_) {
            return false;
        }
        QObject::connect(viewport_, SIGNAL(imageReady()), this, SLOT(imageReady()));
        window_->installEventFilter(this);
        connect(window_, &QQuickWindow::frameSwapped, this, &ViewerMeasurement::framePresented,
                Qt::QueuedConnection);
        QTimer::singleShot(30000 + (cycles_ - 1) * 10000, this, [this] {
            result_["error"] = QStringLiteral("Viewer measurement timed out");
            result_["image_status_at_timeout"] = viewport_->property("imageStatus").toInt();
            result_["pending_image_slot_at_timeout"] =
                viewport_->property("pendingImageSlot").toInt();
            result_["preview_phase_at_timeout"] = window_->property("previewPhase").toBool();
            result_["resident_image_at_timeout"] =
                window_->property("residentImageUrl").toUrl().toString();
            result_["rendered_source_at_timeout"] =
                window_->property("renderedProviderSource").toString();
            result_["active_request_key_at_timeout"] =
                viewport_->property("activeRequestKey").toString();
            result_["window_visible_at_timeout"] = window_->isVisible();
            result_["closed_cycles_at_timeout"] = closedCycles_;
            printJson(result_);
            QCoreApplication::exit(2);
        });
        return true;
    }

  protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == window_ && event->type() == QEvent::Expose && window_->isExposed() &&
            !result_.contains("window_exposed_ms")) {
            result_["window_exposed_ms"] = elapsed();
        }
        return false;
    }

  private slots:
    void imageReady()
    {
        if (!result_.contains("preview_ready_ms")) {
            result_["preview_ready_ms"] = elapsed();
        }
        if (!warming_ && !result_.contains("full_detail_ready_ms") && fullDetailReady()) {
            result_["full_detail_ready_ms"] = elapsed();
        }
    }

    void framePresented()
    {
        if (!result_.contains("first_window_frame_ms")) {
            result_["first_window_frame_ms"] = elapsed();
            result_["source_image_at_first_frame"] =
                window_->property("residentImageUrl").toUrl().toString();
            result_["image_status_at_first_frame"] = viewport_->property("imageStatus").toInt();
            result_["fullscreen_at_first_frame"] = window_->property("fullScreenMode").toBool();
        }
        {
            const int status = viewport_->property("imageStatus").toInt();
            if (status == 3 && !result_.contains("image_failed")) {
                // A failed initial open must still present a usable window.
                result_["image_failed"] = true;
                result_["first_error_presented_ms"] = elapsed();
            } else if (status != 1) {
                return;
            }
        }
        if (warming_) {
            if (!result_.contains("warm_reopen_preview_ms")) {
                result_["warm_reopen_preview_ms"] = (monotonicNs() - warmStarted_) / 1e6;
            }
            warming_ = false;
            if (closedCycles_ >= cycles_) {
                result_["final_process"] = processMetrics();
                printJson(result_);
                QCoreApplication::exit(0);
            } else {
                QTimer::singleShot(1000, this, &ViewerMeasurement::releaseImage);
            }
            return;
        }
        if (!imageUrl_.isEmpty() && result_.contains("full_detail_ready_ms") &&
            !result_.contains("full_detail_presented_ms") && fullDetailReady() &&
            viewport_->property("pendingImageSlot").toInt() < 0) {
            result_["full_detail_presented_ms"] = elapsed();
        }
        if (settling_) {
            return;
        }
        settling_ = true;
        const auto* openButton =
            viewport_->findChild<QQuickItem*>(QStringLiteral("emptyStateOpenButton"));
        const auto* overlay = viewport_->findChild<QQuickItem*>(QStringLiteral("emptyImageState"));
        const QSize naturalSize = window_->property("naturalImageSize").toSize();
        result_["source_image_at_settle"] =
            window_->property("residentImageUrl").toUrl().toString();
        result_["natural_image_width"] = naturalSize.width();
        result_["natural_image_height"] = naturalSize.height();
        result_["image_status_at_settle"] = viewport_->property("imageStatus").toInt();
        result_["image_overlay_visible_at_settle"] = overlay && overlay->isVisible();
        result_["open_button_visible_at_settle"] = openButton && openButton->isVisible();
        result_["open_button_focused_at_settle"] = openButton && openButton->hasActiveFocus();
        result_["assigned_image_at_settle"] =
            !window_->property("residentImageUrl").toUrl().isEmpty();
        // Test-only capture keeps production startup free of screenshot work.
        const QString screenshot = qEnvironmentVariable("LICASA_DIAGNOSTIC_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            result_["screenshot_saved"] = window_->grabWindow().save(screenshot);
        }
        if (!imageUrl_.isEmpty() && !result_.value("image_failed").toBool()) {
            result_["first_preview_presented_ms"] = elapsed();
        }
        QTimer::singleShot(1000, this, &ViewerMeasurement::releaseImage);
    }

    void releaseImage()
    {
        // A completed decode can clear pendingImageSlot before its frame is
        // presented. Keep the first image alive until that observed frame too.
        const bool awaitingFinalFrame = !imageUrl_.isEmpty() &&
                                        result_.contains("full_detail_ready_ms") &&
                                        !result_.contains("full_detail_presented_ms");
        if (viewport_->property("imageStatus").toInt() != 3 &&
            (viewport_->property("pendingImageSlot").toInt() >= 0 || awaitingFinalFrame)) {
            QTimer::singleShot(100, this, &ViewerMeasurement::releaseImage);
            return;
        }
        QJsonObject steady = processMetrics();
        steady["retained_gpu_image_bytes"] = double(Licasa::retainedGpuImageEditBytes());
        if (!result_.contains("steady_process")) {
            result_["steady_process"] = steady;
        }
        QMetaObject::invokeMethod(window_, "clearImageState", Qt::DirectConnection);
        window_->hide();
        QTimer::singleShot(1800, this, [this, steady] {
            QJsonObject idle = processMetrics();
            idle["retained_gpu_image_bytes"] = double(Licasa::retainedGpuImageEditBytes());
            result_["idle_after_close_process"] = idle;
            closedCycles_++;
            auto cycles = result_.value("memory_cycles").toArray();
            cycles.append(QJsonObject{{"steady_process", steady}, {"idle_process", idle}});
            result_["memory_cycles"] = cycles;
            result_["qt_version"] = QString::fromLatin1(qVersion());
            result_["platform"] = QGuiApplication::platformName();
            result_["image"] = imageUrl_.toLocalFile();
            if (imageUrl_.isEmpty() || result_.value("image_failed").toBool()) {
                printJson(result_);
                QCoreApplication::exit(0);
                return;
            }
            warming_ = true;
            warmStarted_ = monotonicNs();
            QMetaObject::invokeMethod(window_, "loadImageUrl", Qt::DirectConnection,
                                      Q_ARG(QVariant, QVariant::fromValue(imageUrl_)),
                                      Q_ARG(QVariant, QVariant(true)));
            window_->show();
        });
    }

  private:
    bool fullDetailReady() const
    {
        if (window_->property("previewPhase").toBool() ||
            window_->property("rawFastPhase").toBool()) {
            return false;
        }
        const QUrl source(viewport_->property("activeRequestKey").toString());
        if (source.isEmpty()) {
            return false;
        }
        const QString stage = QUrlQuery(source).queryItemValue(QStringLiteral("licasa_stage"));
        // Unstaged sources have no separate bounded/full handoff, as on the
        // ordinary animation path. A staged raster must identify final detail.
        return stage.isEmpty() || stage == QStringLiteral("full") ||
               stage == QStringLiteral("raw-interactive");
    }

    double elapsed() const { return (monotonicNs() - started_) / 1e6; }
    qint64 started_;
    qint64 warmStarted_ = 0;
    QUrl imageUrl_;
    QQuickWindow* window_ = nullptr;
    QObject* viewport_ = nullptr;
    QJsonObject result_;
    bool settling_ = false;
    bool warming_ = false;
    int cycles_ = 1;
    int closedCycles_ = 0;
};

} // namespace

std::unique_ptr<DiagnosticCheck> makeViewerMeasurement(qint64 started, const QUrl& imageUrl,
                                                       int cycles)
{
    return std::make_unique<ViewerMeasurement>(started, imageUrl, cycles);
}

} // namespace LicasaDiagnostics

#include "diagnostics_viewer_measurement.moc"
