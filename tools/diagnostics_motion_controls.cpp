#include "diagnostics_common.h"
#include "diagnostics_playback_click.h"

#include "media/motion/motion_photo_session.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QMetaMethod>
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
        AwaitSavedFrame,
        AwaitClear,
    };

  public:
    explicit MotionPhotoControlsCheck(const QString& frameOutput) : frameOutput_(frameOutput) {}

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        session_ =
            window_->findChild<Licasa::MotionPhotoSession*>(QString(), Qt::FindDirectChildrenOnly);
        if (!session_) {
            return false;
        }

        elapsed_.start();
        QObject* viewport = window_->findChild<QObject*>(QStringLiteral("imageViewport"));
        if (!viewport) {
            return false;
        }
        if (QObject* pointer = viewport->findChild<QObject*>(QStringLiteral("imagePointerLayer"))) {
            connect(pointer, SIGNAL(clicked(QQuickMouseEvent*)), this, SLOT(recordPointerEvent()));
            connect(pointer, SIGNAL(canceled()), this, SLOT(recordPointerEvent()));
        }
        connect(viewport, SIGNAL(playbackToggleRequested()), this, SLOT(recordPointerEvent()));
        recordState(QStringLiteral("attached"));
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

    void clickFloatingImage(bool doubleClick = false, bool expectedPlaying = false)
    {
        QObject* viewport = window_->findChild<QObject*>(QStringLiteral("imageViewport"));
        clickCheck_ = std::make_unique<PlaybackClickCheck>(viewport, expectedPlaying);
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
        recordState(QStringLiteral("floating-click-before"));
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
        recordState(QStringLiteral("floating-click-after"));
    }

    void poll()
    {
        if (done_ || !window_ || !session_) {
            return;
        }
        if (unexpectedFloatingToggle_) {
            finish(QStringLiteral("Floating playback toggled outside a single-click gesture"));
            return;
        }

        QObject* control = controls();
        if (stage_ == Stage::AwaitScrubFrame) {
            const auto state = stateSnapshot();
            if (state != lastScrubState_) {
                lastScrubState_ = state;
                recordState(QStringLiteral("scrub-state"));
            }
        }
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
            setStage(Stage::AwaitFirstFrame);
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
            setStage(Stage::AwaitPause);
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
            recordState(QStringLiteral("seek-intent"));
            if (!seek(control, 0.5)) {
                finish(QStringLiteral("Could not invoke the production scrub control"));
                return;
            }
            setStage(Stage::AwaitScrubFrame);
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
            setStage(Stage::AwaitResume);
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
            setStage(Stage::AwaitReplayControl);
            return;

        case Stage::AwaitReplayControl:
            if (!control || !control->property("interactionEnabled").toBool()) {
                return;
            }
            if (!toggle(control)) {
                finish(QStringLiteral("Could not prepare floating playback"));
                return;
            }
            setStage(Stage::AwaitFloatingFrame);
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
            setStage(Stage::AwaitFloatingWindow);
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
            setStage(Stage::AwaitSingleClickPause);
            clickFloatingImage();
            return;

        case Stage::AwaitSingleClickPause: {
            const auto outcome = clickCheck_->check(session_->active(), session_->playing());
            if (outcome == PlaybackClickCheck::Outcome::Pending) {
                return;
            }
            if (outcome == PlaybackClickCheck::Outcome::Failed) {
                finish(QStringLiteral("Floating single click did not pause playback after its "
                                      "intent and state observation deadline"));
                return;
            }
            result_["floating_pause_intent_observed"] = true;
            result_["floating_single_click_pauses"] = true;
            targetMs_ = session_->positionMs();
            setStage(Stage::AwaitPausedDoubleClick);
            clickFloatingImage(true);
            return;
        }

        case Stage::AwaitPausedDoubleClick:
            if (gestureTimer_.elapsed() <
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50) {
                return;
            }
            if (clickCheck_->requests() != 0 || !window_->property("fullScreenMode").toBool() ||
                !controls() || !session_->active() || session_->playing() ||
                qAbs(session_->positionMs() - targetMs_) > 50) {
                finish(QStringLiteral("Paused double click changed playback or position"));
                return;
            }
            result_["paused_double_click_preserves_position"] = true;
            QMetaObject::invokeMethod(window_, "exitFullScreenToImage");
            setStage(Stage::AwaitPausedFloatingWindow);
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
            setStage(Stage::AwaitSingleClickResume);
            clickFloatingImage(false, true);
            return;

        case Stage::AwaitSingleClickResume: {
            const auto outcome = clickCheck_->check(session_->active(), session_->playing());
            if (outcome == PlaybackClickCheck::Outcome::Pending) {
                return;
            }
            if (outcome == PlaybackClickCheck::Outcome::Failed) {
                finish(QStringLiteral("Floating single click did not resume playback after its "
                                      "intent and state observation deadline"));
                return;
            }
            result_["floating_resume_intent_observed"] = true;
            result_["floating_single_click_resumes"] = true;
            setStage(Stage::AwaitPlayingDoubleClick);
            clickFloatingImage(true, true);
            return;
        }

        case Stage::AwaitPlayingDoubleClick:
            if (gestureTimer_.elapsed() <
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50) {
                return;
            }
            if (clickCheck_->requests() != 0 || !window_->property("fullScreenMode").toBool() ||
                !controls() || !session_->playing()) {
                finish(
                    QStringLiteral("Floating double click failed fullscreen/playback arbitration"));
                return;
            }
            result_["floating_double_click_restores_fullscreen_without_pause"] = true;
            window_->setProperty("editPanelOpen", true);
            setStage(Stage::AwaitEditorStop);
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
            setStage(Stage::AwaitReturnedTray);
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
            setStage(Stage::AwaitReplay);
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
            if (!motionSurface() || !motionSurface()->property("hasFrame").toBool()) {
                return;
            }
            result_["replay_after_editor"] = true;
            if (!frameOutput_.isEmpty()) {
                if (QFileInfo::exists(frameOutput_)) {
                    finish(QStringLiteral("Frame qualification output already exists"));
                    return;
                }
                savedFrameSize_ = motionSurface()->property("frameSize").toSize();
                if (savedFrameSize_.isEmpty() ||
                    !QMetaObject::invokeMethod(
                        window_, "saveCurrentMotionFrame", Qt::DirectConnection,
                        Q_ARG(QVariant, QVariant::fromValue(QUrl::fromLocalFile(frameOutput_))))) {
                    finish(QStringLiteral("Could not invoke production Motion Photo frame save"));
                    return;
                }
                setStage(Stage::AwaitSavedFrame);
                return;
            }
            if (!QMetaObject::invokeMethod(window_, "clearImageState", Qt::DirectConnection)) {
                finish(QStringLiteral("Could not invoke production clearImageState"));
                return;
            }
            setStage(Stage::AwaitClear);
            return;

        case Stage::AwaitSavedFrame: {
            if (!QFileInfo::exists(frameOutput_)) {
                return;
            }
            QImageReader reader(frameOutput_);
            const QImage image = reader.read();
            if (image.isNull() || image.size() != savedFrameSize_) {
                finish(QStringLiteral(
                    "Saved Motion Photo frame does not decode at its captured size"));
                return;
            }
            if (session_->active() || session_->playing()) {
                finish(QStringLiteral("Frame save did not release the active media backend"));
                return;
            }
            result_["captured_frame_saved_and_decoded"] = true;
            result_["frame_save_releases_playback"] = true;
            result_["saved_frame_output"] = frameOutput_;
            result_["saved_frame_width"] = image.width();
            result_["saved_frame_height"] = image.height();
            if (!QMetaObject::invokeMethod(window_, "clearImageState", Qt::DirectConnection)) {
                finish(QStringLiteral("Could not clear after Motion Photo frame save"));
                return;
            }
            setStage(Stage::AwaitClear);
            return;
        }

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

    QString stageName() const
    {
        switch (stage_) {
        case Stage::AwaitInitialTray:
            return QStringLiteral("AwaitInitialTray");
        case Stage::AwaitFirstFrame:
            return QStringLiteral("AwaitFirstFrame");
        case Stage::AwaitPause:
            return QStringLiteral("AwaitPause");
        case Stage::AwaitScrubFrame:
            return QStringLiteral("AwaitScrubFrame");
        case Stage::AwaitResume:
            return QStringLiteral("AwaitResume");
        case Stage::AwaitReplayControl:
            return QStringLiteral("AwaitReplayControl");
        case Stage::AwaitFloatingFrame:
            return QStringLiteral("AwaitFloatingFrame");
        case Stage::AwaitFloatingWindow:
            return QStringLiteral("AwaitFloatingWindow");
        case Stage::AwaitSingleClickPause:
            return QStringLiteral("AwaitSingleClickPause");
        case Stage::AwaitPausedDoubleClick:
            return QStringLiteral("AwaitPausedDoubleClick");
        case Stage::AwaitPausedFloatingWindow:
            return QStringLiteral("AwaitPausedFloatingWindow");
        case Stage::AwaitSingleClickResume:
            return QStringLiteral("AwaitSingleClickResume");
        case Stage::AwaitPlayingDoubleClick:
            return QStringLiteral("AwaitPlayingDoubleClick");
        case Stage::AwaitEditorStop:
            return QStringLiteral("AwaitEditorStop");
        case Stage::AwaitReturnedTray:
            return QStringLiteral("AwaitReturnedTray");
        case Stage::AwaitReplay:
            return QStringLiteral("AwaitReplay");
        case Stage::AwaitSavedFrame:
            return QStringLiteral("AwaitSavedFrame");
        case Stage::AwaitClear:
            return QStringLiteral("AwaitClear");
        }
        return {};
    }

    QJsonObject stateSnapshot() const
    {
        QJsonObject state;
        state["stage"] = stageName();
        state["target_ms"] = double(targetMs_);
        state["floating_toggle_requests"] = clickCheck_ ? clickCheck_->requests() : 0;
        state["window_exposed"] = window_->isExposed();
        state["window_active"] = window_->isActive();
        state["window_visibility"] = int(window_->visibility());
        state["window_width"] = window_->width();
        state["window_height"] = window_->height();
        QObject* viewport = window_->findChild<QObject*>(QStringLiteral("imageViewport"));
        state["viewport_width"] = viewport->property("width").toDouble();
        state["viewport_height"] = viewport->property("height").toDouble();
        state["playback_interaction_enabled"] =
            viewport->property("playbackInteractionEnabled").toBool();
        state["interaction_blocked"] = viewport->property("interactionBlocked").toBool();
        if (QObject* pointer = viewport->findChild<QObject*>(QStringLiteral("imagePointerLayer"))) {
            state["pointer_pressed_on_image"] = pointer->property("pressedOnImage").toBool();
            state["pointer_dragged"] = pointer->property("dragged").toBool();
            state["pointer_enabled"] = pointer->property("enabled").toBool();
            state["pointer_width"] = pointer->property("width").toDouble();
            state["pointer_height"] = pointer->property("height").toDouble();
        }
        if (session_) {
            state["available"] = session_->available();
            state["active"] = session_->active();
            state["playing"] = session_->playing();
            state["seekable"] = session_->seekable();
            state["position_ms"] = double(session_->positionMs());
            state["duration_ms"] = double(session_->durationMs());
            state["error"] = session_->errorString();
        }
        if (QObject* control = controls()) {
            state["control_visible"] = control->property("visible").toBool();
            state["control_playing"] = control->property("playing").toBool();
            state["control_can_scrub"] = control->property("canScrub").toBool();
            state["control_interaction_enabled"] = control->property("interactionEnabled").toBool();
        }
        if (QObject* surface = motionSurface()) {
            state["has_frame"] = surface->property("hasFrame").toBool();
            state["frame_timestamp_us"] =
                double(surface->property("frameTimestampUs").toLongLong());
        }
        return state;
    }

  private slots:
    void recordPointerEvent()
    {
        const QString event =
            QString::fromLatin1(sender()->metaObject()->method(senderSignalIndex()).name());
        if (event == QStringLiteral("playbackToggleRequested") &&
            stage_ != Stage::AwaitSingleClickPause && stage_ != Stage::AwaitSingleClickResume) {
            unexpectedFloatingToggle_ = true;
        }
        recordState(event);
    }

  private:
    void recordState(const QString& event)
    {
        QJsonObject state = stateSnapshot();
        state["event"] = event;
        state["elapsed_ms"] = double(elapsed_.elapsed());
        state["stage_elapsed_ms"] = double(elapsed_.elapsed() - stageStartedMs_);
        // Retain bounded state evidence without logging every poll or changing
        // transport intent, presentation assertions, or the 30-second deadline.
        if (trace_.size() < 128) {
            trace_.append(state);
        }
    }

    void setStage(Stage stage)
    {
        recordState(QStringLiteral("stage-completed"));
        stage_ = stage;
        stageStartedMs_ = elapsed_.elapsed();
        recordState(QStringLiteral("stage-entered"));
    }

    void finish(const QString& error = {})
    {
        if (done_) {
            return;
        }
        recordState(QStringLiteral("finished"));
        result_["state_trace"] = trace_;
        result_["final_state"] = stateSnapshot();
        result_["elapsed_ms"] = double(elapsed_.elapsed());
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
    QString frameOutput_;
    QSize savedFrameSize_;
    qint64 targetMs_ = 0;
    qint64 initialFrameTimestampUs_ = -1;
    QElapsedTimer gestureTimer_;
    QElapsedTimer elapsed_;
    qint64 stageStartedMs_ = 0;
    QJsonArray trace_;
    QJsonObject lastScrubState_;
    std::unique_ptr<PlaybackClickCheck> clickCheck_;
    Stage stage_ = Stage::AwaitInitialTray;
    bool sawReplayDecodeBlock_ = false;
    bool done_ = false;
    bool unexpectedFloatingToggle_ = false;
};

} // namespace

std::unique_ptr<DiagnosticCheck> makeMotionPhotoControlsCheck(const QString& frameOutput)
{
    return std::make_unique<MotionPhotoControlsCheck>(frameOutput);
}

} // namespace LicasaDiagnostics

#include "diagnostics_motion_controls.moc"
