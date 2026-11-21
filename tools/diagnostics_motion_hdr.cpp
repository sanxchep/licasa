#include "diagnostics_common.h"

#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
#include "app/window_manager.h"
#include "imaging/hdr/output_negotiation.h"
#include "imaging/hdr/render_target_budget.h"
#include "media/motion/motion_photo_session.h"
#include "platform/application_identity.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QMutex>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <vector>

namespace LicasaDiagnostics {
namespace {

// One production HDR window plays a real Motion Photo and returns to its HDR
// cover after each viewer action. Every checkpoint is a settled native frame
// with a centre readback; the Wayland verifier pairs it with its commit. The
// cover must come back with the same FP16 pixels, playback must release the
// shared processing gate, and no file may be extracted or changed.
class MotionHdrJourneyCheck final : public QObject, public DiagnosticCheck {
  public:
    MotionHdrJourneyCheck(QUrl image, QUrl peer, Licasa::WindowManager* manager, QMutex* gate,
                          quint32 seed, QString output, int rounds = 0)
        : image_(std::move(image)), peer_(std::move(peer)), manager_(manager), gate_(gate),
          output_(std::move(output)), scale_(std::max(1, qEnvironmentVariableIntValue(
                                              "LICASA_JOURNEY_TIMEOUT_SCALE"))), rounds_(rounds)
    {
        quint32 random = seed;
        const auto next = [&random] {
            random = random * 1664525u + 1013904223u;
            return random >> 16;
        };
        static const char* actions[] = {"ordinary", "peer", "main", "toggle"};
        for (int i = 0; i < 12; ++i) {
            QString action = QLatin1String(actions[next() % 4]);
            const int delay = int(next() % 13);
            if (action == "peer" && peer_.isEmpty()) action = QStringLiteral("ordinary");
            navigation_.append(QJsonObject{{"action", action}, {"delay_ms", delay}});
        }
        navigation_.append(QJsonObject{{"action", "main"}, {"delay_ms", 0}});
        result_ = {{"seed", qint64(seed)}, {"navigation_plan", navigation_},
                   {"platform", QGuiApplication::platformName()}, {"qt", QString::fromLatin1(qVersion())},
                   {"fixture", image_.toLocalFile()}, {"peer", peer_.toLocalFile()},
                   {"output_directory", output_}, {"timeout_scale", scale_}, {"soak_rounds", rounds_},
                   {"build", Licasa::applicationIdentity().toJson()}};
    }

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        viewport_ = window->findChild<QQuickItem*>(QStringLiteral("imageViewport"));
        originalSha_ = fileSha256(image_.toLocalFile());
        if (!viewport_ || originalSha_.isEmpty() || !gate_ || !manager_ || output_.isEmpty() ||
            QFileInfo::exists(output_) || !QDir().mkpath(output_)) {
            return false;
        }
        result_["fixture_sha256"] = originalSha_;
        QImage ordinary(640, 480, QImage::Format_RGB32);
        ordinary.fill(QColor(35, 185, 93));
        ordinaryPath_ = QDir(output_).filePath(QStringLiteral("ordinary.png"));
        if (!ordinary.save(ordinaryPath_)) return false;
        baselineFiles_ = scanFiles();
        sampler_ = makeHdrFrameSampler(image_, [this](QJsonObject frame) {
            QTimer::singleShot(0, this, [this, frame] { received(frame); });
        });
        if (!sampler_->attach(window)) return false;
        if (rounds_ > 0) buildSoakPlan();
        else buildPlan();
        timer_.setInterval(20);
        connect(&timer_, &QTimer::timeout, this, &MotionHdrJourneyCheck::poll);
        elapsed_.start();
        stepStarted_ = 0;
        timer_.start();
        QTimer::singleShot(600000 * scale_, this, [this] { fail(QStringLiteral("journey_timed_out")); });
        return true;
    }

  private:
    struct Step {
        QString name;
        std::function<bool()> poll;
        int timeoutMs;
    };

    // ---- production objects -------------------------------------------------
    Licasa::MotionPhotoSession* session(QQuickWindow* window) const
    {
        return window ? window->findChild<Licasa::MotionPhotoSession*>(QString(), Qt::FindDirectChildrenOnly)
                      : nullptr;
    }
    QObject* controls(QQuickWindow* window) const
    {
        return window ? window->findChild<QObject*>(QStringLiteral("temporalControls")) : nullptr;
    }
    QQuickItem* surface(QQuickWindow* window) const
    {
        return window ? window->findChild<QQuickItem*>(QStringLiteral("motionPhotoFrameSurface")) : nullptr;
    }
    bool frameShown(QQuickWindow* window) const
    {
        auto* item = surface(window);
        return item && item->isVisible() && item->property("hasFrame").toBool();
    }
    qint64 frameTimestamp(QQuickWindow* window) const
    {
        auto* item = surface(window);
        return item ? item->property("frameTimestampUs").toLongLong() : -1;
    }
    QObject* saveService() const { return window_->property("imageSaveService").value<QObject*>(); }
    bool gateFree() const
    {
        if (!gate_->tryLock()) return false;
        gate_->unlock();
        return true;
    }
    bool trayReady(QQuickWindow* window) const
    {
        auto* tray = controls(window);
        auto* motion = session(window);
        return motion && motion->available() && tray && tray->property("visible").toBool() &&
               tray->property("interactionEnabled").toBool() && !motion->active();
    }
    bool settledImage() const
    {
        // Full detail, as a viewer sees it: the preview stage's promotion takes
        // the processing gate, and playback refuses to wait for it.
        const QString stage = QUrlQuery(QUrl(viewport_->property("activeRequestKey").toString()))
                                  .queryItemValue(QStringLiteral("licasa_stage"));
        if (window_->property("previewPhase").toBool() || (!stage.isEmpty() && stage != QStringLiteral("full")))
            return false;
        return viewport_->property("imageStatus").toInt() == 1 && !viewport_->property("decodePending").toBool() &&
               window_->property("residentImageUrl").toUrl() == image_ &&
               window_->property("hdrCompositionReady").toBool();
    }
    bool invoke(QObject* object, const char* method)
    {
        return object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection);
    }
    bool invoke(QObject* object, const char* method, const QVariant& argument)
    {
        return object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection, Q_ARG(QVariant, argument));
    }
    bool openImage(const QUrl& url)
    {
        return QMetaObject::invokeMethod(window_, "loadImageUrl", Qt::DirectConnection,
                                         Q_ARG(QVariant, QVariant(url)), Q_ARG(QVariant, QVariant(true)));
    }

    // Open files that look like extracted video: a known extension or an ISO
    // BMFF header at the start. The Motion Photo itself begins with a JPEG.
    static bool videoLike(const QString& path)
    {
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "mp4" || suffix == "mov" || suffix == "m4v" || suffix == "3gp") return true;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        const QByteArray head = file.read(12);
        return head.size() == 12 && head.mid(4, 4) == "ftyp";
    }
    QJsonArray openVideoFiles() const
    {
        QJsonArray found;
        QDirIterator fds(QStringLiteral("/proc/self/fd"), QDir::Files | QDir::System | QDir::NoDotAndDotDot);
        while (fds.hasNext()) {
            const QString target = QFileInfo(fds.next()).symLinkTarget();
            if (target.startsWith('/') && QFileInfo(target).isFile() && videoLike(target)) found.append(target);
        }
        return found;
    }
    QSet<QString> scanFiles() const
    {
        QSet<QString> files;
        QStringList roots{QDir::tempPath()};
        for (const char* name : {"XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME"}) {
            const QString root = qEnvironmentVariable(name);
            if (!root.isEmpty()) roots.append(root);
        }
        roots.append(QFileInfo(image_.toLocalFile()).absolutePath());
        for (const QString& root : roots) {
            QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            for (int count = 0; it.hasNext() && count < 20000; ++count) files.insert(it.next());
        }
        return files;
    }

    // ---- plan --------------------------------------------------------------
    void then(const QString& name, std::function<bool()> poll, int timeoutMs = 20000)
    {
        plan_.push_back(Step{name, std::move(poll), timeoutMs * scale_});
    }
    void act(const QString& name, std::function<bool()> action)
    {
        then(name, [this, name, action] {
            qInfo("LICASA_MOTION_STEP %d %s", ++actions_, qPrintable(name));
            if (!action()) fail(QStringLiteral("action_failed:") + name);
            return true;
        });
    }
    void sampleStep(const QString& name, bool hdr, HdrFrameSampler::Content content)
    {
        then(QStringLiteral("sample:") + name, [this, name, hdr, content] {
            if (!requested_) {
                requested_ = true;
                pendingPhase_ = QStringLiteral("motion_%1_%2").arg(++checkpoint_).arg(name);
                sampler_->sample(pendingPhase_, hdr ? 200.0f : 0.0f, hdr ? 800.0f : 0.0f, content);
                return false;
            }
            if (!lastFrame_.contains(QStringLiteral("phase")) || lastFrame_["phase"].toString() != pendingPhase_)
                return false;
            requested_ = false;
            return checkFrame(name, hdr, content);
        }, 30000);
    }
    void playStep(const QString& name, QQuickWindow** target = nullptr)
    {
        act(name + QStringLiteral(":play"), [this, target] {
            return invoke(controls(target ? *target : window_.data()), "togglePlaybackRequested");
        });
        then(name + QStringLiteral(":first-frame"), [this, target] {
            QQuickWindow* window = target ? *target : window_.data();
            auto* motion = session(window);
            if (!motion || !motion->active() || !motion->playing() || !frameShown(window)) return false;
            if (!multimediaMapped()) fail(QStringLiteral("Playback presented a frame without the media backend"));
            const QJsonArray open = openVideoFiles();
            if (!open.isEmpty()) {
                result_["open_video_files"] = open;
                fail(QStringLiteral("A video file was opened for playback"));
            }
            ++playbackChecks_;
            return true;
        });
    }
    void pauseStep(const QString& name)
    {
        act(name + QStringLiteral(":pause"), [this] { return invoke(controls(window_), "togglePlaybackRequested"); });
        then(name + QStringLiteral(":paused"), [this] {
            auto* motion = session(window_);
            auto* tray = controls(window_);
            return motion && motion->active() && !motion->playing() && tray && !tray->property("playing").toBool();
        });
    }
    void seekStep(const QString& name, double fraction)
    {
        act(name + QStringLiteral(":seek"), [this, fraction] {
            targetMs_ = qint64(session(window_)->durationMs() * fraction);
            seekFrom_ = frameTimestamp(window_);
            return QMetaObject::invokeMethod(controls(window_), "seekRequested", Qt::DirectConnection,
                                             Q_ARG(double, fraction));
        });
        then(name + QStringLiteral(":seek-frame"), [this] {
            auto* motion = session(window_);
            const qint64 position = motion->positionMs();
            const qint64 timestamp = frameTimestamp(window_);
            if (qAbs(position - targetMs_) > 350 || !frameShown(window_) || timestamp == seekFrom_ ||
                timestamp < std::max<qint64>(0, targetMs_ * 1000 - 100000)) {
                return false;
            }
            pausedTimestampUs_ = timestamp;
            pausedPositionMs_ = position;
            return true;
        });
    }
    void stopStep(const QString& name)
    {
        act(name + QStringLiteral(":stop"), [this] { return invoke(window_, "stopMotionPhotoPlaybackForLifecycle"); });
        releasedStep(name);
    }
    void releasedStep(const QString& name)
    {
        then(name + QStringLiteral(":released"), [this, name] {
            auto* motion = session(window_);
            if (!motion || motion->active() || motion->playing() || !gateFree()) return false;
            gateObservations_.append(QJsonObject{{"step", name}, {"gate_free", true},
                                                 {"elapsed_ms", double(elapsed_.elapsed())}});
            return true;
        }, 5000);
    }
    void coverStep(const QString& name, bool hdr = true)
    {
        then(name + QStringLiteral(":tray"), [this] { return settledImage() && trayReady(window_); });
        sampleStep(name, hdr, HdrFrameSampler::Content::HdrStill);
    }
    void chooseFileStep(const QString& name, bool exporting, const QString& path)
    {
        act(name + QStringLiteral(":dialog"), [this, exporting] {
            return invoke(window_, "openSaveAsDialog", QVariant(exporting));
        });
        then(name + QStringLiteral(":choose"), [this, path] {
            auto* loader = window_->findChild<QObject*>(QStringLiteral("saveDialogLoader"));
            auto* dialog = loader ? loader->property("item").value<QObject*>() : nullptr;
            if (!dialog || !dialog->property("visible").toBool()) return false;
            dialog->setProperty("selectedFile", QUrl::fromLocalFile(path));
            saveRequested_ = elapsed_.elapsed();
            if (!invoke(dialog, "accepted") || !invoke(dialog, "close"))
                fail(QStringLiteral("Could not choose a file in the production save dialog"));
            return true;
        });
        then(name + QStringLiteral(":written"), [this, path, name] {
            auto* service = saveService();
            if (!QFileInfo::exists(path) || !service || service->property("busy").toBool()) return false;
            auto* motion = session(window_);
            saves_.append(QJsonObject{{"step", name}, {"path", path},
                                      {"elapsed_ms", double(elapsed_.elapsed() - saveRequested_)},
                                      {"playback_active_after", motion && motion->active()}});
            return true;
        }, 10000);
    }

    void buildPlan()
    {
        act("hdr-settings", [this] {
            return Licasa::setHdrMonitorSettings(window_, Licasa::hdrOutputRevision(window_), 200, 800, false);
        });
        then("recognized", [this] {
            if (!settledImage() || !trayReady(window_)) return false;
            if (multimediaMapped()) fail(QStringLiteral("Multimedia was loaded before playback"));
            return true;
        }, 30000);
        sampleStep("cover", true, HdrFrameSampler::Content::HdrStill);

        // Play, pause and seek on HDR output, then Use SDR while paused.
        playStep("first");
        sampleStep("playing", true, HdrFrameSampler::Content::MotionFrame);
        pauseStep("first");
        seekStep("first", 0.5);
        sampleStep("paused_seek", true, HdrFrameSampler::Content::MotionFrame);
        act("use-sdr:badge", [this] {
            auto* badge = window_->findChild<QQuickItem*>(QStringLiteral("hdrOutputStatusBadge"));
            return badge && badge->isVisible() && badge->isEnabled() && invoke(badge, "activate");
        });
        then("use-sdr:control", [this] {
            auto* control = window_->findChild<QQuickItem*>(QStringLiteral("hdrUseSdr"));
            if (!control || !control->isVisible() || !control->isEnabled()) return false;
            if (!invoke(control, "activate")) fail(QStringLiteral("Could not activate Use SDR"));
            return true;
        });
        // Changing output replaces the native surface by hiding and showing the
        // window, which the viewer treats as a lifecycle stop.
        releasedStep("use-sdr");
        then("use-sdr:shown", [this] {
            return window_->isExposed() && window_->property("hdrCompositionReady").toBool() &&
                   Licasa::hdrMonitorSettings(window_).value("code").toString() == QStringLiteral("disabled");
        });
        // Close the display settings as a user would once the window is back; a
        // click during the switch is lost, and their scrim dims the photo.
        act("use-sdr:close-settings", [this] {
            return invoke(window_->findChild<QQuickItem*>(QStringLiteral("settingsCloseButton")), "clicked");
        });
        then("use-sdr:settings-closed", [this] {
            auto* scrim = window_->findChild<QQuickItem*>(QStringLiteral("settingsScrim"));
            auto* menu = scrim ? scrim->parentItem() : nullptr;
            return window_->isExposed() && scrim && !scrim->isVisible() && menu &&
                   !menu->property("panelOpen").toBool();
        }, 5000);
        coverStep("cover_sdr", false);

        // Play, pause and seek to the same frame on SDR output, then to the end and replay.
        playStep("sdr");
        sampleStep("playing_sdr", false, HdrFrameSampler::Content::MotionFrame);
        pauseStep("sdr");
        seekStep("sdr", 0.5);
        sampleStep("paused_sdr", false, HdrFrameSampler::Content::MotionFrame);
        act("resume", [this] { return invoke(controls(window_), "togglePlaybackRequested"); });
        then("ended", [this] {
            auto* motion = session(window_);
            return motion->active() && !motion->playing() && motion->durationMs() > 0 &&
                   motion->positionMs() >= motion->durationMs() - 250;
        });
        act("replay", [this] { return invoke(controls(window_), "togglePlaybackRequested"); });
        then("replay:fresh-frame", [this] {
            auto* motion = session(window_);
            if (!motion->playing() || !frameShown(window_) || frameTimestamp(window_) >= 500000) return false;
            result_["replay_fresh_frame_us"] = double(frameTimestamp(window_));
            return true;
        });
        sampleStep("replay_sdr", false, HdrFrameSampler::Content::MotionFrame);
        act("restore-hdr", [this] {
            return Licasa::setHdrMonitorSettings(window_, Licasa::hdrOutputRevision(window_), 200, 800, false);
        });
        releasedStep("restore-hdr");
        coverStep("cover_after_hdr_restore");

        // An explicit stop on HDR output.
        playStep("stop");
        stopStep("stop");
        coverStep("cover_after_stop");

        // Editor entry stops playback; leaving it restores the HDR cover.
        playStep("editor");
        act("editor:open", [this] { return invoke(window_, "setEditPanelOpen", QVariant(true)); });
        then("editor:stopped", [this] {
            auto* motion = session(window_);
            return motion && !motion->active() && !controls(window_) && gateFree();
        });
        act("editor:close", [this] { return invoke(window_, "setEditPanelOpen", QVariant(false)); });
        coverStep("cover_after_editor");

        // A preferred cover frame changes where playback starts, not the still or the file.
        playStep("cover-change");
        pauseStep("cover-change");
        seekStep("cover-change", 0.3);
        act("cover-change:set", [this] {
            preferredTimestampUs_ = pausedTimestampUs_;
            return invoke(controls(window_), "coverFrameRequested");
        });
        then("cover-change:stored", [this] {
            auto* tray = controls(window_);
            return tray && tray->property("coverFramePreferred").toBool() &&
                   tray->property("currentIsPreferredCover").toBool();
        });
        stopStep("cover-change");
        coverStep("cover_after_cover_change");
        // Browse to another photo and back; the stored cover applies to the next playback.
        act("cover-change:away", [this] { return openImage(QUrl::fromLocalFile(ordinaryPath_)); });
        then("cover-change:away-shown", [this] {
            return window_->property("residentImageUrl").toUrl() == QUrl::fromLocalFile(ordinaryPath_) &&
                   viewport_->property("imageStatus").toInt() == 1 && !viewport_->property("decodePending").toBool();
        });
        act("cover-change:reopen", [this] { return openImage(image_); });
        then("cover-change:reopened", [this] { return settledImage() && trayReady(window_); }, 30000);
        act("cover-change:play-again", [this] {
            observedTimestamps_ = {};
            return invoke(controls(window_), "togglePlaybackRequested");
        });
        then("cover-change:applied", [this] {
            // Playback from the start passes the preferred frame too, so require
            // that the first frame is it or that the position jumps onto it.
            auto* motion = session(window_);
            if (!motion->active() || !frameShown(window_)) return false;
            const qint64 position = motion->positionMs();
            const qint64 timestamp = frameTimestamp(window_);
            if (observedTimestamps_.isEmpty() ||
                observedTimestamps_.last().toArray()[1].toInteger() != timestamp) {
                observedTimestamps_.append(QJsonArray{position, timestamp, double(elapsed_.elapsed())});
            }
            const qint64 preferredMs = preferredTimestampUs_ / 1000;
            if (position < preferredMs - 40) return false;
            // The first position at or past the preferred frame must come from a
            // jump onto it, not from playing through from the start.
            const qint64 before = observedTimestamps_.size() > 1
                ? observedTimestamps_[observedTimestamps_.size() - 2].toArray()[0].toInteger() : 0;
            preferredApplied_ = QJsonObject{{"preferred_us", double(preferredTimestampUs_)},
                                            {"first_at_or_past_ms", double(position)},
                                            {"previous_ms", double(before)},
                                            {"observed", observedTimestamps_}};
            if (qAbs(position - preferredMs) > 40 || (observedTimestamps_.size() > 1 && position - before < 200)) {
                fail(QStringLiteral("Playback did not start from the preferred cover frame"));
                return true;
            }
            return true;
        }, 5000);
        stopStep("cover-change-play");
        then("cover-change:tray", [this] { return settledImage() && trayReady(window_); });
        act("cover-change:clear", [this] { return invoke(controls(window_), "coverFrameRequested"); });
        then("cover-change:cleared", [this] {
            return controls(window_) && !controls(window_)->property("coverFramePreferred").toBool();
        });
        coverStep("cover_after_cover_reset");

        // Copy the original and export an SDR copy while the Motion Photo plays or is paused.
        const QString copyPath = QDir(output_).filePath(QStringLiteral("original-copy.jfif"));
        const QString exportPath = QDir(output_).filePath(QStringLiteral("sdr-export.jpg"));
        playStep("copy");
        chooseFileStep("copy", false, copyPath);
        then("copy:verified", [this, copyPath] {
            if (fileSha256(copyPath) != originalSha_) fail(QStringLiteral("The original copy differs from the photo"));
            return true;
        });
        releasedStep("copy");
        coverStep("cover_after_copy");
        playStep("export");
        pauseStep("export");
        chooseFileStep("export", true, exportPath);
        then("export:verified", [this, exportPath] { return verifyExport(exportPath); });
        releasedStep("export");
        coverStep("cover_after_export");

        // Hiding the window stops playback; showing it again restores the HDR cover.
        playStep("hide");
        act("hide:hide", [this] {
            window_->hide();
            return true;
        });
        then("hide:hidden", [this] {
            auto* motion = session(window_);
            return !window_->isVisible() && !window_->isExposed() && motion && !motion->active() && gateFree();
        }, 5000);
        act("hide:show", [this] {
            window_->show();
            window_->requestActivate();
            return true;
        });
        coverStep("cover_after_hide");

        // Rapid navigation away and back during playback, ending on the Motion Photo.
        playStep("navigation");
        act("navigation:burst", [this] {
            burst(0);
            return true;
        });
        then("navigation:finished", [this] { return burstDone_; }, 30000);
        then("navigation:settled", [this] {
            return settledImage() && trayReady(window_) && !frameShown(window_) && gateFree();
        }, 30000);
        sampleStep("cover_after_navigation", true, HdrFrameSampler::Content::HdrStill);

        // A second window plays the same photo and is closed while playing.
        act("peer:open", [this] {
            const auto before = QGuiApplication::topLevelWindows();
            if (!manager_->createWindow(image_, false, true)) return false;
            for (auto* candidate : QGuiApplication::topLevelWindows()) {
                auto* quick = qobject_cast<QQuickWindow*>(candidate);
                if (quick && !before.contains(candidate) && quick->property("windowSession").isValid()) {
                    peerWindow_ = quick;
                    peerRaw_ = quick;
                    return true;
                }
            }
            return false;
        });
        then("peer:ready", [this] { return peerWindow_ && trayReady(peerWindow_); }, 30000);
        playStep("peer", &peerRaw_);
        act("peer:close", [this] {
            manager_->destroyWindow(peerWindow_.data());
            return true;
        });
        then("peer:closed", [this] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return !peerWindow_ && gateFree();
        }, 5000);
        coverStep("cover_after_peer_close");
        then("finish", [this] {
            finish(QString());
            return true;
        });
    }

    // The same lifecycle repeated in one process: each round plays, seeks, runs
    // to the end and replays, enters the editor, browses away and back, and
    // switches the output to SDR and back. Every round ends on the HDR cover
    // with playback stopped, and is measured there.
    void buildSoakPlan()
    {
        act("hdr-settings", [this] {
            return Licasa::setHdrMonitorSettings(window_, Licasa::hdrOutputRevision(window_), 200, 800, false);
        });
        then("recognized", [this] { return settledImage() && trayReady(window_); }, 30000);
        sampleStep("cover", true, HdrFrameSampler::Content::HdrStill);
        for (int round = 1; round <= rounds_; ++round) {
            const QString name = QStringLiteral("round%1").arg(round);
            playStep(name);
            pauseStep(name);
            seekStep(name, 0.5);
            act(name + QStringLiteral(":resume"), [this] { return invoke(controls(window_), "togglePlaybackRequested"); });
            then(name + QStringLiteral(":ended"), [this] {
                auto* motion = session(window_);
                return motion->active() && !motion->playing() && motion->durationMs() > 0 &&
                       motion->positionMs() >= motion->durationMs() - 250;
            });
            act(name + QStringLiteral(":replay"), [this] { return invoke(controls(window_), "togglePlaybackRequested"); });
            then(name + QStringLiteral(":fresh-frame"), [this] {
                auto* motion = session(window_);
                return motion->playing() && frameShown(window_) && frameTimestamp(window_) < 500000;
            });
            act(name + QStringLiteral(":editor-open"), [this] { return invoke(window_, "setEditPanelOpen", QVariant(true)); });
            then(name + QStringLiteral(":editor-stopped"), [this] {
                auto* motion = session(window_);
                return motion && !motion->active() && !controls(window_) && gateFree();
            });
            act(name + QStringLiteral(":editor-close"), [this] { return invoke(window_, "setEditPanelOpen", QVariant(false)); });
            then(name + QStringLiteral(":tray"), [this] { return settledImage() && trayReady(window_); });
            act(name + QStringLiteral(":away"), [this] { return openImage(QUrl::fromLocalFile(ordinaryPath_)); });
            then(name + QStringLiteral(":away-shown"), [this] {
                return window_->property("residentImageUrl").toUrl() == QUrl::fromLocalFile(ordinaryPath_) &&
                       viewport_->property("imageStatus").toInt() == 1 && !viewport_->property("decodePending").toBool();
            });
            act(name + QStringLiteral(":back"), [this] { return openImage(image_); });
            then(name + QStringLiteral(":back-ready"), [this] { return settledImage() && trayReady(window_); }, 30000);
            // Use SDR and back to HDR, as the display settings do: each output
            // change replaces the native surface and its swapchain.
            act(name + QStringLiteral(":use-sdr"), [this] {
                return Licasa::useSdrForMonitor(window_, Licasa::hdrOutputRevision(window_));
            });
            then(name + QStringLiteral(":sdr-shown"), [this] {
                return window_->isExposed() && settledImage() && trayReady(window_) &&
                       Licasa::hdrMonitorSettings(window_).value("code").toString() == QStringLiteral("disabled");
            }, 30000);
            act(name + QStringLiteral(":restore-hdr"), [this] {
                return Licasa::setHdrMonitorSettings(window_, Licasa::hdrOutputRevision(window_), 200, 800, false);
            });
            then(name + QStringLiteral(":settled"), [this] {
                return window_->isExposed() && settledImage() && trayReady(window_) && gateFree() &&
                       Licasa::hdrMonitorSettings(window_).value("code").toString() == QStringLiteral("hdr");
            }, 30000);
            sampleStep(QStringLiteral("soak_cover"), true, HdrFrameSampler::Content::HdrStill);
            act(name + QStringLiteral(":measure"), [this, round] {
                QJsonObject metrics = processMetrics();
                metrics.remove("mapped_optional_libraries");
                metrics.remove("mapped_qt_libraries");
                metrics["round"] = round;
                metrics["descriptors"] = descriptorKinds(image_.toLocalFile());
                const auto used = Licasa::viewerResourceBudget()->used();
                metrics["pool"] = QJsonObject{{"decode", qint64(used.decode)}, {"retained", qint64(used.retained)},
                                              {"upload", qint64(used.upload)}, {"targets", qint64(used.targets)}};
                metrics["gate_free"] = gateFree();
                metrics["elapsed_ms"] = double(elapsed_.elapsed());
                soak_.append(metrics);
                return true;
            });
        }
        then("finish", [this] {
            finish(QString());
            return true;
        });
    }

    void burst(int index)
    {
        if (done_) return;
        if (index == navigation_.size()) {
            burstDone_ = true;
            return;
        }
        const auto entry = navigation_[index].toObject();
        const QString action = entry["action"].toString();
        qInfo("LICASA_MOTION_ACTION %d %s", index + 1, qPrintable(action));
        bool ok = true;
        if (action == "ordinary") ok = openImage(QUrl::fromLocalFile(ordinaryPath_));
        else if (action == "peer") ok = openImage(peer_);
        else if (action == "main") ok = openImage(image_);
        else if (QObject* tray = controls(window_); tray && tray->property("interactionEnabled").toBool())
            ok = invoke(tray, "togglePlaybackRequested");
        navigationTrace_.append(QJsonObject{{"index", index + 1}, {"action", action}, {"invoked", ok},
                                            {"elapsed_ms", double(elapsed_.elapsed())}});
        if (!ok) {
            fail(QStringLiteral("Navigation action failed"));
            return;
        }
        QTimer::singleShot(entry["delay_ms"].toInt(), this, [this, index] { burst(index + 1); });
    }

    bool verifyExport(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("Export is unreadable"));
            return true;
        }
        const QByteArray bytes = file.readAll();
        QImageReader original(image_.toLocalFile());
        original.setAutoTransform(true);
        const QImage upright = original.read();
        QImageReader reader(path);
        const QImage exported = reader.read();
        QJsonObject info{{"bytes", qint64(bytes.size())}, {"sha256", fileSha256(path)},
                         {"width", exported.width()}, {"height", exported.height()},
                         {"upright_width", upright.width()}, {"upright_height", upright.height()},
                         {"has_mpf", bytes.contains(QByteArray("MPF\0", 4))},
                         {"has_gain_map_xmp", bytes.contains("hdrgm")},
                         {"has_motion_xmp", bytes.contains("MotionPhoto") || bytes.contains("MicroVideo")},
                         {"has_video", bytes.contains("ftyp")}};
        result_["export"] = info;
        if (exported.isNull() || exported.size() != upright.size() || info["has_mpf"].toBool() ||
            info["has_gain_map_xmp"].toBool() || info["has_motion_xmp"].toBool() || info["has_video"].toBool()) {
            fail(QStringLiteral("The SDR export is not a plain upright SDR image"));
        }
        if (fileSha256(image_.toLocalFile()) != originalSha_) fail(QStringLiteral("The Motion Photo changed"));
        return true;
    }

    bool multimediaMapped() const
    {
        for (const QJsonValue& entry : processMetrics().value("mapped_optional_libraries").toArray()) {
            const QString path = entry.toString();
            if (path.contains(QStringLiteral("libQt6Multimedia")) || path.contains(QStringLiteral("libffmpegmediaplugin")))
                return true;
        }
        return false;
    }

    // ---- checkpoints --------------------------------------------------------
    void received(QJsonObject frame)
    {
        if (done_) return;
        if (frame.contains("error")) {
            fail(QStringLiteral("sampler:") + frame["error"].toString());
            return;
        }
        auto* motion = session(window_);
        frame["session_available"] = motion && motion->available();
        frame["session_active"] = motion && motion->active();
        frame["session_playing"] = motion && motion->playing();
        frame["session_position_ms"] = motion ? double(motion->positionMs()) : -1.0;
        frame["gate_free"] = gateFree();
        frame["fixture_unchanged"] = fileSha256(image_.toLocalFile()) == originalSha_;
        auto* scrim = window_->findChild<QQuickItem*>(QStringLiteral("settingsScrim"));
        frame["settings_scrim_visible"] = scrim && scrim->isVisible();
        frames_.append(frame);
        lastFrame_ = frame;
    }
    static QJsonArray channels(const QJsonObject& frame)
    {
        QJsonArray values;
        for (const QJsonValue& sample : frame["readback"].toObject()["samples"].toArray())
            values.append(sample.toObject()["rgba"]);
        return values;
    }
    bool checkFrame(const QString& name, bool hdr, HdrFrameSampler::Content content)
    {
        const QJsonObject frame = lastFrame_;
        QString error;
        if (frame["settings_scrim_visible"].toBool()) {
            error = QStringLiteral("A settings scrim covered the checkpoint");
        } else if (!frame["fixture_unchanged"].toBool()) {
            error = QStringLiteral("The Motion Photo changed");
        } else if (content == HdrFrameSampler::Content::HdrStill) {
            const QJsonArray values = channels(frame);
            QJsonArray& reference = hdr ? cover_ : sdrCover_;
            if (frame["session_active"].toBool() || !frame["gate_free"].toBool()) {
                error = QStringLiteral("The cover was sampled with playback holding the gate");
            } else if (reference.isEmpty()) {
                reference = values;
            } else if (!sameChannels(reference, values)) {
                result_["cover_mismatch"] =
                    QJsonObject{{"phase", frame["phase"]}, {"expected", reference}, {"actual", values}};
                error = QStringLiteral("The restored cover differs from the first cover on this output");
            }
        } else if (frame["hdr_still_loaded"].toBool() || !frame["motion_frame"].toBool() ||
                   !frame["session_active"].toBool()) {
            error = QStringLiteral("A playback checkpoint did not show the video frame");
        } else if (name.startsWith("paused") &&
                   qint64(frame["motion_frame_timestamp_us"].toDouble()) != pausedTimestampUs_) {
            error = QStringLiteral("The paused frame changed");
        }
        if (!error.isEmpty()) fail(error + QStringLiteral(": ") + name);
        return true;
    }
    static bool sameChannels(const QJsonArray& a, const QJsonArray& b)
    {
        if (a.size() != b.size() || a.isEmpty()) return false;
        for (int i = 0; i < a.size(); ++i) {
            const QJsonArray x = a[i].toArray(), y = b[i].toArray();
            if (x.size() != 4 || y.size() != 4) return false;
            for (int c = 0; c < 4; ++c) {
                const double p = x[c].toDouble(), q = y[c].toDouble();
                if (std::abs(p - q) > 0.002 + 0.002 * std::abs(p)) return false;
            }
        }
        return true;
    }

    // ---- driver -------------------------------------------------------------
    void poll()
    {
        if (done_) return;
        if (index_ >= plan_.size()) return;
        Step& step = plan_[index_];
        const qint64 now = elapsed_.elapsed();
        if (window_ && stateTrace_.size() < 600) {
            QJsonObject state = snapshot();
            state["tray_interactive"] = controls(window_) && controls(window_)->property("interactionEnabled").toBool();
            state["playback_interaction"] = window_->property("playbackInteractionEnabled").toBool();
            state["request"] = viewport_ ? viewport_->property("activeRequestKey").toString().section('&', -6) : QString();
            if (state != lastState_) {
                lastState_ = state;
                state["step"] = step.name;
                state["elapsed_ms"] = double(now);
                stateTrace_.append(state);
            }
        }
        if (step.poll()) {
            if (done_) return;
            steps_.append(QJsonObject{{"step", step.name}, {"elapsed_ms", double(now)},
                                      {"duration_ms", double(now - stepStarted_)}});
            ++index_;
            stepStarted_ = now;
            return;
        }
        if (now - stepStarted_ > step.timeoutMs) fail(QStringLiteral("timeout:") + step.name);
    }

    QJsonObject snapshot() const
    {
        QJsonObject state;
        if (auto* motion = session(window_)) {
            state["available"] = motion->available();
            state["active"] = motion->active();
            state["playing"] = motion->playing();
            state["position_ms"] = double(motion->positionMs());
            state["duration_ms"] = double(motion->durationMs());
            state["error"] = motion->errorString();
        }
        state["tray"] = controls(window_) != nullptr;
        state["frame_shown"] = frameShown(window_);
        state["frame_timestamp_us"] = double(frameTimestamp(window_));
        if (viewport_) {
            state["image_status"] = viewport_->property("imageStatus").toInt();
            state["decode_pending"] = viewport_->property("decodePending").toBool();
            state["motion_frame_ready"] = viewport_->property("motionFrameReady").toBool();
        }
        state["resident"] = window_->property("residentImageUrl").toUrl().toLocalFile();
        state["window_size"] = QJsonArray{window_->width(), window_->height()};
        state["current_scale"] = window_->property("currentScale").toDouble();
        state["fullscreen"] = window_->property("fullScreenMode").toBool();
        state["fit_mode"] = window_->property("fitMode").toBool();
        state["fit_padding"] = window_->property("fitPadding").toDouble();
        const QSizeF natural = window_->property("naturalImageSize").toSizeF();
        state["natural_size"] = QJsonArray{natural.width(), natural.height()};
        if (viewport_) state["viewport_size"] = QJsonArray{viewport_->width(), viewport_->height()};
        QVariant width, height;
        QMetaObject::invokeMethod(window_, "imageWidth", Qt::DirectConnection, Q_RETURN_ARG(QVariant, width));
        QMetaObject::invokeMethod(window_, "imageHeight", Qt::DirectConnection, Q_RETURN_ARG(QVariant, height));
        state["image_size"] = QJsonArray{width.toDouble(), height.toDouble()};
        state["hdr_composition_ready"] = window_->property("hdrCompositionReady").toBool();
        state["save_busy"] = saveService() && saveService()->property("busy").toBool();
        state["gate_free"] = gateFree();
        return state;
    }

    bool fail(const QString& error)
    {
        finish(error);
        return false;
    }

    void finish(const QString& error)
    {
        if (done_) return;
        done_ = true;
        timer_.stop();
        result_["status"] = error.isEmpty() ? QStringLiteral("passed") : QStringLiteral("failed");
        if (!error.isEmpty()) result_["error"] = error;
        result_["failed_step"] = index_ < plan_.size() ? plan_[index_].name : QString();
        if (window_) result_["state_at_finish"] = snapshot();
        result_["frames"] = frames_;
        result_["steps"] = steps_;
        result_["state_trace"] = stateTrace_;
        result_["saves"] = saves_;
        result_["gate_observations"] = gateObservations_;
        result_["navigation"] = navigationTrace_;
        result_["preferred_cover"] = preferredApplied_;
        result_["soak"] = soak_;
        result_["playback_checks"] = playbackChecks_;
        result_["open_video_files"] = openVideoFiles();
        QJsonArray created;
        for (const QString& path : scanFiles())
            if (!baselineFiles_.contains(path) && !path.startsWith(output_ + '/') && videoLike(path)) created.append(path);
        result_["new_video_files"] = created;
        result_["fixture_sha256_at_finish"] = fileSha256(image_.toLocalFile());
        if (error.isEmpty() && (!created.isEmpty() || result_["fixture_sha256_at_finish"] != originalSha_)) {
            result_["status"] = QStringLiteral("failed");
            result_["error"] = QStringLiteral("A video file was created or the Motion Photo changed");
        }
        const int code = result_["status"] == "passed" ? 0 : 1;
        sampler_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        manager_->destroyAllWindows();
        drain_.start();
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer, code] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const auto used = Licasa::viewerResourceBudget()->used();
            const bool drained = !used.decode && !used.retained && !used.upload && !used.targets;
            const bool released = gateFree();
            if ((!drained || !released) && drain_.elapsed() < 5000) return;
            timer->stop();
            result_["drained"] = drained;
            result_["gate_free_after_close"] = released;
            result_["remaining_bytes"] = QJsonObject{{"decode", qint64(used.decode)}, {"retained", qint64(used.retained)},
                                                     {"upload", qint64(used.upload)}, {"targets", qint64(used.targets)}};
            printJson(result_);
            QCoreApplication::exit(drained && released ? code : 1);
        });
        timer->start(10);
    }

    QUrl image_, peer_;
    Licasa::WindowManager* manager_;
    QMutex* gate_;
    QString output_, ordinaryPath_, originalSha_;
    int scale_ = 1, rounds_ = 0;
    QPointer<QQuickWindow> window_, peerWindow_;
    QQuickWindow* peerRaw_ = nullptr;
    QPointer<QQuickItem> viewport_;
    std::unique_ptr<HdrFrameSampler> sampler_;
    std::vector<Step> plan_;
    size_t index_ = 0;
    QTimer timer_;
    QElapsedTimer elapsed_, drain_;
    qint64 stepStarted_ = 0, saveRequested_ = 0;
    QJsonObject result_, lastFrame_, preferredApplied_;
    QJsonArray navigation_, frames_, steps_, saves_, gateObservations_, navigationTrace_, cover_;
    QJsonArray observedTimestamps_, sdrCover_, stateTrace_, soak_;
    QJsonObject lastState_;
    QSet<QString> baselineFiles_;
    QString pendingPhase_;
    bool requested_ = false, done_ = false, burstDone_ = false;
    int checkpoint_ = 0, playbackChecks_ = 0, actions_ = 0;
    qint64 targetMs_ = 0, seekFrom_ = -1, pausedTimestampUs_ = -1, pausedPositionMs_ = -1;
    qint64 preferredTimestampUs_ = -1;
};

} // namespace

std::unique_ptr<DiagnosticCheck> makeMotionHdrJourneyCheck(const QUrl& image, const QUrl& peer,
                                                           Licasa::WindowManager* manager, QMutex* gate,
                                                           quint32 seed, const QString& output, int rounds)
{
    return std::make_unique<MotionHdrJourneyCheck>(image, peer, manager, gate, seed, output, rounds);
}

} // namespace LicasaDiagnostics
#endif
