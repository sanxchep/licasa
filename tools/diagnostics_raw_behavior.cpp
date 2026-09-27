#include "diagnostics_common.h"

#include <QCoreApplication>
#include <QQuickWindow>
#include <QTimer>

#include <algorithm>

namespace LicasaDiagnostics {
namespace {
// Exercise the production QML's progressive RAW path. The preview paints first,
// then the viewer automatically starts full development without waiting for zoom.
class RawBehaviorCheck final : public QObject, public DiagnosticCheck {
    Q_OBJECT
    enum class Stage {
        AwaitFirstRaster = 0,
        AwaitFullDetail = 1,
        AwaitFirstFullEdit = 5,
        AwaitRepeatFullEdit = 6,
    };

  public:
    RawBehaviorCheck(bool previewOnly, bool editOnly)
        : previewOnly_(previewOnly), editOnly_(editOnly)
    {}
    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        viewport_ = window->findChild<QObject*>(QStringLiteral("imageViewport"));
        if (!viewport_) {
            return false;
        }
        QObject::connect(viewport_, SIGNAL(imageReady()), this, SLOT(imageReady()));
        started_ = lastBeat_ = monotonicNs();
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
        timer_.start();
        QTimer::singleShot(30000, this, [this] {
            result_["step_at_timeout"] = static_cast<int>(stage_);
            result_["image_status_at_timeout"] = viewport_->property("imageStatus").toInt();
            result_["pending_image_slot_at_timeout"] =
                viewport_->property("pendingImageSlot").toInt();
            result_["active_request_key_at_timeout"] =
                viewport_->property("activeRequestKey").toString();
            result_["preview_phase_at_timeout"] = window_->property("previewPhase").toBool();
            result_["raw_full_requested_at_timeout"] =
                window_->property("rawFullDetailRequested").toBool();
            finish("RAW behavior check timed out");
        });
        return true;
    }
  private slots:
    void imageReady() { ++readyCount_; }

  private:
    void poll()
    {
        const auto now = monotonicNs();
        maxGapNs_ = std::max(maxGapNs_, now - lastBeat_);
        lastBeat_ = now;
        const int status = viewport_->property("imageStatus").toInt();

        // A failed embedded preview can immediately hand off to an already
        // running full reader. The brief Error state between requests is not
        // the first presented raster.
        if (stage_ == Stage::AwaitFirstRaster && status == 3 &&
            window_->property("rawFullDetailRequested").toBool()) {
            return;
        }

        if (stage_ == Stage::AwaitFirstRaster && (status == 1 || status == 3)) {
            const bool fastIntermediate = window_->property("rawFastPhase").toBool();
            const bool fullAlreadyRequested =
                window_->property("rawFullDetailRequested").toBool() &&
                !window_->property("previewPhase").toBool();
            initialPreviewAvailable_ = status == 1 && !fastIntermediate && !fullAlreadyRequested;
            result_["initial_preview_available"] = initialPreviewAvailable_;
            result_["first_raster_full_detail"] = status == 1 && fullAlreadyRequested;
            result_["raw_fast_intermediate_ready"] = status == 1 && fastIntermediate;
            result_["preview_max_heartbeat_gap_ms"] = maxGapNs_ / 1e6;
            result_["preview_ready_ms"] = (now - started_) / 1e6;
            readyBeforePromotion_ = readyCount_;
            stage_ = Stage::AwaitFullDetail;

            if (previewOnly_) {
                result_["preview_only_verified"] = initialPreviewAvailable_;
                finish(initialPreviewAvailable_ ? QString()
                                                : QStringLiteral("Expected embedded RAW preview"));
                return;
            }

            if (window_->property("imageLimitedToPreview").toBool()) {
                // Give any accidental timer time to fire; policy-limited images
                // must never request a forbidden full development.
                QTimer::singleShot(250, this, [this] {
                    if (window_->property("rawFullDetailRequested").toBool() ||
                        !window_->property("previewPhase").toBool()) {
                        finish("Policy-limited RAW requested full development");
                        return;
                    }
                    result_["full_detail_blocked_by_policy"] = true;
                    finish();
                });
            } else {
                // Production now promotes automatically shortly after the preview
                // paints. Record that point as the start of full-detail loading;
                // zoom must not be required to trigger sensor development.
                detailRequested_ = now;
            }
            return;
        }

        if (stage_ == Stage::AwaitFullDetail &&
            window_->property("imageLimitedToPreview").toBool()) {
            const bool stayedOnPreview = window_->property("previewPhase").toBool() &&
                                         !window_->property("rawFullDetailRequested").toBool();
            if (!stayedOnPreview) {
                finish("Policy-limited RAW left the preview stage");
                return;
            }
            result_["full_detail_blocked_by_policy"] = true;
            finish();
            return;
        }

        if (stage_ == Stage::AwaitFullDetail &&
            !window_->property("imageLimitedToPreview").toBool() &&
            window_->property("rawFullDetailRequested").toBool() &&
            !window_->property("previewPhase").toBool() &&
            !window_->property("rawFastPhase").toBool() &&
            viewport_->property("activeRequestKey")
                .toString()
                .contains(QStringLiteral("licasa_stage=raw-interactive")) &&
            viewport_->property("pendingImageSlot").toInt() < 0 && status == 1 &&
            (!initialPreviewAvailable_ || readyCount_ > readyBeforePromotion_)) {
            const double detailMs = (now - detailRequested_) / 1e6;
            result_["automatic_full_detail_ready"] = true;
            result_["automatic_full_detail_ms"] = detailMs;
            // Keep the historical keys for benchmark/result compatibility.
            result_["explicit_full_detail_ready"] = true;
            result_["explicit_full_detail_ms"] = detailMs;
            result_["full_max_heartbeat_gap_ms"] = maxGapNs_ / 1e6;
            requestEdit(0.6, Stage::AwaitFirstFullEdit);
            return;
        }

        if ((stage_ == Stage::AwaitFirstFullEdit || stage_ == Stage::AwaitRepeatFullEdit) &&
            editedFrameReady() && viewport_->property("pendingImageSlot").toInt() < 0 &&
            status == 1) {
            const double editMs = (now - editRequested_) / 1e6;
            const bool keptFullDetail = window_->property("rawFullDetailRequested").toBool() &&
                                        !window_->property("previewPhase").toBool() &&
                                        !window_->property("rawFastPhase").toBool();
            if (!keptFullDetail) {
                finish("RAW full-detail edit unexpectedly changed decode stage");
                return;
            }
            if (stage_ == Stage::AwaitFirstFullEdit) {
                result_["full_first_edit_ready_ms"] = editMs;
                requestEdit(0.8, Stage::AwaitRepeatFullEdit);
                return;
            }
            result_["full_repeat_edit_ready_ms"] = editMs;
            result_["full_edit_kept_detail"] = true;
            if (editOnly_) {
                result_["edit_only_verified"] = true;
            }
            finish();
        }
    }

    void finish(const QString& error = {})
    {
        if (done_) {
            return;
        }
        done_ = true;
        timer_.stop();
        if (!error.isEmpty()) {
            result_["error"] = error;
        }
        result_["process"] = processMetrics();
        printJson(result_);
        QCoreApplication::exit(error.isEmpty() ? 0 : 2);
    }
    void requestEdit(double exposure, Stage nextStage)
    {
        window_->setProperty("editorExposure", exposure);
        readyBeforeEdit_ = readyCount_;
        expectedEditKeyFragment_ = QStringLiteral("?b=%1").arg(exposure, 0, 'f', 4);
        editRequested_ = monotonicNs();
        stage_ = nextStage;
        QMetaObject::invokeMethod(window_, "scheduleEditedPreviewRefresh", Qt::DirectConnection);
    }
    bool editedFrameReady() const
    {
        return readyCount_ > readyBeforeEdit_ && viewport_->property("activeRequestKey")
                                                     .toString()
                                                     .contains(expectedEditKeyFragment_);
    }
    QQuickWindow* window_ = nullptr;
    QObject* viewport_ = nullptr;
    QTimer timer_;
    qint64 started_ = 0, detailRequested_ = 0, editRequested_ = 0, lastBeat_ = 0, maxGapNs_ = 0;
    Stage stage_ = Stage::AwaitFirstRaster;
    int readyCount_ = 0, readyBeforePromotion_ = 0;
    int readyBeforeEdit_ = 0;
    QString expectedEditKeyFragment_;
    bool initialPreviewAvailable_ = false;
    bool done_ = false;
    bool previewOnly_ = false;
    bool editOnly_ = false;
    QJsonObject result_;
};

} // namespace

std::unique_ptr<DiagnosticCheck> makeRawBehaviorCheck(bool previewOnly, bool editOnly)
{
    return std::make_unique<RawBehaviorCheck>(previewOnly, editOnly);
}

} // namespace LicasaDiagnostics

#include "diagnostics_raw_behavior.moc"
