#include "diagnostics_common.h"

#include "imaging/compute/edit_compute.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QQuickWindow>
#include <QTimer>

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
        if (!warming_ && !result_.contains("full_detail_ready_ms") &&
            !window_->property("previewPhase").toBool()) {
            result_["full_detail_ready_ms"] = elapsed();
        }
    }

    void framePresented()
    {
        if (!result_.contains("first_window_frame_ms")) {
            result_["first_window_frame_ms"] = elapsed();
        }
        if (!imageUrl_.isEmpty()) {
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
                QCoreApplication::quit();
            } else {
                QTimer::singleShot(1000, this, &ViewerMeasurement::releaseImage);
            }
            return;
        }
        if (settling_) {
            return;
        }
        settling_ = true;
        if (!imageUrl_.isEmpty() && !result_.value("image_failed").toBool()) {
            result_["first_preview_presented_ms"] = elapsed();
        }
        QTimer::singleShot(1000, this, &ViewerMeasurement::releaseImage);
    }

    void releaseImage()
    {
        // Wait for an automatic full-detail promotion that is still in flight.
        if (viewport_->property("imageStatus").toInt() != 3 &&
            viewport_->property("pendingImageSlot").toInt() >= 0) {
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
                QCoreApplication::quit();
                return;
            }
            warming_ = true;
            warmStarted_ = monotonicNs();
            QMetaObject::invokeMethod(window_, "loadImageUrl", Qt::DirectConnection,
                                      Q_ARG(QVariant, QVariant::fromValue(imageUrl_)));
        });
    }

  private:
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
