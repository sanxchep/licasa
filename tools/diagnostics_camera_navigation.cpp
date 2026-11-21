#include "diagnostics_common.h"

#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
#include "app/window_manager.h"
#include "imaging/hdr/hdr_image_item.h"
#include "imaging/hdr/output_negotiation.h"
#include "imaging/hdr/render_target_budget.h"
#include "platform/application_identity.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <map>
#include <vector>

namespace LicasaDiagnostics {
namespace {

// Two camera-sized gain-map photos in one production HDR window. Each return
// to a photo must present the same FP16 pixels, a seeded burst of opens must
// settle on the requested photo, a lower memory limit must leave the photos
// on their SDR base, and raising it again must bring HDR back without a
// reopen. The HDR pool must drain when the window closes.
class CameraNavigationCheck final : public QObject, public DiagnosticCheck {
  public:
    CameraNavigationCheck(QUrl first, QUrl second, Licasa::WindowManager* manager, quint32 seed,
                          QString output)
        : first_(std::move(first)), second_(std::move(second)), manager_(manager),
          output_(std::move(output)),
          scale_(std::max(1, qEnvironmentVariableIntValue("LICASA_JOURNEY_TIMEOUT_SCALE")))
    {
        quint32 random = seed;
        const auto next = [&random] {
            random = random * 1664525u + 1013904223u;
            return random >> 16;
        };
        static const char* actions[] = {"first", "second", "ordinary"};
        for (int i = 0; i < 16; ++i) {
            const QString action = QLatin1String(actions[next() % 3]);
            burstPlan_.append(QJsonObject{{"action", action}, {"delay_ms", int(next() % 13)}});
        }
        burstPlan_.append(QJsonObject{{"action", "second"}, {"delay_ms", 0}});
        // Low enough that neither photo fits beside the display targets; those
        // scale with the output, so a smaller output needs a lower limit.
        lowLimit_ = qEnvironmentVariableIsSet("LICASA_CAMERA_LOW_MP")
            ? qEnvironmentVariableIntValue("LICASA_CAMERA_LOW_MP") : 100;
        newWindowOutput_ = qEnvironmentVariableIsSet("LICASA_CAMERA_NEW_WINDOW_OUTPUT")
            ? qEnvironmentVariable("LICASA_CAMERA_NEW_WINDOW_OUTPUT") : QStringLiteral("memory");
        retirementRetry_ = qEnvironmentVariableIntValue("LICASA_CAMERA_RETIREMENT_RETRY") == 1;
        result_ = {{"seed", qint64(seed)}, {"navigation_plan", burstPlan_}, {"low_limit_mp", lowLimit_},
                   {"new_window_output", newWindowOutput_},
                   {"platform", QGuiApplication::platformName()}, {"qt", QString::fromLatin1(qVersion())},
                   {"first", first_.toLocalFile()}, {"second", second_.toLocalFile()},
                   {"first_sha256", fileSha256(first_.toLocalFile())},
                   {"second_sha256", fileSha256(second_.toLocalFile())}, {"timeout_scale", scale_},
                   {"build", Licasa::applicationIdentity().toJson()}};
    }

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        viewport_ = window->findChild<QQuickItem*>(QStringLiteral("imageViewport"));
        preferences_ = window->property("viewerPreferences").value<QObject*>();
        if (!viewport_ || !preferences_ || !manager_ || output_.isEmpty() || QFileInfo::exists(output_) ||
            !QDir().mkpath(output_)) {
            return false;
        }
        QImage ordinary(640, 480, QImage::Format_RGB32);
        ordinary.fill(QColor(35, 185, 93));
        ordinary_ = QUrl::fromLocalFile(QDir(output_).filePath(QStringLiteral("ordinary.png")));
        if (!ordinary.save(ordinary_.toLocalFile())) return false;
        for (const QUrl& url : {first_, second_}) {
            auto sampler = makeHdrFrameSampler(url, [this](QJsonObject frame) {
                QTimer::singleShot(0, this, [this, frame] { received(frame); });
            }, retirementRetry_);
            if (!sampler->attach(window)) return false;
            samplers_[{window, url}] = std::move(sampler);
        }
        startingLimit_ = preferences_->property("maximumImageMegapixels").toInt();
        result_["starting_limit_mp"] = startingLimit_;
        result_["retirement_retry"] = retirementRetry_;
        if (retirementRetry_) {
            buildRetirementPlan();
        } else {
            buildPlan();
        }
        timer_.setInterval(20);
        connect(&timer_, &QTimer::timeout, this, &CameraNavigationCheck::poll);
        elapsed_.start();
        timer_.start();
        QTimer::singleShot(300000 * scale_, this, [this] { finish(QStringLiteral("journey_timed_out")); });
        return true;
    }

  private:
    struct Step {
        QString name;
        std::function<bool()> poll;
        int timeoutMs;
    };

    bool settled(const QUrl& url) const { return settledIn(window_, url); }
    static bool settledIn(QQuickWindow* window, const QUrl& url)
    {
        auto* viewport = window ? window->findChild<QQuickItem*>(QStringLiteral("imageViewport")) : nullptr;
        if (!viewport) return false;
        const QString stage = QUrlQuery(QUrl(viewport->property("activeRequestKey").toString()))
                                  .queryItemValue(QStringLiteral("licasa_stage"));
        return window->property("residentImageUrl").toUrl() == url && !window->property("previewPhase").toBool() &&
               (stage.isEmpty() || stage == QStringLiteral("full")) &&
               viewport->property("imageStatus").toInt() == 1 && !viewport->property("decodePending").toBool() &&
               window->property("hdrCompositionReady").toBool() && window->isExposed();
    }
    bool open(const QUrl& url)
    {
        return QMetaObject::invokeMethod(window_, "loadImageUrl", Qt::DirectConnection,
                                         Q_ARG(QVariant, QVariant(url)), Q_ARG(QVariant, QVariant(true)));
    }
    void then(const QString& name, std::function<bool()> poll, int timeoutMs = 30000)
    {
        plan_.push_back(Step{name, std::move(poll), timeoutMs * scale_});
    }
    void act(const QString& name, std::function<bool()> action)
    {
        then(name, [this, name, action] {
            qInfo("LICASA_CAMERA_STEP %d %s", ++actions_, qPrintable(name));
            if (!action()) fail(QStringLiteral("action_failed:") + name);
            return true;
        });
    }
    void openStep(const QString& name, const QUrl& url)
    {
        act(name, [this, url] { return open(url); });
        then(name + QStringLiteral(":settled"), [this, url] { return settled(url); });
    }
    // The production Open: the window's Open action and its file dialog, which
    // opens the file as a new photo rather than as navigation.
    void dialogOpenStep(const QString& name, const QUrl& url)
    {
        act(name + QStringLiteral(":dialog"), [this] {
            return QMetaObject::invokeMethod(window_, "openFileDialog", Qt::DirectConnection);
        });
        chooseStep(name, url);
        then(name + QStringLiteral(":settled"), [this, url] { return settled(url); });
    }
    void chooseStep(const QString& name, const QUrl& url)
    {
        then(name + QStringLiteral(":choose"), [this, url] {
            // Select through the live Quick fallback picker, as its file delegate
            // does; the public wrapper's selectedFile does not reach it while open.
            for (auto* object : window_->findChildren<QObject*>()) {
                if (!object->inherits("QQuickFileDialogImpl") || !object->property("visible").toBool() ||
                    !object->property("opened").toBool()) {
                    continue;
                }
                if (!object->setProperty("selectedFile", url) || object->property("selectedFile").toUrl() != url ||
                    !QMetaObject::invokeMethod(object, "accept"))
                    fail(QStringLiteral("Could not choose a file in the production Open dialog"));
                return true;
            }
            return false;
        });
    }
    // hdr: the photo's HDR rendition; otherwise its SDR base on HDR output.
    void sampleStep(const QString& name, const QUrl& url, bool hdr, const QString& reference, bool peer = false,
                    const QString& sdrOutput = QString(), int timeoutMs = 60000)
    {
        then(QStringLiteral("sample:") + name, [this, name, url, hdr, reference, peer, sdrOutput] {
            if (!requested_) {
                requested_ = true;
                pending_ = QStringLiteral("camera_%1_%2").arg(++checkpoint_).arg(name);
                auto& sampler = samplers_[{peer ? peer_.data() : window_.data(), url}];
                if (!sampler) {
                    fail(QStringLiteral("No sampler for ") + name);
                    return true;
                }
                // A white of 200 with the HDR photo or its SDR base on the HDR output,
                // or the photo on an SDR output that names why HDR was refused.
                if (!sdrOutput.isEmpty())
                    sampler->sample(pending_, 0.0f, 0.0f, HdrFrameSampler::Content::HdrStill, sdrOutput);
                else {
                    const auto content = hdr ? HdrFrameSampler::Content::HdrStill :
                        retirementRetry_ ? HdrFrameSampler::Content::PendingSdrBaseOnHdr :
                                           HdrFrameSampler::Content::SdrBaseOnHdr;
                    sampler->sample(pending_, 200.0f, 800.0f, content);
                }
                return false;
            }
            if (lastFrame_["phase"].toString() != pending_) return false;
            requested_ = false;
            const QJsonArray values = channels(lastFrame_);
            auto& stored = references_[reference];
            if (stored.isEmpty()) {
                stored = values;
            } else if (!same(stored, values)) {
                result_["mismatch"] = QJsonObject{{"phase", pending_}, {"expected", stored}, {"actual", values}};
                fail(QStringLiteral("The photo's pixels changed: ") + name);
            }
            return true;
        }, timeoutMs);
    }
    void limitStep(const QString& name, int megapixels)
    {
        act(name, [this, megapixels] {
            return preferences_->setProperty("maximumImageMegapixels", megapixels) &&
                   preferences_->property("maximumImageMegapixels").toInt() == megapixels;
        });
    }

    void buildRetirementPlan()
    {
        act("hdr-settings", [this] {
            return Licasa::setHdrMonitorSettings(window_, Licasa::hdrOutputRevision(window_), 200, 800, false);
        });
        // Establish both solo HDR renders at the actual trial budget. The
        // empty window between them ensures the control also starts clean.
        const auto clear = [this](const QString& name) {
            act(name, [this] {
                return QMetaObject::invokeMethod(window_, "clearImageState", Qt::DirectConnection);
            });
            then(name + QStringLiteral(":drained"), [this] {
                const auto used = Licasa::viewerResourceBudget()->used();
                return window_->property("residentImageUrl").toUrl().isEmpty() &&
                       !used.decode && !used.retained && !used.upload;
            });
        };
        then("initial:revealed", [this] { return settled(first_); });
        clear("clear-start");
        openStep("second-reference", second_);
        sampleStep("second_reference", second_, true, "second-hdr");
        clear("clear-reference");
        openStep("first-reference", first_);
        sampleStep("first_reference", first_, true, "first-hdr");
        act("retry-navigate", [this] {
            const auto pool = Licasa::viewerResourceBudget();
            const auto limits = pool->limits();
            result_["budget_bytes"] = QJsonObject{{"decode", qint64(limits.decode)},
                {"retained", qint64(limits.retained)}, {"upload", qint64(limits.upload)},
                {"targets", qint64(limits.targets)}, {"total", qint64(pool->totalLimit())},
                {"gpu", qint64(pool->gpuLimit())}};
            Licasa::HdrImageItem::armRetirementDiagnostic(window_);
            return open(second_);
        });
        then("retry-base:refused", [this] {
            return firstRefusalMs_ >= 0 && settled(second_);
        });
        sampleStep("retry_base", second_, false, "second-sdr");
        // Wait through the first 250 ms timer interval in both builds.
        // Continue independently of whether the implementation scheduled it.
        then("retry-release:ready", [this] {
            return elapsed_.elapsed() >= firstRefusalMs_ + 375;
        });
        act("retry-release", [] {
            Licasa::HdrImageItem::releaseDiagnosticFence();
            return true;
        });
        sampleStep("retry_recovered", second_, true, "second-hdr", false, QString(), 8000);
        then("retry-finish", [this] {
            finish(QString());
            return true;
        });
    }

    void buildPlan()
    {
        act("hdr-settings", [this] {
            return Licasa::setHdrMonitorSettings(window_, Licasa::hdrOutputRevision(window_), 200, 800, false);
        });
        then("first:settled", [this] { return settled(first_); });
        sampleStep("first", first_, true, "first-hdr");
        openStep("second", second_);
        sampleStep("second", second_, true, "second-hdr");
        openStep("first-again", first_);
        sampleStep("first_again", first_, true, "first-hdr");
        act("burst", [this] {
            burst(0);
            return true;
        });
        then("burst:finished", [this] { return burstDone_; });
        then("burst:settled", [this] { return settled(second_); });
        sampleStep("second_after_burst", second_, true, "second-hdr");
        // A lower limit leaves an admitted lease alone; photos opened under it
        // stay on their SDR base. Raising it brings HDR back without a reopen.
        limitStep("limit-low", lowLimit_);
        openStep("first-low", first_);
        sampleStep("first_low_limit", first_, false, "first-sdr");
        openStep("second-low", second_);
        sampleStep("second_low_limit", second_, false, "second-sdr");
        limitStep("limit-restore", 536);
        sampleStep("second_restored_limit", second_, true, "second-hdr");
        openStep("first-restored", first_);
        sampleStep("first_restored_limit", first_, true, "first-hdr");
        // The production Open. In a window without a photo it opens in place;
        // over a shown photo it opens a new window. HDR display targets are
        // capped at 960 MiB across windows and one fullscreen 1080p window
        // reserves 920 MiB, so there the new window shows the photo in SDR
        // with the memory reason while the first window keeps its HDR. On a
        // smaller output the targets fit but the raster does not (sdr-base).
        act("clear", [this] { return QMetaObject::invokeMethod(window_, "clearImageState", Qt::DirectConnection); });
        then("cleared", [this] { return window_->property("residentImageUrl").toUrl().isEmpty(); });
        dialogOpenStep("open-first", first_);
        sampleStep("first_opened", first_, true, "first-hdr");
        act("open-second:dialog", [this] {
            before_ = QGuiApplication::topLevelWindows();
            return QMetaObject::invokeMethod(window_, "openFileDialog", Qt::DirectConnection);
        });
        chooseStep("open-second", second_);
        then("open-second:window", [this] {
            for (auto* candidate : QGuiApplication::topLevelWindows()) {
                auto* quick = qobject_cast<QQuickWindow*>(candidate);
                if (quick && quick != window_ && !before_.contains(candidate) && quick->property("windowSession").isValid()) {
                    peer_ = quick;
                    return true;
                }
            }
            return false;
        });
        act("open-second:attach", [this] {
            auto sampler = makeHdrFrameSampler(second_, [this](QJsonObject frame) {
                QTimer::singleShot(0, this, [this, frame] { received(frame); });
            });
            if (!peer_ || !sampler->attach(peer_)) return false;
            samplers_[{peer_.data(), second_}] = std::move(sampler);
            return true;
        });
        then("open-second:settled", [this] { return peer_ && settledIn(peer_, second_); });
        // "sdr-base": the output is HDR, but the first window's camera raster
        // fills the pool's retained category (one maximum raster, 128 MiB), so
        // the second photo stays on its SDR base.
        if (newWindowOutput_ == QStringLiteral("hdr"))
            sampleStep("second_new_window", second_, true, "second-hdr", true);
        else if (newWindowOutput_ == QStringLiteral("sdr-base"))
            sampleStep("second_new_window", second_, false, "second-new-window", true);
        else
            sampleStep("second_new_window", second_, false, "second-new-window", true, newWindowOutput_);
        act("close-new-window", [this] {
            samplers_.erase({peer_.data(), second_});
            manager_->destroyWindow(peer_.data());
            return true;
        });
        then("new-window-closed", [this] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return !peer_ && settled(first_);
        });
        sampleStep("first_after_window_closed", first_, true, "first-hdr");
        then("finish", [this] {
            finish(QString());
            return true;
        });
    }

    void burst(int index)
    {
        if (done_) return;
        if (index == burstPlan_.size()) {
            burstDone_ = true;
            return;
        }
        const auto entry = burstPlan_[index].toObject();
        const QString action = entry["action"].toString();
        qInfo("LICASA_CAMERA_ACTION %d %s", index + 1, qPrintable(action));
        const QUrl url = action == "first" ? first_ : action == "second" ? second_ : ordinary_;
        if (!open(url)) {
            fail(QStringLiteral("Navigation action failed"));
            return;
        }
        QTimer::singleShot(entry["delay_ms"].toInt(), this, [this, index] { burst(index + 1); });
    }

    void received(QJsonObject frame)
    {
        if (done_) return;
        if (frame.contains("error")) {
            fail(QStringLiteral("sampler:") + frame["error"].toString());
            return;
        }
        const auto used = Licasa::viewerResourceBudget()->used();
        frame["pool_bytes"] = QJsonObject{{"decode", qint64(used.decode)}, {"retained", qint64(used.retained)},
                                          {"upload", qint64(used.upload)}, {"targets", qint64(used.targets)}};
        frame["limit_mp"] = preferences_->property("maximumImageMegapixels").toInt();
        if (retirementRetry_) {
            frame["window"] = QString::number(quintptr(window_.data()), 16);
            frame["budget_revision"] = QString::number(Licasa::viewerResourceBudget()->revision());
        }
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
    static bool same(const QJsonArray& a, const QJsonArray& b)
    {
        if (a.size() != b.size() || a.isEmpty()) return false;
        for (int i = 0; i < a.size(); ++i) {
            const QJsonArray x = a[i].toArray(), y = b[i].toArray();
            if (x.size() != 4 || y.size() != 4) return false;
            for (int c = 0; c < 4; ++c)
                if (std::abs(x[c].toDouble() - y[c].toDouble()) > 0.002 + 0.002 * std::abs(x[c].toDouble()))
                    return false;
        }
        return true;
    }

    void poll()
    {
        if (done_ || index_ >= plan_.size()) return;
        if (retirementRetry_ && firstRefusalMs_ < 0 && Licasa::HdrImageItem::diagnosticRasterRefusals()) {
            firstRefusalMs_ = elapsed_.elapsed();
        }
        Step& step = plan_[index_];
        const qint64 now = elapsed_.elapsed();
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

    void fail(const QString& error) { finish(error); }

    void finish(const QString& error)
    {
        if (done_) return;
        done_ = true;
        timer_.stop();
        result_["status"] = error.isEmpty() ? QStringLiteral("passed") : QStringLiteral("failed");
        if (!error.isEmpty()) result_["error"] = error;
        result_["failed_step"] = index_ < plan_.size() ? plan_[index_].name : QString();
        result_["frames"] = frames_;
        result_["steps"] = steps_;
        if (retirementRetry_) {
            Licasa::HdrImageItem::armRetirementDiagnostic(nullptr);
            qInfo("LICASA_RASTER_RETRY_CLOSE");
            result_["closing_limit_mp"] = preferences_->property("maximumImageMegapixels").toInt();
            result_["closing_budget_revision"] = QString::number(Licasa::viewerResourceBudget()->revision());
            result_["closing_output_revision"] = QString::number(Licasa::hdrOutputRevision(window_));
            result_["closing_pool_upload"] = qint64(Licasa::viewerResourceBudget()->used().upload);
        }
        if (window_ && viewport_) {
            result_["state_at_finish"] = QJsonObject{
                {"resident", window_->property("residentImageUrl").toUrl().toLocalFile()},
                {"current", window_->property("currentImageUrl").toUrl().toLocalFile()},
                {"image_status", viewport_->property("imageStatus").toInt()},
                {"decode_pending", viewport_->property("decodePending").toBool()},
                {"preview_phase", window_->property("previewPhase").toBool()},
                {"request", viewport_->property("activeRequestKey").toString()},
                {"composition_ready", window_->property("hdrCompositionReady").toBool()},
                {"exposed", window_->isExposed()}, {"active", window_->isActive()}};
            QJsonArray dialogs;
            if (auto* loader = window_->findChild<QObject*>(QStringLiteral("openDialogLoader"))) {
                if (auto* dialog = loader->property("item").value<QObject*>())
                    dialogs.append(QJsonObject{{"wrapper_selected", dialog->property("selectedFile").toUrl().toString()},
                                               {"wrapper_visible", dialog->property("visible").toBool()}});
            }
            for (auto* object : window_->findChildren<QObject*>()) {
                if (object->inherits("QQuickFileDialogImpl"))
                    dialogs.append(QJsonObject{{"impl_selected", object->property("selectedFile").toUrl().toString()},
                                               {"impl_folder", object->property("currentFolder").toUrl().toString()},
                                               {"impl_visible", object->property("visible").toBool()},
                                               {"impl_opened", object->property("opened").toBool()}});
            }
            result_["dialogs_at_finish"] = dialogs;
        }
        if (peer_) {
            auto* viewport = peer_->findChild<QQuickItem*>(QStringLiteral("imageViewport"));
            auto* loader = viewport ? viewport->findChild<QQuickItem*>(QStringLiteral("hdrStillImage")) : nullptr;
            auto* photo = loader ? loader->property("item").value<QObject*>() : nullptr;
            const auto settings = Licasa::hdrMonitorSettings(peer_);
            result_["peer_at_finish"] = QJsonObject{
                {"resident", peer_->property("residentImageUrl").toUrl().toLocalFile()},
                {"size", QJsonArray{peer_->width(), peer_->height()}}, {"exposed", peer_->isExposed()},
                {"fullscreen", peer_->property("fullScreenMode").toBool()},
                {"composition_ready", peer_->property("hdrCompositionReady").toBool()},
                {"code", settings.value("code").toString()}, {"white", peer_->property("licasaHdrPaperWhiteNits").toDouble()},
                {"peak", peer_->property("licasaHdrPeakNits").toDouble()},
                {"photo_status", photo ? photo->property("photoStatus").toInt() : -1},
                {"photo_loaded", photo != nullptr}};
        }
        if (preferences_) preferences_->setProperty("maximumImageMegapixels", startingLimit_);
        const int code = error.isEmpty() ? 0 : 1;
        samplers_.clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        manager_->destroyAllWindows();
        drain_.start();
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer, code] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const auto used = Licasa::viewerResourceBudget()->used();
            const bool drained = !used.decode && !used.retained && !used.upload && !used.targets;
            if (!drained && drain_.elapsed() < 5000) return;
            timer->stop();
            result_["drained"] = drained;
            result_["remaining_bytes"] = QJsonObject{{"decode", qint64(used.decode)}, {"retained", qint64(used.retained)},
                                                     {"upload", qint64(used.upload)}, {"targets", qint64(used.targets)}};
            if (retirementRetry_) {
                qInfo("LICASA_RASTER_RETRY_DRAIN %d", drained);
            }
            printJson(result_);
            QCoreApplication::exit(drained ? code : 1);
        });
        timer->start(10);
    }

    QUrl first_, second_, ordinary_;
    Licasa::WindowManager* manager_;
    QString output_;
    int scale_ = 1, startingLimit_ = 0, lowLimit_ = 100;
    QPointer<QQuickWindow> window_;
    QPointer<QQuickItem> viewport_;
    QPointer<QObject> preferences_;
    std::map<std::pair<QQuickWindow*, QUrl>, std::unique_ptr<HdrFrameSampler>> samplers_;
    QPointer<QQuickWindow> peer_;
    std::vector<Step> plan_;
    size_t index_ = 0;
    QTimer timer_;
    QElapsedTimer elapsed_, drain_;
    qint64 stepStarted_ = 0;
    QJsonObject result_, lastFrame_;
    QJsonArray burstPlan_, frames_, steps_;
    QWindowList before_;
    QString newWindowOutput_;
    std::map<QString, QJsonArray> references_;
    QString pending_;
    bool requested_ = false, done_ = false, burstDone_ = false;
    bool retirementRetry_ = false;
    qint64 firstRefusalMs_ = -1;
    int checkpoint_ = 0, actions_ = 0;
};

} // namespace

std::unique_ptr<DiagnosticCheck> makeCameraNavigationCheck(const QUrl& first, const QUrl& second,
                                                           Licasa::WindowManager* manager, quint32 seed,
                                                           const QString& output)
{
    return std::make_unique<CameraNavigationCheck>(first, second, manager, seed, output);
}

} // namespace LicasaDiagnostics
#endif
