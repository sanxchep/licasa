#include "diagnostics_common.h"

#include "media/motion/motion_photo_session.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTimer>

namespace LicasaDiagnostics {
namespace {
// Drive the production Main.qml temporal tray against a real Motion Photo.
// This verifies the QML adapter and lifecycle boundaries without exposing any
// media path/range/frame authority to QML or linking diagnostics to Multimedia.
class MotionPhotoControlsCheck final : public QObject, public DiagnosticCheck {
    Q_OBJECT
    enum class Stage {
        AwaitInitialTray,
        AwaitFirstFrame,
        AwaitPause,
        AwaitScrubFrame,
        AwaitResume,
        AwaitReplayControl,
        AwaitFloatingFrame,
        AwaitFloatingWindow,
        AwaitSingleClickPause,
        AwaitPausedDoubleClick,
        AwaitPausedFloatingWindow,
        AwaitSingleClickResume,
        AwaitPlayingDoubleClick,
        AwaitEditorStop,
        AwaitReturnedTray,
        AwaitReplay,
        AwaitClear,
    };

  public:
    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        session_ =
            window_->findChild<Licasa::MotionPhotoSession*>(QString(), Qt::FindDirectChildrenOnly);
        if (!session_) {
            return false;
        }

        timer_.setInterval(20);
        connect(&timer_, &QTimer::timeout, this, &MotionPhotoControlsCheck::poll);
        timer_.start();
        QTimer::singleShot(30000, this, [this] {
            finish(QStringLiteral("Motion Photo control lifecycle check timed out"));
        });
        return true;
    }

  private:
    QObject* controls() const
    {
        return window_ ? window_->findChild<QObject*>(QStringLiteral("temporalControls")) : nullptr;
    }

    QObject* motionSurface() const
    {
        return window_ ? window_->findChild<QObject*>(QStringLiteral("motionPhotoFrameSurface"))
                       : nullptr;
    }

    bool multimediaMapped() const
    {
        const QJsonArray mappings = processMetrics().value("mapped_optional_libraries").toArray();
        for (const QJsonValue& entry : mappings) {
            const QString path = entry.toString();
            if (path.contains(QStringLiteral("libQt6Multimedia")) ||
                path.contains(QStringLiteral("libffmpegmediaplugin")) ||
                path.contains(QStringLiteral("libavcodec.so")) ||
                path.contains(QStringLiteral("libavformat.so")) ||
                path.contains(QStringLiteral("libavutil.so"))) {
                return true;
            }
        }
        return false;
    }

    bool toggle(QObject* control)
    {
        return control &&
               QMetaObject::invokeMethod(control, "togglePlaybackRequested", Qt::DirectConnection);
    }

    bool seek(QObject* control, double normalized)
    {
        return control && QMetaObject::invokeMethod(control, "seekRequested", Qt::DirectConnection,
                                                    Q_ARG(double, normalized));
    }

    void clickFloatingImage(bool doubleClick = false)
    {
        QObject* viewport = window_->findChild<QObject*>(QStringLiteral("imageViewport"));
        const QPointF point(viewport->property("imageX").toDouble() +
                                viewport->property("imageWidth").toDouble() *
                                    viewport->property("currentScale").toDouble() * 0.5,
                            viewport->property("imageY").toDouble() +
                                viewport->property("imageHeight").toDouble() *
                                    viewport->property("currentScale").toDouble() * 0.5);
        result_["floating_click_image_status"] = viewport->property("imageStatus").toInt();
        result_["floating_click_enabled"] = viewport->property("floatingPlaybackEnabled").toBool();
        result_["floating_click_x"] = point.x();
        result_["floating_click_y"] = point.y();
        const QPointF global = window_->mapToGlobal(point.toPoint());
        const auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
            QMouseEvent event(type, point, global, Qt::LeftButton, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(window_, &event);
        };
        send(QEvent::MouseButtonPress, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        if (doubleClick) {
            // Qt Quick receives the second press before the double-click event,
            // matching QuickTest's mouseDoubleClickSequence contract.
            send(QEvent::MouseButtonPress, Qt::LeftButton);
            send(QEvent::MouseButtonDblClick, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, Qt::NoButton);
        }
        gestureTimer_.restart();
    }

    void poll()
    {
        if (done_ || !window_ || !session_) {
            return;
        }

        QObject* control = controls();
        switch (stage_) {
        case Stage::AwaitInitialTray:
            if (!session_->available() || !control) {
                return;
            }
            if (multimediaMapped()) {
                finish(QStringLiteral("Temporal tray loaded Multimedia before playback intent"));
                return;
            }
            if (!control->property("visible").toBool() ||
                !control->property("timeBased").toBool() || control->property("canStep").toBool()) {
                finish(QStringLiteral(
                    "Motion Photo tray exposed an invalid initial capability state"));
                return;
            }
            result_["tray_visible_before_play"] = true;
            result_["multimedia_absent_before_play"] = true;
            result_["frame_step_disabled"] = true;
            if (!control->property("interactionEnabled").toBool()) {
                return;
            }
            if (!toggle(control)) {
                finish(QStringLiteral("Could not invoke the production play control"));
                return;
            }
            stage_ = Stage::AwaitFirstFrame;
            return;

        case Stage::AwaitFirstFrame:
            if (!session_->active() || !session_->playing() || !session_->seekable() ||
                session_->durationMs() <= 0) {
                return;
            }

            // Pass 1K keeps a naturally-finished Motion Photo session alive,
            // seekable, and replayable until an explicit lifecycle stop. That
            // removes the old reason to pause before presentation: doing so can
            // freeze Qt 6.4's decoder before its first QVideoSink frame and trip
            // the first-frame watchdog. Require the real native presentation
            // surface first, then exercise Pause/Scrub/Resume.
            if (QObject* surface = motionSurface();
                !surface || !surface->property("hasFrame").toBool()) {
                return;
            }
            initialFrameTimestampUs_ = motionSurface()->property("frameTimestampUs").toLongLong();
            result_["first_presentation_frame_visible"] = true;
            result_["first_presentation_timestamp_us"] = double(initialFrameTimestampUs_);

            control = controls();
            if (!control || !control->property("playing").toBool() ||
                control->property("timeLabel").toString().isEmpty()) {
                finish(QStringLiteral("Production tray did not mirror active playback state"));
                return;
            }
            if (!multimediaMapped()) {
                finish(QStringLiteral("Playback became active without the private media backend"));
                return;
            }
            result_["play_from_visible_control"] = true;
            result_["timeline_state_mirrored"] = true;
            result_["multimedia_loaded_after_play"] = true;
            if (!control->property("interactionEnabled").toBool()) {
                result_["transport_disabled_during_active_motion"] = true;
                result_["transport_disable_position_ms"] = double(session_->positionMs());
                result_["transport_disable_duration_ms"] = double(session_->durationMs());
                finish(QStringLiteral("Production temporal controls became disabled while active "
                                      "Motion Photo owned the processing gate"));
                return;
            }
            result_["transport_interaction_enabled_during_active_motion"] = true;
            if (!toggle(control)) {
                finish(QStringLiteral("Could not invoke the production pause control"));
                return;
            }
            stage_ = Stage::AwaitPause;
            return;

        case Stage::AwaitPause:
            if (!session_->active()) {
                result_["pause_lost_before_observable_state"] = true;
                result_["pause_loss_position_ms"] = double(session_->positionMs());
                result_["pause_loss_duration_ms"] = double(session_->durationMs());
                finish(QStringLiteral("Playback became inactive before the visible pause intent "
                                      "reached PausedState"));
                return;
            }
            if (session_->playing()) {
                return;
            }
            control = controls();
            if (!control || control->property("playing").toBool()) {
                return;
            }
            if (!control->property("interactionEnabled").toBool()) {
                result_["transport_disabled_before_scrub"] = true;
                result_["transport_disable_position_ms"] = double(session_->positionMs());
                result_["transport_disable_duration_ms"] = double(session_->durationMs());
                finish(QStringLiteral("Production temporal controls disabled Pause/Seek while "
                                      "Motion Photo remained active"));
                return;
            }
            targetMs_ = session_->durationMs() / 2;
            result_["pause_from_visible_control"] = true;
            if (!seek(control, 0.5)) {
                finish(QStringLiteral("Could not invoke the production scrub control"));
                return;
            }
            stage_ = Stage::AwaitScrubFrame;
            return;

        case Stage::AwaitScrubFrame: {
            const qint64 position = session_->positionMs();
            if (position < std::max<qint64>(0, targetMs_ - 350) || position > targetMs_ + 350) {
                return;
            }
            QObject* surface = motionSurface();
            if (!surface || !surface->property("hasFrame").toBool()) {
                return;
            }
            const qint64 timestampUs = surface->property("frameTimestampUs").toLongLong();
            if (timestampUs == initialFrameTimestampUs_ ||
                timestampUs < std::max<qint64>(0, targetMs_ * 1000 - 100000)) {
                return;
            }
            result_["scrub_from_visible_control"] = true;
            result_["scrub_position_ms"] = double(position);
            result_["scrub_presentation_frame_visible"] = true;
            result_["scrub_presentation_timestamp_us"] = double(timestampUs);
            control = controls();
            if (!toggle(control)) {
                finish(QStringLiteral("Could not resume from the production play control"));
                return;
            }
            stage_ = Stage::AwaitResume;
            return;
        }

        case Stage::AwaitResume:
            if (!session_->playing()) {
                return;
            }
            result_["resume_from_visible_control"] = true;
            // Start a full-length clip for click arbitration; a short clip
            // resumed halfway through could naturally reach EOF during the
            // desktop's double-click interval.
            session_->stop();
            stage_ = Stage::AwaitReplayControl;
            return;

        case Stage::AwaitReplayControl:
            if (!control || !control->property("interactionEnabled").toBool()) {
                return;
            }
            if (!toggle(control)) {
                finish(QStringLiteral("Could not prepare floating playback"));
                return;
            }
            stage_ = Stage::AwaitFloatingFrame;
            return;

        case Stage::AwaitFloatingFrame:
            if (!session_->playing() || !motionSurface() ||
                !motionSurface()->property("hasFrame").toBool()) {
                return;
            }
            if (!QMetaObject::invokeMethod(window_, "exitFullScreenToImage")) {
                finish(QStringLiteral("Could not enter floating mode"));
                return;
            }
            stage_ = Stage::AwaitFloatingWindow;
            return;

        case Stage::AwaitFloatingWindow:
            if (window_->property("fullScreenMode").toBool() || controls()) {
                return;
            }
            if (!session_->active() || !session_->playing()) {
                finish(QStringLiteral("Floating transition interrupted playback"));
                return;
            }
            if (QObject* viewport = window_->findChild<QObject*>(QStringLiteral("imageViewport"));
                !viewport || !viewport->property("playbackInteractionEnabled").toBool() ||
                viewport->property("imageStatus").toInt() != 1) {
                return;
            }
            result_["floating_has_no_transport_object"] = true;
            result_["floating_transition_preserves_playback"] = true;
            clickFloatingImage();
            stage_ = Stage::AwaitSingleClickPause;
            return;

        case Stage::AwaitSingleClickPause:
            if (gestureTimer_.elapsed() <
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50) {
                return;
            }
            if (!session_->active() || session_->playing()) {
                finish(QStringLiteral("Floating single click did not pause playback"));
                return;
            }
            result_["floating_single_click_pauses"] = true;
            targetMs_ = session_->positionMs();
            clickFloatingImage(true);
            stage_ = Stage::AwaitPausedDoubleClick;
            return;

        case Stage::AwaitPausedDoubleClick:
            if (gestureTimer_.elapsed() <
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50) {
                return;
            }
            if (!window_->property("fullScreenMode").toBool() || !controls() ||
                !session_->active() || session_->playing() ||
                qAbs(session_->positionMs() - targetMs_) > 50) {
                finish(QStringLiteral("Paused double click changed playback or position"));
                return;
            }
            result_["paused_double_click_preserves_position"] = true;
            QMetaObject::invokeMethod(window_, "exitFullScreenToImage");
            stage_ = Stage::AwaitPausedFloatingWindow;
            return;

        case Stage::AwaitPausedFloatingWindow:
            if (window_->property("fullScreenMode").toBool() || controls()) {
                return;
            }
            if (!session_->active() || session_->playing() ||
                qAbs(session_->positionMs() - targetMs_) > 50) {
                finish(QStringLiteral("Returning to floating mode lost paused position"));
                return;
            }
            if (QObject* viewport = window_->findChild<QObject*>(QStringLiteral("imageViewport"));
                !viewport || !viewport->property("playbackInteractionEnabled").toBool()) {
                return;
            }
            result_["paused_floating_transition_preserves_position"] = true;
            clickFloatingImage();
            stage_ = Stage::AwaitSingleClickResume;
            return;

        case Stage::AwaitSingleClickResume:
            if (gestureTimer_.elapsed() <
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50) {
                return;
            }
            if (!session_->playing()) {
                finish(QStringLiteral("Floating single click did not resume playback"));
                return;
            }
            result_["floating_single_click_resumes"] = true;
            clickFloatingImage(true);
            stage_ = Stage::AwaitPlayingDoubleClick;
            return;

        case Stage::AwaitPlayingDoubleClick:
            if (gestureTimer_.elapsed() <
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50) {
                return;
            }
            if (!window_->property("fullScreenMode").toBool() || !controls() ||
                !session_->playing()) {
                finish(
                    QStringLiteral("Floating double click failed fullscreen/playback arbitration"));
                return;
            }
            result_["floating_double_click_restores_fullscreen_without_pause"] = true;
            window_->setProperty("editPanelOpen", true);
            stage_ = Stage::AwaitEditorStop;
            return;

        case Stage::AwaitEditorStop:
            if (session_->active() || session_->playing() || controls()) {
                return;
            }
            if (!session_->available()) {
                finish(QStringLiteral(
                    "Entering the editor discarded validated Motion Photo authority"));
                return;
            }
            result_["editor_transition_stops_playback"] = true;
            result_["editor_transition_removes_tray"] = true;
            window_->setProperty("editPanelOpen", false);
            stage_ = Stage::AwaitReturnedTray;
            return;

        case Stage::AwaitReturnedTray:
            control = controls();
            if (!control) {
                return;
            }
            if (!session_->available() || session_->active() || session_->playing()) {
                finish(QStringLiteral("Motion Photo tray returned with stale playback state"));
                return;
            }
            // Loader activation and child binding propagation are not an
            // atomic observation from this external diagnostic. Do not emit
            // the synthetic click until the replacement control has completed
            // the same Motion Photo capability bindings required at startup.
            if (!control->property("visible").toBool() ||
                !control->property("timeBased").toBool() || control->property("canStep").toBool()) {
                return;
            }

            result_["tray_returns_after_editor"] = true;
            if (!control->property("interactionEnabled").toBool()) {
                sawReplayDecodeBlock_ = true;
                return;
            }

            result_["replay_control_ready"] = true;
            result_["replay_waited_for_still_decode"] = sawReplayDecodeBlock_;
            if (!toggle(control)) {
                finish(QStringLiteral("Could not replay after leaving the editor"));
                return;
            }
            stage_ = Stage::AwaitReplay;
            return;

        case Stage::AwaitReplay:
            if (!session_->errorString().isEmpty()) {
                result_["replay_session_error"] = session_->errorString();
                finish(
                    QStringLiteral("Replay after editor failed: %1").arg(session_->errorString()));
                return;
            }
            if (!session_->active() || !session_->playing()) {
                return;
            }
            result_["replay_after_editor"] = true;
            if (!QMetaObject::invokeMethod(window_, "clearImageState", Qt::DirectConnection)) {
                finish(QStringLiteral("Could not invoke production clearImageState"));
                return;
            }
            stage_ = Stage::AwaitClear;
            return;

        case Stage::AwaitClear:
            if (session_->available() || session_->active() || session_->playing() || controls() ||
                motionSurface()) {
                return;
            }
            result_["clear_removes_motion_authority"] = true;
            result_["clear_removes_tray"] = true;
            result_["clear_removes_motion_surface"] = true;
            result_["process_after_clear"] = processMetrics();
            finish();
            return;
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
        result_["final_session_available"] = session_ ? session_->available() : false;
        result_["final_session_active"] = session_ ? session_->active() : false;
        result_["final_session_playing"] = session_ ? session_->playing() : false;
        result_["final_session_error"] = session_ ? session_->errorString() : QString();
        if (QObject* control = controls()) {
            result_["final_control_present"] = true;
            result_["final_control_visible"] = control->property("visible").toBool();
            result_["final_control_time_based"] = control->property("timeBased").toBool();
            result_["final_control_can_step"] = control->property("canStep").toBool();
        } else {
            result_["final_control_present"] = false;
        }
        printJson(result_);
        QCoreApplication::exit(error.isEmpty() ? 0 : 2);
    }

    QQuickWindow* window_ = nullptr;
    Licasa::MotionPhotoSession* session_ = nullptr;
    QTimer timer_;
    QJsonObject result_;
    qint64 targetMs_ = 0;
    qint64 initialFrameTimestampUs_ = -1;
    QElapsedTimer gestureTimer_;
    Stage stage_ = Stage::AwaitInitialTray;
    bool sawReplayDecodeBlock_ = false;
    bool done_ = false;
};

} // namespace

std::unique_ptr<DiagnosticCheck> makeMotionPhotoControlsCheck()
{
    return std::make_unique<MotionPhotoControlsCheck>();
}

} // namespace LicasaDiagnostics

#include "diagnostics_motion_controls.moc"
