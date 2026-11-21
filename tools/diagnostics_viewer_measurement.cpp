#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
#include "hdr-probe/surface_format.h"
#include "imaging/hdr/hdr_image_item.h"
#include "imaging/hdr/output_negotiation.h"
#include "imaging/hdr/render_target_budget.h"
#include "imaging/presented_still_sources.h"
#include "io/external_file_identity.h"
#include "platform/application_identity.h"
#include "app/window_manager.h"
#include <qpa/qplatformwindow_p.h>
#include <qpa/qwindowsysteminterface.h>
#include <wayland-client.h>
#include <rhi/qrhi.h>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <cstdio>
#include <unistd.h>
#endif
#include "diagnostics_common.h"

#include "imaging/compute/edit_compute.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <atomic>
#include <QtCore/qfloat16.h>
#include <cstring>
#include <utility>
#include <functional>
#include <array>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QFile>
#include <QDir>

namespace LicasaDiagnostics {
namespace {
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
class WaylandHdrCheck final : public QObject, public DiagnosticCheck {
  public:
    WaylandHdrCheck(const QUrl& imageUrl, const QString& expectedOutput, bool windowChurn,
                    const QString& windowSeed,
                    std::function<void(QJsonObject)> sampleCallback = {})
        : imageUrl_(imageUrl),
          expectHdr_(expectedOutput == "hdr" || expectedOutput == "hdr-calibration"),
          checkCalibration_(expectedOutput == "hdr-calibration"),
          sdrStatus_(expectedOutput == "unsupported" ? "unsupported" : "wayland-sdr"),
          windowChurn_(windowChurn), sampleCallback_(std::move(sampleCallback))
    {
        if (!windowSeed.isEmpty()) {
            quint32 random = windowSeed.toUInt();
            const auto next = [&random] {
                random = random * 1664525u + 1013904223u;
                return random;
            };
            const auto append = [&](const char* action) {
                windowPlan_.append(QJsonObject{{"action", QLatin1String(action)},
                                               {"delay_ms", int((next() >> 16) % 4) * 25}});
            };
            for (const auto* action : {"floating", "zoom", "hide", "restore", "fullscreen"})
                append(action);
            bool fullscreen = true;
            int zoom = 1;
            while (windowPlan_.size() < 100) {
                const auto choice = (next() >> 16) % 4;
                if (choice == 0) {
                    fullscreen = !fullscreen;
                    append(fullscreen ? "fullscreen" : "floating");
                } else if (choice == 1 || windowPlan_.size() == 99) {
                    append(zoom < 2 ? "zoom" : "zoom-out");
                    zoom += zoom < 2 ? 1 : -1;
                } else if (choice == 2) {
                    append(zoom > 0 ? "zoom-out" : "zoom");
                    zoom += zoom > 0 ? -1 : 1;
                } else {
                    append("hide");
                    append("restore");
                }
            }
            result_["window_seed"] = windowSeed;
            result_["window_plan"] = windowPlan_;
        }
    }

    ~WaylandHdrCheck() override
    {
        if (window_) {
            window_->hide();
            window_->releaseResources();
            QObject::disconnect(window_, nullptr, this, nullptr);
        }
    }

    // Receipts name the file behind the presented SDR base instead of the
    // file currently at the path, which a peer window may have replaced.
    static QString identityToken(const std::optional<Licasa::ExternalFileIdentity>& identity)
    {
        return identity ? QStringLiteral("%1:%2:%3:%4.%5:%6.%7").arg(identity->device).arg(identity->inode)
            .arg(identity->size).arg(identity->modifiedSeconds).arg(identity->modifiedNanoseconds)
            .arg(identity->changedSeconds).arg(identity->changedNanoseconds) : QString();
    }
    void expectPresented(const QString& token) { receiptMode_ = true; expectedPresented_ = token; }

    // motion samples a presented Motion Photo frame, which replaces the still.
    void sample(const QString& phase, float white, float peak, const QString& sdrCode = QStringLiteral("disabled"),
                bool photoHdr = true, bool motion = false, bool allowPreparing = false)
    {
        samplePhase_ = phase;
        sampleAllowPreparing_ = allowPreparing;
        sampleWhite_ = white;
        samplePeak_ = peak;
        sampleSdrCode_ = sdrCode;
        samplePhotoHdr_ = photoHdr;
        sampleMotion_ = motion;
        sdrPhotoBaseline_ = ~quint64(0);
        readbackWaiting_ = false;
        settledFrames_ = 0;
        minimumFrame_ = frameSerial_.load() + 1;
        window_->update();
    }

    QJsonObject lastFrame() const { return result_["last_frame"].toObject(); }

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        viewport_ = window->findChild<QQuickItem*>(QStringLiteral("imageViewport"));
        const auto output = Licasa::hdrOutputState(window);
        if (!viewport_ || !output) {
            return false;
        }
        result_["qt"] = QString::fromLatin1(qVersion());
        result_["platform"] = QGuiApplication::platformName();
        const auto path = imageUrl_.toLocalFile();
        sourceIdentity_ = Licasa::externalFileIdentityAtPath(path);
        fixtureHash_ = fileSha256(path);
        if (!sourceIdentity_ || fixtureHash_.isEmpty() ||
            sourceIdentity_ != Licasa::externalFileIdentityAtPath(path)) {
            return false;
        }
        result_["fixture"] = path;
        result_["fixture_sha256"] = fixtureHash_;
        result_["build"] = Licasa::applicationIdentity().toJson();
        connect(
            window, &QQuickWindow::afterRendering, this,
            [this, window, output] {
                if (!readbackRequested_.exchange(false)) {
                    return;
                }
                QJsonObject capture;
                auto* rhi = window->rhi();
                auto* chain = window->swapChain();
                if (rhi && chain && rhi->isRecordingFrame() &&
                    chain->flags().testFlag(QRhiSwapChain::UsedAsTransferSource)) {
                    const QSize pixels = chain->currentPixelSize();
                    const int centerX = readbackX_.load(), centerY = readbackY_.load();
                    if (centerX >= 2 && centerX + 2 < pixels.width() && centerY >= 0 &&
                        centerY < pixels.height()) {
                        QRhiReadbackResult result;
                        QRhiReadbackDescription description;
                        description.setRect(QRect(centerX - 2, centerY, 5, 1));
                        auto* updates = rhi->nextResourceUpdateBatch();
                        updates->readBackTexture(description, &result);
                        chain->currentFrameCommandBuffer()->resourceUpdate(updates);
                        // The pinned Qt grab path also finishes here. Read only a
                        // bounded strip and retain its native FP16 interpretation.
                        if (rhi->finish() == QRhi::FrameOpSuccess) {
                            const bool fp16 = result.format == QRhiTexture::RGBA16F;
                            const bool rgba = result.format == QRhiTexture::RGBA8;
                            const bool bgra = result.format == QRhiTexture::BGRA8;
                            const int stride = fp16 ? 8 : 4;
                            if ((fp16 || rgba || bgra) && result.pixelSize == QSize(5, 1) &&
                                result.data.size() == 5 * stride) {
                                QJsonArray samples;
                                for (int i : {0, 2, 4}) {
                                    QJsonArray channels;
                                    for (int c = 0; c < 4; ++c) {
                                        float value;
                                        if (fp16) {
                                            qfloat16 half;
                                            std::memcpy(
                                                &half, result.data.constData() + i * stride + c * 2,
                                                2);
                                            value = float(half);
                                        } else {
                                            const int component =
                                                bgra && (c == 0 || c == 2) ? 2 - c : c;
                                            value =
                                                uchar(result.data[i * stride + component]) / 255.0f;
                                        }
                                        channels.append(value);
                                    }
                                    samples.append(QJsonObject{{"x", centerX - 2 + i},
                                                               {"y", centerY},
                                                               {"rgba", channels}});
                                }
                                capture = {
                                    {"format", fp16 ? "rgba16f" : "rgba8"},
                                    {"size", QJsonArray{pixels.width(), pixels.height()}},
                                    {"samples", samples},
                                    {"revision", QString::number(output->snapshot().revision)}};
                            }
                        }
                    }
                }
                renderCapture_ = std::move(capture);
                renderHasCapture_ = true;
            },
            Qt::DirectConnection);
        connect(
            window, &QQuickWindow::afterFrameEnd, this,
            [this, window, output] {
                // Pair metadata with this render submission. frameSwapped can be
                // delivered after another frame has advanced the counter.
                auto capture = std::exchange(renderCapture_, QJsonObject{});
                const bool captured = std::exchange(renderHasCapture_, false);
                const auto format = nativeSwapchainFormat(window->swapChain());
                if (!format) {
                    return;
                }
                const auto revision = output->snapshot().revision;
                const bool tagged = Licasa::hdrWaylandSurfaceTagged(window);
                const QSize pixels = window->swapChain()->currentPixelSize();
                const auto serial = ++frameSerial_;
                if (captured && !capture.isEmpty()) {
                    capture["frame_serial"] = qint64(serial);
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, format = *format, revision, tagged, pixels, serial, capture, captured] {
                        if (captured) {
                            readback_ = capture;
                            readbackSerial_ = serial;
                            readbackInFlight_ = false;
                        }
                        frame(format, revision, tagged, pixels, serial);
                    },
                    Qt::QueuedConnection);
            },
            Qt::DirectConnection);
        connect(window, &QObject::destroyed, this, [this] { finish("window_destroyed", 1); });
        if (!sampleCallback_) QTimer::singleShot(!windowPlan_.isEmpty() ? 180000 : windowChurn_ ? 25000 : 15000,
                           this, [this] { finish("timed_out", 1); });
        return true;
    }

  private:
    void frame(VkSurfaceFormatKHR format, quint64 revision, bool tagged, QSize pixels,
               quint64 serial)
    {
        if (finished_ || !window_ || !viewport_ || !window_->isExposed() ||
            serial < minimumFrame_) {
            return;
        }
        if (windowChurn_) {
            result_["window_wait"] =
                QJsonObject{{"logical_size", QJsonArray{window_->width(), window_->height()}},
                            {"pixel_size", QJsonArray{pixels.width(), pixels.height()}},
                            {"fullscreen", window_->property("fullScreenMode").toBool()},
                            {"visibility", int(window_->visibility())},
                            {"readback_ready", Licasa::hdrTargetReadbackReady(window_)},
                            {"code", Licasa::hdrMonitorSettings(window_).value("code").toString()}};
            if (pixels != window_->size() * window_->devicePixelRatio() ||
                !Licasa::hdrTargetReadbackReady(window_)) {
                window_->update();
                return;
            }
        }
        const auto settings = Licasa::hdrMonitorSettings(window_);
        if (settings.value("revision").toString() != QString::number(revision)) {
            return;
        }
        const QString code = settings.value("code").toString();
        const double whiteScale = window_->property("hdrUiWhiteScale").toDouble();
        const auto* loader = viewport_->findChild<QQuickItem*>(QStringLiteral("hdrStillImage"));
        const auto* photo =
            loader ? qobject_cast<Licasa::HdrImageItem*>(loader->property("item").value<QObject*>())
                   : nullptr;
        const bool imageReady = viewport_->isVisible() &&
                                viewport_->property("imageStatus").toInt() == 1 &&
                                window_->property("residentImageUrl").toUrl() == imageUrl_;
        const auto visibleFrames = photo ? photo->visibleHdrFrames() : 0;
        const bool hdrPhoto =
            photo && photo->photoStatus() == Licasa::HdrImageItem::PhotoStatus::Hdr;
        const auto* overlays = window_->findChild<QQuickItem*>(QStringLiteral("infoOverlays"));
        const QString photoLabel =
            overlays ? overlays->property("hdrPhotoStatusText").toString() : QString();
        const QString displayLabel =
            overlays ? overlays->property("hdrStatusText").toString() : QString();
        const QString provider = window_->property("renderedProviderSource").toString();
        const QString request = viewport_->property("activeRequestKey").toString();
        QString presented;
        if (receiptMode_) {
            const auto entry = Licasa::PresentedStillSources::instance().find(
                QUrl(request).toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1));
            if (entry && entry->path == imageUrl_.toLocalFile()) presented = identityToken(entry->identity);
        }
        // The viewport reports Qt's normalized form of the request it built.
        const bool current =
            !provider.isEmpty() && QUrl(request).toString().startsWith(QUrl(provider).toString() + '&') &&
            !viewport_->property("decodePending").toBool() &&
            (receiptMode_ ? !presented.isEmpty() && presented == expectedPresented_
                          : sourceIdentity_ == Licasa::externalFileIdentityAtPath(imageUrl_.toLocalFile()));
        const char* photoStatus = "none";
        if (photo) switch (photo->photoStatus()) {
            case Licasa::HdrImageItem::PhotoStatus::Sdr: photoStatus = "sdr"; break;
            case Licasa::HdrImageItem::PhotoStatus::Preparing: photoStatus = "preparing"; break;
            case Licasa::HdrImageItem::PhotoStatus::Unavailable: photoStatus = "unavailable"; break;
            case Licasa::HdrImageItem::PhotoStatus::Hdr: photoStatus = "hdr"; break;
            default: break;
        }
        quint32 surfaceId = 0;
#if QT_CONFIG(wayland)
        const auto* native = window_->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        if (native && native->surface()) {
            surfaceId = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(native->surface()));
        }
#endif
        QJsonObject receipt{
            {"output", window_->screen() ? window_->screen()->name() : QString()},
            {"code", code},
            {"revision", QString::number(revision)},
            {"vk_format", int(format.format)},
            {"vk_color_space", int(format.colorSpace)},
            {"app_surface_tagged", tagged},
            {"ui_white_scale", whiteScale},
            {"paper_white_nits", window_->property("licasaHdrPaperWhiteNits").toDouble()},
            {"peak_nits", window_->property("licasaHdrPeakNits").toDouble()},
            {"photo_visible_hdr_frames", qint64(visibleFrames)},
            {"photo_rendition", hdrPhoto ? "hdr" : "sdr"},
            {"photo_status", QLatin1String(photoStatus)},
            {"presented_identity", presented},
            {"photo_label", photoLabel},
            {"display_label", displayLabel},
            {"current_request", current ? request : QString()},
            {"fixture_sha256", current ? fixtureHash_ : QString()},
            {"surface_id", int(surfaceId)},
            {"window_token", windowToken_},
            {"logical_size", QJsonArray{window_->width(), window_->height()}},
            {"pixel_size", QJsonArray{pixels.width(), pixels.height()}},
            {"device_pixel_ratio", window_->devicePixelRatio()},
            {"fullscreen", window_->property("fullScreenMode").toBool()},
            {"photo_scale", window_->property("currentScale").toDouble()},
            {"can_remember", settings.value("canRemember").toBool()},
            {"visibility", int(window_->visibility())},
            {"frame_serial", qint64(serial)},
            {"image_ready", imageReady}};
        const auto* motionSurface = window_->findChild<QQuickItem*>(QStringLiteral("motionPhotoFrameSurface"));
        const bool motionFrame = motionSurface && motionSurface->isVisible() &&
                                 motionSurface->property("hasFrame").toBool();
        if (motionReceipts_) {
            receipt["hdr_still_loaded"] = photo != nullptr;
            receipt["motion_frame_ready"] = viewport_->property("motionFrameReady").toBool();
            receipt["motion_frame"] = motionFrame;
            receipt["motion_frame_timestamp_us"] =
                motionFrame ? double(motionSurface->property("frameTimestampUs").toLongLong()) : -1.0;
        }
        result_["last_frame"] = receipt;
        if (!imageReady || !current || !surfaceId || photoLabel.isEmpty() ||
            displayLabel.isEmpty() || !window_->property("hdrCompositionReady").toBool()) {
            return;
        }
        const auto surface = classifySurfaceFormat(format);
        const bool linear = surface == SurfaceFormat::LinearFp16 ||
                            (tagged && surface == SurfaceFormat::PassThrough);
        if (sampleCallback_) {
            if (samplePhase_.isEmpty()) return;
            const bool hdr = sampleWhite_ > 0;
            const bool sdrSurface = (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                format.format == VK_FORMAT_R8G8B8A8_SRGB || format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                format.format == VK_FORMAT_B8G8R8A8_SRGB) &&
                (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
                 format.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT);
            if (sampleMotion_) {
                // A video frame is shown and the HDR still has been unloaded.
                const bool output = hdr ? code == "hdr" && linear &&
                                              receipt["paper_white_nits"].toDouble() == sampleWhite_ &&
                                              receipt["peak_nits"].toDouble() == samplePeak_
                                        : code == sampleSdrCode_ && sdrSurface && !linear && whiteScale == 0;
                if (!output || !motionFrame || photo || visibleFrames) {
                    window_->update();
                    return;
                }
            } else if (hdr && !samplePhotoHdr_) {
                // HDR output whose photo must stay on its presented SDR base.
                const bool sdrBase = qstrcmp(photoStatus, "unavailable") == 0 ||
                    (sampleAllowPreparing_ && qstrcmp(photoStatus, "preparing") == 0);
                if (code != "hdr" || !linear || hdrPhoto || !sdrBase ||
                    receipt["paper_white_nits"].toDouble() != sampleWhite_ ||
                    receipt["peak_nits"].toDouble() != samplePeak_) {
                    sdrPhotoBaseline_ = ~quint64(0);
                    window_->update();
                    return;
                }
                if (sdrPhotoBaseline_ != quint64(visibleFrames)) {
                    // Count settled frames only once no HDR photo frame is drawn.
                    sdrPhotoBaseline_ = quint64(visibleFrames);
                    settledFrames_ = 0;
                    readbackWaiting_ = false;
                    window_->update();
                    return;
                }
                receipt["photo_new_hdr_frames"] = 0;
            } else
            if (hdr ? (code != "hdr" || !linear || !hdrPhoto || !visibleFrames ||
                       receipt["paper_white_nits"].toDouble() != sampleWhite_ ||
                       receipt["peak_nits"].toDouble() != samplePeak_)
                    : (code != sampleSdrCode_ || !sdrSurface || linear || hdrPhoto || visibleFrames || whiteScale != 0)) {
                window_->update();
                return;
            }
            if (!captureFrame(receipt)) return;
            receipt["phase"] = samplePhase_;
            qInfo("LICASA_WAYLAND_FRAME %s %u %llu", qPrintable(samplePhase_), surfaceId,
                  static_cast<unsigned long long>(revision));
            samplePhase_.clear();
            sampleCallback_(receipt);
            return;
        }
        if (!expectHdr_ || switchedToSdr_) {
            if (code == (switchedToSdr_ ? "disabled" : sdrStatus_) && whiteScale == 0 && !linear &&
                !visibleFrames && !hdrPhoto &&
                (!appTaggedHdr_ || enoughPresentationFrames(revision))) {
                if (windowChurn_ && !windowJourneyComplete_ && windowCheckpoint(receipt)) {
                    return;
                }
                qInfo("LICASA_WAYLAND_FRAME sdr %u %llu", surfaceId,
                      static_cast<unsigned long long>(revision));
                finish("passed", 0);
            }
            return;
        }
        const QString sourceRevision = QUrl(request).path(QUrl::FullyEncoded).section('/', 1, 1);
        auto expectedPhoto = imageUrl_;
        expectedPhoto.setFragment(QStringLiteral("licasa-revision-") + sourceRevision);
        if (code != "hdr" || whiteScale <= 0 || !linear || !visibleFrames || !photo->isVisible() ||
            sourceRevision.isEmpty() || photo->source() != expectedPhoto) {
            return;
        }
        if (!hdrPhoto) {
            window_->update(); // The photo's end-of-frame status reaches the GUI separately.
            return;
        }
        if (windowChurn_ && !windowJourneyComplete_ && windowCheckpoint(receipt)) {
            return;
        }
        if (calibrationRequested_ &&
            (receipt["paper_white_nits"].toDouble() != 300 ||
             receipt["peak_nits"].toDouble() != 1200 || !qFuzzyCompare(whiteScale, 3.75))) {
            return;
        }
        if (tagged && !enoughPresentationFrames(revision)) {
            return;
        }
        if (calibrationRequested_) {
            qInfo("LICASA_WAYLAND_FRAME calibrated %u %llu", surfaceId,
                  static_cast<unsigned long long>(revision));
            result_["calibration_frame"] = receipt;
        } else {
            qInfo("LICASA_WAYLAND_FRAME hdr %u %llu", surfaceId,
                  static_cast<unsigned long long>(revision));
            result_["hdr_frame"] = receipt;
        }
        appTaggedHdr_ = tagged;
        if (!calibrationRequested_) {
            auto* badge = window_->findChild<QQuickItem*>(QStringLiteral("hdrOutputStatusBadge"));
            if (!badge || !badge->isVisible() || !badge->isEnabled() ||
                !QMetaObject::invokeMethod(badge, "activate")) {
                finish("display_settings_action_unavailable", 1);
                return;
            }
        }
        if (checkCalibration_ && !calibrationRequested_) {
            calibrationRequested_ = true;
            QTimer::singleShot(0, this, [this] { calibrateFromSettings(); });
            return;
        }
        switchedToSdr_ = true;
        QTimer::singleShot(0, this, [this] {
            auto* useSdr =
                window_ ? window_->findChild<QQuickItem*>(QStringLiteral("hdrUseSdr")) : nullptr;
            if (!useSdr || !useSdr->isVisible() || !useSdr->isEnabled() ||
                !QMetaObject::invokeMethod(useSdr, "activate")) {
                finish("sdr_control_unavailable", 1);
                return;
            }
            result_["used_sdr_control"] = true;
        });
    }

    bool windowCheckpoint(QJsonObject receipt)
    {
        if (waitingWindowAction_) return true;
        const bool fullscreen = !windowPlan_.isEmpty() ? plannedFullscreen_
                                                       : windowStep_ == 0 || windowStep_ == 4;
        if (window_->property("fullScreenMode").toBool() != fullscreen ||
            window_->visibility() != (fullscreen ? QWindow::FullScreen : QWindow::Windowed)) {
            return true;
        }
        const QSize size = window_->size();
        if (windowStep_ == 1 && size == initialSize_) {
            return true;
        }
        if (windowStep_ == 2 && size == floatingSize_) {
            return true;
        }
        if (!captureFrame(receipt)) return true;
        static const char* phases[] = {"window_initial", "window_floating", "window_zoomed",
                                       "window_restored", "window_fullscreen"};
        const QString phase = !windowPlan_.isEmpty() ? QStringLiteral("window_seed_%1").arg(windowStep_)
                                                      : QLatin1String(phases[windowStep_]);
        receipt["phase"] = phase;
        windowFrames_.append(receipt);
        result_["window_frames"] = windowFrames_;
        qInfo("LICASA_WAYLAND_FRAME %s %d %s", qPrintable(phase), receipt["surface_id"].toInt(),
              qPrintable(receipt["revision"].toString()));
        if (windowStep_ == 0) {
            initialSize_ = size;
        }
        if (windowStep_ == 1) {
            floatingSize_ = size;
        }
        ++windowStep_;
        settledFrames_ = 0;
        minimumFrame_ = frameSerial_.load() + 1;
        if (!windowPlan_.isEmpty() && windowAction_ == windowPlan_.size()) {
            windowJourneyComplete_ = true;
            window_->update();
            return true;
        }
        if (windowPlan_.isEmpty() && windowStep_ == 5) windowJourneyComplete_ = true;
        waitingWindowAction_ = true;
        QTimer::singleShot(0, this, [this] { advanceWindow(); });
        return true;
    }

    bool captureFrame(QJsonObject& receipt)
    {
        const auto size = window_->size();
        const bool fullscreen = receipt["fullscreen"].toBool();
        const double scale = receipt["photo_scale"].toDouble();
        if (settlingSize_ != size || ((!windowPlan_.isEmpty() || sampleCallback_) && !qFuzzyCompare(settlingScale_, scale))) {
            settlingSize_ = size;
            settlingScale_ = scale;
            settledFrames_ = 0;
        }
        if (++settledFrames_ < 4) {
            window_->update();
            return false;
        }
        if (!readbackWaiting_) {
            readbackWaiting_ = true;
            readbackInFlight_ = true;
            const auto pixels = receipt["pixel_size"].toArray();
            // The close control covers the center of this deliberately tiny
            // floating fixture. Sample its unobstructed top-left photo area.
            readbackX_ = fullscreen ? pixels[0].toInt() / 2 : 4;
            readbackY_ = fullscreen ? pixels[1].toInt() / 2 : 4;
            if (fullscreen && (!windowPlan_.isEmpty() || sampleCallback_)) {
                // At inspection zoom the grid crosses the center of this
                // even-sized raster. Sample the center of the adjacent cell.
                const int halfCell = qRound(scale * window_->devicePixelRatio() / 2);
                readbackX_ -= halfCell;
                readbackY_ -= halfCell;
            }
            readbackRequested_ = true;
            window_->update();
            return false;
        }
        if (readbackSerial_ != quint64(receipt["frame_serial"].toInteger())) {
            // A capture's GUI receipt can be superseded while resize or source
            // readiness settles. Retry a fresh frame instead of waiting forever
            // for a serial the observer has already skipped.
            if (!readbackInFlight_ &&
                readbackSerial_ < quint64(receipt["frame_serial"].toInteger())) {
                readbackWaiting_ = false;
            }
            window_->update();
            return false;
        }
        if (!sampleCallback_ && windowStep_ == 1 && !discardedCapture_) {
            // Force one lost observer receipt to cover the busy-GUI ordering.
            discardedCapture_ = true;
            result_["discarded_capture_serial"] = qint64(readbackSerial_);
            window_->update();
            return false;
        }
        if (readback_.isEmpty() || readback_["size"] != receipt["pixel_size"] ||
            readback_["revision"] != receipt["revision"]) {
            finish("window_readback_unavailable", 1);
            return false;
        }
        receipt["readback"] = readback_;
        readbackWaiting_ = false;
        return true;
    }

    void sendKey(int key, const QString& text = {})
    {
        window_->requestActivate();
        // Isolated virtual compositors need not provide a physical input seat.
        // Supply the focus notification before exercising Qt's shortcut path.
        QWindowSystemInterface::handleFocusWindowChanged(window_, Qt::ActiveWindowFocusReason);
        QWindowSystemInterface::flushWindowSystemEvents();
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
            window_, QEvent::KeyPress, key, Qt::NoModifier, text);
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
            window_, QEvent::KeyRelease, key, Qt::NoModifier, text);
    }

    void advanceWindow()
    {
        if (!window_ || finished_) {
            return;
        }
        if (!windowPlan_.isEmpty()) {
            const auto entry = windowPlan_[windowAction_].toObject();
            QTimer::singleShot(entry["delay_ms"].toInt(), this, [this, entry] {
                if (!window_ || finished_) return;
                const QString name = entry["action"].toString();
                windowActions_.append(name);
                result_["window_actions"] = windowActions_;
                qInfo("LICASA_WAYLAND_ACTION %d %s", int(windowActions_.size()), qPrintable(name));
                ++windowAction_;
                settledFrames_ = 0;
                minimumFrame_ = frameSerial_.load() + 1;
                if (name == "hide") {
                    window_->hide();
                    QTimer::singleShot(150, this, [this] {
                        if (!window_ || finished_) return;
                        if (window_->isVisible() || window_->isExposed()) {
                            finish("window_did_not_hide", 1);
                            return;
                        }
                        ++hiddenCount_;
                        result_["hidden_count"] = hiddenCount_;
                        advanceWindow();
                    });
                    return;
                }
                if (name == "restore") {
                    window_->show();
                    window_->requestActivate();
                } else if (name == "floating" || name == "fullscreen") {
                    plannedFullscreen_ = name == "fullscreen";
                    sendKey(Qt::Key_F11);
                } else {
                    sendKey(name == "zoom" ? Qt::Key_Plus : Qt::Key_Minus,
                            name == "zoom" ? QStringLiteral("+") : QStringLiteral("-"));
                }
                waitingWindowAction_ = false;
                window_->update();
            });
            return;
        }
        const auto action = [this](const char* name) {
            windowActions_.append(QLatin1String(name));
            qInfo("LICASA_WAYLAND_ACTION %d %s", int(windowActions_.size()), name);
        };
        if (windowStep_ == 1 || windowStep_ == 4) {
            action(windowStep_ == 1 ? "floating" : "fullscreen");
            sendKey(Qt::Key_F11);
        } else if (windowStep_ == 2) {
            action("zoom");
            sendKey(Qt::Key_Plus, QStringLiteral("+"));
        } else if (windowStep_ == 3) {
            action("hide");
            window_->hide();
            QTimer::singleShot(300, this, [this] {
                if (!window_ || finished_) {
                    return;
                }
                if (window_->isVisible() || window_->isExposed()) {
                    finish("window_did_not_hide", 1);
                    return;
                }
                result_["hidden_confirmed"] = true;
                windowActions_.append("restore");
                qInfo("LICASA_WAYLAND_ACTION %d restore", int(windowActions_.size()));
                minimumFrame_ = frameSerial_.load() + 1;
                waitingWindowAction_ = false;
                window_->show();
                window_->requestActivate();
                result_["window_actions"] = windowActions_;
            });
        }
        if (windowStep_ != 3) waitingWindowAction_ = false;
        result_["window_actions"] = windowActions_;
        window_->update();
    }

    void calibrateFromSettings()
    {
        if (!window_) {
            return;
        }
        for (const auto& entry :
             {std::make_pair("hdrPaperWhite", 10), std::make_pair("hdrPeakBrightness", 8)}) {
            auto* control = window_->findChild<QQuickItem*>(QLatin1String(entry.first));
            if (!control || !control->isVisible() || !control->isEnabled()) {
                finish("calibration_control_unavailable", 1);
                return;
            }
            control->forceActiveFocus();
            for (int i = 0; i < entry.second; ++i) {
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
                QCoreApplication::sendEvent(window_, &press);
                QCoreApplication::sendEvent(window_, &release);
            }
        }
        auto* session = window_->findChild<QQuickItem*>(QStringLiteral("hdrUseSession"));
        if (!session || !session->isVisible() || !session->isEnabled() ||
            !QMetaObject::invokeMethod(session, "activate")) {
            finish("calibration_submit_unavailable", 1);
            return;
        }
        result_["used_calibration_controls"] = true;
    }

    bool enoughPresentationFrames(quint64 revision)
    {
        if (presentationRevision_ != revision) {
            presentationRevision_ = revision;
            presentationFrames_ = 0;
        }
        if (++presentationFrames_ >= 4) {
            return true;
        }
        window_->update(); // Keep the surface alive for compositor presentation feedback.
        return false;
    }

    void finish(const char* status, int code)
    {
        if (finished_) {
            return;
        }
        finished_ = true;
        if (sampleCallback_) {
            sampleCallback_(QJsonObject{{"error", QLatin1String(status)}});
            return;
        }
        if (windowChurn_) {
            result_["window_progress"] = QJsonObject{{"step", windowStep_},
                                                     {"settled_frames", settledFrames_},
                                                     {"readback_waiting", readbackWaiting_},
                                                     {"readback_serial", qint64(readbackSerial_)},
                                                     {"frame_serial", qint64(frameSerial_.load())},
                                                     {"readback", readback_}};
        }
        result_["status"] = QLatin1String(status);
        printJson(result_);
        if (window_) {
            window_->close();
        }
        QCoreApplication::exit(code);
    }

    QUrl imageUrl_;
    std::optional<Licasa::ExternalFileIdentity> sourceIdentity_;
    QString fixtureHash_;
    bool expectHdr_;
    bool checkCalibration_;
    QString sdrStatus_;
    bool windowChurn_ = false;
    QString windowToken_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::atomic<quint64> frameSerial_{0};
    // Render-owner-only strip, delivered with its frame metadata in one GUI call.
    QJsonObject renderCapture_;
    bool renderHasCapture_ = false;
    std::atomic<bool> readbackRequested_{false};
    std::atomic<int> readbackX_{0}, readbackY_{0};
    bool readbackWaiting_ = false;
    bool readbackInFlight_ = false;
    bool discardedCapture_ = false;
    quint64 readbackSerial_ = 0;
    QJsonObject readback_;
    quint64 minimumFrame_ = 0;
    int windowStep_ = 0, settledFrames_ = 0;
    QSize initialSize_, floatingSize_, settlingSize_;
    double settlingScale_ = 0;
    QJsonArray windowFrames_, windowActions_;
    QJsonArray windowPlan_;
    int windowAction_ = 0, hiddenCount_ = 0;
    bool plannedFullscreen_ = true, windowJourneyComplete_ = false;
    bool waitingWindowAction_ = false;
    bool calibrationRequested_ = false;
    bool switchedToSdr_ = false;
    bool appTaggedHdr_ = false;
    bool finished_ = false;
    quint64 presentationRevision_ = 0;
    int presentationFrames_ = 0;
    QPointer<QQuickWindow> window_;
    QPointer<QQuickItem> viewport_;
    QJsonObject result_;
    std::function<void(QJsonObject)> sampleCallback_;
    QString samplePhase_;
    QString sampleSdrCode_;
    float sampleWhite_ = 0, samplePeak_ = 0;
    bool samplePhotoHdr_ = true, receiptMode_ = false, sampleMotion_ = false;
    bool sampleAllowPreparing_ = false;

  public:
    void recordMotionState() { motionReceipts_ = true; }

  private:
    bool motionReceipts_ = false;
    QString expectedPresented_;
    quint64 sdrPhotoBaseline_ = ~quint64(0);

};


class OutputMigrationCheck final : public QObject, public DiagnosticCheck {
  public:
    OutputMigrationCheck(QUrl image, Licasa::WindowManager* manager, quint32 seed, int cycles)
        : image_(std::move(image)), manager_(manager), cycles_(cycles)
    {
        quint32 random=seed;
        const auto append=[&](const char* action) {
            random=random*1664525u+1013904223u;
            plan_.append(QJsonObject{{"action",QLatin1String(action)},
                                     {"delay_ms",int((random>>16)%4)*25}});
        };
        for (int cycle=0; cycle<cycles_; ++cycle)
            for (const char* action : {"move_hdr","calibrate","move_sdr","move_hdr","calibrate",
                  "off_hdr","on_hdr","move_hdr","calibrate","off_sdr","on_sdr","move_sdr"})
                append(action);
        for (const char* action : {"move_hdr","calibrate","move_sdr","use_sdr"}) append(action);
        result_={{"seed",qint64(seed)},{"cycles",cycles_},{"plan",plan_},{"platform",QGuiApplication::platformName()},
                 {"qt",QString::fromLatin1(qVersion())},{"fixture",image_.toLocalFile()},
                 {"fixture_sha256",fileSha256(image_.toLocalFile())},
                 {"build",Licasa::applicationIdentity().toJson()}};
    }

    bool attach(QQuickWindow* window) override
    {
        window_=window;
        observer_=std::make_unique<WaylandHdrCheck>(image_,"hdr",true,QString{},
            [this](QJsonObject frame) { QTimer::singleShot(0,this,[this,frame] { received(frame); }); });
        if (!observer_->attach(window)) return false;
        connect(&poll_,&QTimer::timeout,this,[this] { poll(); });
        poll_.start(20);
        checkpoint_.start();
        QTimer::singleShot(300000*std::max(1,cycles_/8),this,[this] { finish("timed_out",1); });
        return true;
    }

  private:
    QStringList screens() const {
        QStringList names;
        for (auto* screen : QGuiApplication::screens()) names.append(screen->name());
        names.sort();
        return names;
    }
    void move(const QString& output) {
        QMetaObject::invokeMethod(window_,"setFullScreenState",Q_ARG(QVariant,QVariant(false)));
        moving_=true;
        QTimer::singleShot(200,this,[this,output] {
            if (finished_) return;
            QMetaObject::invokeMethod(window_,"fitToWindow");
            const auto* native=window_->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
            if (!native || !native->surface()) { finish("move_surface_missing",1); return; }
            moveTarget_=output;
            command(QStringLiteral("move %1 %2").arg(output).arg(
                wl_proxy_get_id(reinterpret_cast<wl_proxy*>(native->surface()))));
            moving_=false;
        });
    }
    void command(const QString& operation) {
        QSaveFile file(QDir(qEnvironmentVariable("XDG_RUNTIME_DIR")).filePath("output-command"));
        const auto bytes=QStringLiteral("%1 %2\n").arg(step_+1).arg(operation).toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
            finish("compositor_control_write_failed",1); return;
        }
        controlPending_=true;
    }
    void poll() {
        if (!finished_ && checkpoint_.elapsed()>15000) { finish("checkpoint_timed_out",1); return; }
        if (!finished_ && window_) window_->update();
        if (finished_ || moving_) return;
        if (!started_) {
            if (screens()!=QStringList{"headless","sdr"} || !observer_->lastFrame()["image_ready"].toBool()) return;
            started_=true;
            expectedOutput_="sdr";
            expectedCode_="wayland-sdr";
            move("sdr");
            return;
        }
        if (controlPending_) {
            QFile reply(QDir(qEnvironmentVariable("XDG_RUNTIME_DIR")).filePath("output-reply"));
            if (!reply.open(QIODevice::ReadOnly)) return;
            const auto fields=reply.readAll().trimmed().split(' ');
            if (fields.size()!=2 || fields[0].toInt()!=step_+1) return;
            if (fields[1]!="ok") { finish("compositor_control_failed",1); return; }
            controlPending_=false;
        }
        if (!moveTarget_.isEmpty()) {
            if (!window_->screen() || window_->screen()->name()!=moveTarget_) return;
            moveTarget_.clear();
        }
        if (samplePending_ || screens().size()!=expectedCount_ || !window_->screen() ||
            window_->screen()->name()!=expectedOutput_) return;
        samplePending_=true;
        observer_->sample(QStringLiteral("output_%1").arg(step_),expectedCode_=="hdr"?200:0,
                          expectedCode_=="hdr"?800:0,expectedCode_);
    }
    void advance(const QJsonObject& entry) {
        if (finished_) return;
        ++step_;
        checkpoint_.restart();
        const QString action=entry["action"].toString();
        oldRevision_=Licasa::hdrOutputRevision(window_);
        qInfo("LICASA_OUTPUT_ACTION %d %s",step_,qPrintable(action));
        expectedCode_="calibration";
        expectedOutput_="headless";
        if (action=="move_hdr" || action=="move_sdr") {
            expectedOutput_=action=="move_hdr"?"headless":"sdr";
            expectedCode_=action=="move_hdr"?"calibration":"wayland-sdr";
            move(expectedOutput_);
        } else if (action=="calibrate") {
            expectedCode_="hdr";
            if (!Licasa::setHdrMonitorSettings(window_,oldRevision_,200,800,false)) {
                finish("calibration_failed",1); return;
            }
        } else if (action=="use_sdr") {
            expectedOutput_="sdr"; expectedCode_="disabled";
            if (!Licasa::useSdrForMonitor(window_,oldRevision_)) {
                finish("sdr_request_failed",1); return;
            }
        } else {
            const bool hdr=action.endsWith("hdr");
            const bool on=action.startsWith("on");
            const QString name=hdr?"headless":"sdr";
            expectedCount_=on?2:1;
            if (hdr) { expectedOutput_="sdr"; expectedCode_="wayland-sdr"; }
            command(QStringLiteral("%1 %2").arg(on?"on":"off").arg(name));
        }
        samplePending_=false;
    }
    void received(QJsonObject frame) {
        if (finished_) return;
        if (frame.contains("error")) { finish(frame["error"].toString(),1); return; }
        if (frame["output"].toString()!=expectedOutput_ || screens().size()!=expectedCount_) {
            samplePending_=false;
            return;
        }
        QJsonArray names;
        for (const auto& name : screens()) names.append(name);
        frame["screens"]=names;
        frame["old_revision"]=QString::number(oldRevision_);
        if (step_ && plan_[step_-1].toObject()["action"]!="calibrate" &&
            plan_[step_-1].toObject()["action"]!="use_sdr") {
            frame["stale_rejected"]=!Licasa::setHdrMonitorSettings(window_,oldRevision_,200,800,false);
            if (!frame["stale_rejected"].toBool()) { finish("stale_calibration_accepted",1); return; }
        }
        // Kept as compact text, so the receipt's own growth is known exactly
        // when a long session's live allocations are measured.
        frameTexts_.append(QJsonDocument(frame).toJson(QJsonDocument::Compact));
        receiptBytes_+=frameTexts_.constLast().size();
        // Twice per cycle, each time in the same state: calibrated HDR on the HDR
        // output after the cycle's last hotplug of it, and the cycle's end on
        // the SDR output, both heads connected each time.
        if (step_<=12*cycles_ && (step_%12==0 || step_%12==9)) measure(step_%12==0 ? "sdr" : "hdr");
        if (step_==plan_.size()) { finish("passed",0); return; }
        const auto entry=plan_[step_].toObject();
        QTimer::singleShot(entry["delay_ms"].toInt(),this,[this,entry] { advance(entry); });
    }
    // The resources a long session of moves, recalibrations and hotplugs must
    // retire, measured in the same two states in every cycle.
    void measure(const char* phase) {
        QJsonObject metrics=processMetrics();
        metrics.remove("mapped_optional_libraries");
        metrics.remove("mapped_qt_libraries");
        metrics["step"]=step_;
        metrics["phase"]=QLatin1String(phase);
        metrics["descriptors"]=descriptorKinds(image_.toLocalFile());
        const auto used=Licasa::viewerResourceBudget()->used();
        metrics["pool"]=QJsonObject{{"decode",qint64(used.decode)},{"retained",qint64(used.retained)},
                                    {"upload",qint64(used.upload)},{"targets",qint64(used.targets)}};
        metrics["screens"]=int(QGuiApplication::screens().size());
        metrics["windows"]=int(QGuiApplication::topLevelWindows().size());
        metrics["receipt_bytes"]=receiptBytes_;
        soak_.append(metrics);
    }
    void finish(const QString& status,int code) {
        if (finished_) return;
        finished_=true; poll_.stop();
        result_["status"]=status;
        result_["completed_actions"]=step_;
        QJsonArray frames;
        for (const QByteArray& text : frameTexts_) frames.append(QJsonDocument::fromJson(text).object());
        result_["frames"]=frames;
        result_["soak"]=soak_;
        result_["last_settings"]=window_?QJsonObject::fromVariantMap(Licasa::hdrMonitorSettings(window_)):QJsonObject{};
        result_["last_screens"]=QJsonArray::fromStringList(screens());
        result_["last_frame"]=observer_?observer_->lastFrame():QJsonObject{};
        observer_.reset();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        manager_->destroyAllWindows();
        drain_.start();
        auto* timer=new QTimer(this);
        connect(timer,&QTimer::timeout,this,[this,timer,code] {
            const auto bytes=Licasa::viewerResourceBudget()->used();
            const bool drained=!bytes.decode&&!bytes.retained&&!bytes.upload&&!bytes.targets;
            if (!drained && drain_.elapsed()<5000) return;
            timer->stop();
            result_["drained"]=drained;
            result_["remaining_bytes"]=QJsonObject{{"decode",qint64(bytes.decode)},
                {"retained",qint64(bytes.retained)},{"upload",qint64(bytes.upload)},
                {"targets",qint64(bytes.targets)}};
            printJson(result_);
            QCoreApplication::exit(drained?code:1);
        });
        timer->start(10);
    }
    QUrl image_;
    Licasa::WindowManager* manager_;
    QPointer<QQuickWindow> window_;
    std::unique_ptr<WaylandHdrCheck> observer_;
    QTimer poll_;
    QElapsedTimer drain_,checkpoint_;
    QJsonArray plan_,soak_;
    QList<QByteArray> frameTexts_;
    qint64 receiptBytes_=0;
    int cycles_=8;
    QJsonObject result_;
    QString expectedOutput_,expectedCode_,moveTarget_;
    quint64 oldRevision_=0;
    int step_=0,expectedCount_=2;
    bool started_=false,moving_=false,controlPending_=false,samplePending_=false,finished_=false;
};

class MultiwindowHdrCheck final : public QObject, public DiagnosticCheck {
  public:
    MultiwindowHdrCheck(QUrl image, Licasa::WindowManager* manager, quint32 seed, int actions)
        : image_(std::move(image)), manager_(manager), actions_(actions)
    {
        quint32 random = seed;
        const auto next = [&] { random = random * 1664525u + 1013904223u; return random >> 16; };
        for (const char* action : {"calibrate", "toggle", "toggle", "hide", "reopen"})
            for (int slot : {0, 1})
                plan_.append(QJsonObject{{"slot",slot},{"action",QLatin1String(action)},{"delay_ms",0}});
        while (plan_.size() < actions_) {
            const int slot = next() % 2;
            const auto choice = next() % 4;
            const char* action = choice == 0 ? "toggle" : choice == 1 ? "calibrate"
                : choice == 2 || plan_.size() % 10 != 0 ? "hide" : "reopen";
            plan_.append(QJsonObject{{"slot",slot},{"action",QLatin1String(action)},
                                     {"delay_ms",int(next() % 4)*25}});
        }
        result_ = {{"seed",qint64(seed)},{"actions",actions_},{"plan",plan_},{"platform",QGuiApplication::platformName()},
                   {"qt",QString::fromLatin1(qVersion())},{"fixture",image_.toLocalFile()},
                   {"fixture_sha256",fileSha256(image_.toLocalFile())},
                   {"build",Licasa::applicationIdentity().toJson()}};
    }

    bool attach(QQuickWindow* window) override
    {
        if (!attachSlot(0,window) || !createSlot(1)) return false;
        QTimer::singleShot(300000 * std::max(1, actions_ / 100),this,[this] { finish("timed_out",1); });
        samplePair();
        return true;
    }

  private:
    struct Slot {
        QPointer<QQuickWindow> window;
        std::unique_ptr<WaylandHdrCheck> observer;
        bool hdr = true, highWhite = false;
        float white = 0, peak = 0;
    };
    bool attachSlot(int slot, QQuickWindow* window)
    {
        auto& state = slots_[slot];
        state.window = window;
        state.white = slot == 0 ? (state.highWhite ? 300 : 200) : (state.highWhite ? 150 : 100);
        state.peak = state.white * (slot == 0 ? 4 : 2);
        state.observer = std::make_unique<WaylandHdrCheck>(image_,"hdr",true,QString{},
            [this,slot](QJsonObject frame) {
                QTimer::singleShot(0,this,[this,slot,frame] { received(slot,frame); });
            });
        if (!state.observer->attach(window)) return false;
        if (!QMetaObject::invokeMethod(window,"setFullScreenState",Q_ARG(QVariant,QVariant(false))))
            return false;
        if (Licasa::hdrMonitorSettings(window).value("canRemember").toBool()) return false;
        return apply(slot);
    }
    bool createSlot(int slot)
    {
        const auto before = QGuiApplication::topLevelWindows();
        if (!manager_->createWindow(image_,false,true)) return false;
        for (auto* window : QGuiApplication::topLevelWindows()) {
            auto* quick = qobject_cast<QQuickWindow*>(window);
            if (quick && !before.contains(window) && quick->property("windowSession").isValid())
                return attachSlot(slot,quick);
        }
        return false;
    }
    bool apply(int slot)
    {
        auto& state = slots_[slot];
        if (!state.window) return false;
        const auto revision = Licasa::hdrOutputRevision(state.window);
        return state.hdr ? Licasa::setHdrMonitorSettings(state.window,revision,state.white,state.peak,false)
                         : Licasa::useSdrForMonitor(state.window,revision);
    }
    void sample(int slot, const char* stage)
    {
        auto& state = slots_[slot];
        state.observer->sample(QStringLiteral("multi_%1_%2_%3").arg(step_).arg(slot).arg(QLatin1String(stage)),
                               state.hdr ? state.white : 0,state.hdr ? state.peak : 0);
    }
    void samplePair()
    {
        pending_ = 2;
        sample(0,"settled");
        sample(1,"settled");
    }
    void received(int slot, QJsonObject frame)
    {
        if (finished_) return;
        if (frame.contains("error")) { finish(frame["error"].toString(),1); return; }
        frame["slot"] = slot;
        frame["step"] = step_;
        // Kept as compact text, so the receipt's own growth is known exactly
        // when a long session's live allocations are measured.
        frameTexts_.append(QJsonDocument(frame).toJson(QJsonDocument::Compact));
        receiptBytes_ += frameTexts_.constLast().size();
        if (during_) {
            during_ = false;
            const auto action = plan_[step_-1].toObject();
            const int target = action["slot"].toInt();
            if (action["action"] == "reopen") {
                // The retiring native generation must release its target lease
                // before admitting the replacement window.
                QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
                if (!createSlot(target)) { finish("peer_reopen_failed",1); return; }
            } else if (action["action"] == "hide") {
                slots_[target].window->show();
                slots_[target].window->requestActivate();
            }
            samplePair();
            return;
        }
        if (--pending_ > 0) return;
        if (step_ % 50 == 0) measure();
        if (step_ == plan_.size()) { finish("passed",0); return; }
        const auto entry = plan_[step_].toObject();
        QTimer::singleShot(entry["delay_ms"].toInt(),this,[this,entry] { advance(entry); });
    }
    void advance(const QJsonObject& entry)
    {
        if (finished_) return;
        ++step_;
        const int target = entry["slot"].toInt();
        const auto action = entry["action"].toString();
        auto& state = slots_[target];
        qInfo("LICASA_MULTI_ACTION %d %d %s",step_,target,qPrintable(action));
        if (action == "toggle") {
            state.hdr = !state.hdr;
            if (!apply(target)) { finish("output_toggle_failed",1); return; }
        } else if (action == "calibrate") {
            state.hdr = true;
            state.highWhite = !state.highWhite;
            state.white = target == 0 ? (state.highWhite ? 300 : 200) : (state.highWhite ? 150 : 100);
            state.peak = state.white * (target == 0 ? 4 : 2);
            if (!apply(target)) { finish("calibration_failed",1); return; }
        } else if (action == "reopen") {
            auto* old = state.window.data();
            state.observer.reset();
            manager_->destroyWindow(old);
        } else {
            state.window->hide();
        }
        during_ = true;
        QTimer::singleShot(action == "hide" ? 150 : 0,this,[this,target] { sample(1-target,"during"); });
    }
    // Every 50 actions, with both windows settled: the resources a long
    // multiwindow session must retire, and each window's output state.
    void measure()
    {
        QJsonObject metrics = processMetrics();
        metrics.remove("mapped_optional_libraries");
        metrics.remove("mapped_qt_libraries");
        metrics["step"] = step_;
        metrics["descriptors"] = descriptorKinds(image_.toLocalFile());
        const auto used = Licasa::viewerResourceBudget()->used();
        metrics["pool"] = QJsonObject{{"decode",qint64(used.decode)},{"retained",qint64(used.retained)},
                                      {"upload",qint64(used.upload)},{"targets",qint64(used.targets)}};
        metrics["outputs"] = QJsonArray{slots_[0].hdr, slots_[1].hdr};
        metrics["windows"] = int(QGuiApplication::topLevelWindows().size());
        metrics["receipt_bytes"] = receiptBytes_;
        soak_.append(metrics);
    }
    void finish(const QString& status, int exitCode)
    {
        if (finished_) return;
        finished_ = true;
        result_["status"] = status;
        result_["completed_actions"] = step_;
        QJsonArray frames;
        for (const QByteArray& text : frameTexts_) frames.append(QJsonDocument::fromJson(text).object());
        result_["frames"] = frames;
        result_["soak"] = soak_;
        QJsonArray outputs;
        for (const auto& state : slots_) {
            outputs.append(state.window ? QJsonObject::fromVariantMap(Licasa::hdrMonitorSettings(state.window))
                                        : QJsonObject{});
        }
        result_["outputs_at_finish"] = outputs;
        for (auto& state : slots_) state.observer.reset();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        manager_->destroyAllWindows();
        drain_.start();
        auto* timer = new QTimer(this);
        connect(timer,&QTimer::timeout,this,[this,timer,exitCode] {
            const auto used = Licasa::viewerResourceBudget()->used();
            const bool drained = !used.decode && !used.retained && !used.upload && !used.targets;
            if (!drained && drain_.elapsed() < 5000) return;
            timer->stop();
            result_["drained"] = drained;
            result_["remaining_bytes"] = QJsonObject{{"decode",qint64(used.decode)},
                {"retained",qint64(used.retained)},{"upload",qint64(used.upload)},{"targets",qint64(used.targets)}};
            printJson(result_);
            QCoreApplication::exit(drained ? exitCode : 1);
        });
        timer->start(10);
    }

    QUrl image_;
    Licasa::WindowManager* manager_;
    std::array<Slot,2> slots_;
    QJsonArray plan_, soak_;
    QList<QByteArray> frameTexts_;
    qint64 receiptBytes_ = 0;
    QJsonObject result_;
    QElapsedTimer drain_;
    int actions_ = 100, step_ = 0, pending_ = 0;
    bool during_ = false, finished_ = false;
};

// Two production windows view one path while it is replaced by a different
// gain-map photo of the same size and modification time. A window that has
// not reloaded must keep the photo it presented, in HDR or in SDR.
class ReplacementHdrCheck final : public QObject, public DiagnosticCheck {
  public:
    ReplacementHdrCheck(const QUrl& gray, Licasa::WindowManager* manager, quint32 seed)
        : manager_(manager)
    {
        static const int barriers[] = {0, 1, 2, 3, 5, 8, 13, 21, 34, 55};
        quint32 random = seed;
        const auto next = [&] { random = random * 1664525u + 1013904223u; return random >> 16; };
        const auto append = [&](int slot, const char* action, int delay, int barrier) {
            plan_.append(QJsonObject{{"slot",slot},{"action",QLatin1String(action)},
                                     {"delay_ms",delay},{"barrier_ms",barrier}});
        };
        int fixed = 0;
        for (const char* action : {"replace-atomic","calibrate","replace-inplace","toggle","toggle",
                                   "replace-symlink","hide","reload","reopen","calibrate"})
            for (int slot : {0, 1}) append(slot, action, 0, barriers[fixed++ % 10]);
        // So far the first window always fell behind, holding the red photo.
        // One more replacement swaps the photos, so each is held in every mode.
        static const std::pair<int, const char*> mirrored[] = {
            {1, "replace-atomic"}, {0, "calibrate"}, {0, "toggle"}, {1, "toggle"}, {0, "toggle"}, {1, "toggle"}};
        for (const auto& [slot, action] : mirrored) append(slot, action, 0, barriers[fixed++ % 10]);
        static const char* names[] = {"replace-atomic","replace-inplace","replace-symlink","reload",
                                      "calibrate","toggle","hide","reopen"};
        while (plan_.size() < 100) {
            const int slot = next() % 2;
            const char* action = names[next() % 8];
            const int delay = int(next() % 4) * 25;
            append(slot, action, delay, barriers[next() % 10]);
        }
        const QString grayPath = gray.toLocalFile();
        const QString redPath = QFileInfo(grayPath).dir().filePath(QStringLiteral("jpeg-gainmap-p3-red.jpg"));
        QFile grayFile(grayPath), redFile(redPath);
        if (scratch_.isValid() && grayFile.open(QIODevice::ReadOnly) && redFile.open(QIODevice::ReadOnly)) {
            contents_[0] = grayFile.readAll();
            contents_[1] = redFile.readAll();
            const auto length = qMax(contents_[0].size(), contents_[1].size());
            for (auto& bytes : contents_) bytes.append(QByteArray(length - bytes.size(), '\0'));
        }
        // The name also exercises request escaping between QML and the provider.
        path_ = scratch_.filePath(QString::fromUtf8("photo #1 \xc3\xbc.jpg"));
        image_ = QUrl::fromLocalFile(path_);
        result_ = {{"seed",qint64(seed)},{"plan",plan_},{"platform",QGuiApplication::platformName()},
                   {"qt",QString::fromLatin1(qVersion())},{"fixture",path_},
                   {"source_fixtures",QJsonObject{{"gray",grayPath},{"gray_sha256",fileSha256(grayPath)},
                                                  {"red",redPath},{"red_sha256",fileSha256(redPath)}}},
                   {"build",Licasa::applicationIdentity().toJson()}};
        valid_ = !contents_[0].isEmpty() && contents_[0] != contents_[1] && write(path_, contents_[0])
            && recordGeneration(QStringLiteral("initial"));
    }

    QUrl image() const { return image_; }

    bool attach(QQuickWindow* window) override
    {
        if (!valid_ || !attachSlot(0, window) || !createSlot(1)) return false;
        connect(&poll_, &QTimer::timeout, this, [this] {
            if (finished_) return;
            if (checkpoint_.elapsed() > 20000) { finish(QStringLiteral("checkpoint_timed_out"), 1); return; }
            for (auto& state : slots_) if (state.window && state.window->isVisible()) state.window->update();
        });
        poll_.start(50);
        checkpoint_.start();
        QTimer::singleShot(300000, this, [this] { finish(QStringLiteral("timed_out"), 1); });
        samplePair();
        return true;
    }

  private:
    struct Slot {
        QPointer<QQuickWindow> window;
        std::unique_ptr<WaylandHdrCheck> observer;
        bool hdr = true, highWhite = false, hdrValid = true;
        float white = 0, peak = 0;
        int loaded = 0;
    };
    struct Generation {
        QString token, sha256, kind;
    };

    bool write(const QString& path, const QByteArray& bytes) const
    {
        QFile file(path);
        return file.open(QIODevice::ReadWrite) && file.write(bytes) == bytes.size() && file.flush()
            && file.setFileTime(modified_, QFileDevice::FileModificationTime);
    }
    bool recordGeneration(const QString& kind)
    {
        const auto identity = Licasa::externalFileIdentityAtPath(path_);
        const auto& bytes = contents_[generations_.size() % 2];
        if (!identity || qint64(identity->size) != bytes.size()
            || identity->modifiedSeconds != modified_.toSecsSinceEpoch() || identity->modifiedNanoseconds != 0)
            return false;
        const QString token = WaylandHdrCheck::identityToken(identity);
        for (const auto& previous : generations_) if (previous.token == token) return false;
        generations_.push_back({token, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), kind});
        QJsonArray list;
        for (int index = 0; index < int(generations_.size()); ++index)
            list.append(QJsonObject{{"generation",index},{"kind",generations_[index].kind},
                                    {"content",index % 2 ? "red" : "gray"},
                                    {"sha256",generations_[index].sha256},{"identity",generations_[index].token}});
        result_["generations"] = list;
        return true;
    }
    bool replaceFile(const QString& kind)
    {
        const auto& bytes = contents_[generations_.size() % 2];
        const QByteArray target = QFile::encodeName(path_);
        if (kind == "replace-atomic") {
            const QString next = scratch_.filePath(QStringLiteral("atomic.next"));
            if (!write(next, bytes) || std::rename(QFile::encodeName(next).constData(), target.constData()) != 0)
                return false;
        } else if (kind == "replace-inplace") {
            // Same inode (through a link, when one is in place) and same length.
            if (!write(path_, bytes)) return false;
        } else {
            const QString store = scratch_.filePath(QStringLiteral("linked-%1.jpg").arg(generations_.size()));
            const QByteArray link = QFile::encodeName(scratch_.filePath(QStringLiteral("link.next")));
            if (!write(store, bytes) || ::symlink(QFile::encodeName(store).constData(), link.constData()) != 0
                || std::rename(link.constData(), target.constData()) != 0)
                return false;
        }
        return recordGeneration(kind);
    }
    int generation() const { return int(generations_.size()) - 1; }

    bool attachSlot(int slot, QQuickWindow* window)
    {
        auto& state = slots_[slot];
        state.window = window;
        state.white = slot == 0 ? (state.highWhite ? 300 : 200) : (state.highWhite ? 150 : 100);
        state.peak = state.white * (slot == 0 ? 4 : 2);
        state.loaded = generation();
        state.hdrValid = state.hdr;
        state.observer = std::make_unique<WaylandHdrCheck>(image_, "hdr", true, QString{},
            [this, slot](QJsonObject frame) {
                QTimer::singleShot(0, this, [this, slot, frame] { received(slot, frame); });
            });
        if (!state.observer->attach(window)) return false;
        if (!QMetaObject::invokeMethod(window, "setFullScreenState", Q_ARG(QVariant, QVariant(false))))
            return false;
        if (Licasa::hdrMonitorSettings(window).value("canRemember").toBool()) return false;
        return apply(slot);
    }
    bool createSlot(int slot)
    {
        const auto before = QGuiApplication::topLevelWindows();
        if (!manager_->createWindow(image_, false, true)) return false;
        for (auto* window : QGuiApplication::topLevelWindows()) {
            auto* quick = qobject_cast<QQuickWindow*>(window);
            if (quick && !before.contains(window) && quick->property("windowSession").isValid())
                return attachSlot(slot, quick);
        }
        return false;
    }
    bool apply(int slot)
    {
        auto& state = slots_[slot];
        if (!state.window) return false;
        const auto revision = Licasa::hdrOutputRevision(state.window);
        return state.hdr ? Licasa::setHdrMonitorSettings(state.window, revision, state.white, state.peak, false)
                         : Licasa::useSdrForMonitor(state.window, revision);
    }
    bool reload(int slot)
    {
        auto& state = slots_[slot];
        if (!state.window || !QMetaObject::invokeMethod(state.window, "loadImageUrl", Qt::DirectConnection,
                Q_ARG(QVariant, QVariant(image_)), Q_ARG(QVariant, QVariant(true))))
            return false;
        state.loaded = generation();
        state.hdrValid = state.hdr;
        return true;
    }
    // Where the interrupted revision was when its file or window went away.
    void observe(int slot, const QJsonObject& entry)
    {
        const auto& state = slots_[slot];
        const auto* viewport = state.window ? state.window->findChild<QQuickItem*>(QStringLiteral("imageViewport")) : nullptr;
        const auto* loader = viewport ? viewport->findChild<QQuickItem*>(QStringLiteral("hdrStillImage")) : nullptr;
        const auto* photo = loader ? qobject_cast<Licasa::HdrImageItem*>(loader->property("item").value<QObject*>()) : nullptr;
        const auto used = Licasa::viewerResourceBudget()->used();
        const char* stage = !viewport ? "no_viewport"
            : viewport->property("decodePending").toBool() || viewport->property("imageStatus").toInt() != 1 ? "sdr_decode"
            : !state.hdr ? "sdr_output"
            : !photo || photo->photoStatus() == Licasa::HdrImageItem::PhotoStatus::Sdr
                || photo->photoStatus() == Licasa::HdrImageItem::PhotoStatus::None ? "hdr_not_requested"
            : photo->photoStatus() == Licasa::HdrImageItem::PhotoStatus::Preparing
                ? (used.decode ? "hdr_decode" : "hdr_upload")
            : photo->photoStatus() == Licasa::HdrImageItem::PhotoStatus::Hdr ? "hdr_presented" : "hdr_unavailable";
        observed_.append(QJsonObject{{"step",step_},{"slot",slot},{"action",entry["action"]},
            {"barrier_ms",entry["barrier_ms"]},{"stage",QLatin1String(stage)},
            {"decode_bytes",qint64(used.decode)},{"retained_bytes",qint64(used.retained)},
            {"upload_bytes",qint64(used.upload)}});
        result_["interrupted"] = observed_;
    }
    void sample(int slot, const char* stage)
    {
        auto& state = slots_[slot];
        state.observer->expectPresented(generations_[state.loaded].token);
        state.observer->sample(QStringLiteral("replace_%1_%2_%3").arg(step_).arg(slot).arg(QLatin1String(stage)),
                               state.hdr ? state.white : 0, state.hdr ? state.peak : 0,
                               QStringLiteral("disabled"), state.hdrValid);
    }
    void samplePair()
    {
        pending_ = 2;
        sample(0, "settled");
        sample(1, "settled");
    }
    void received(int slot, QJsonObject frame)
    {
        if (finished_) return;
        if (frame.contains("error")) { finish(frame["error"].toString(), 1); return; }
        checkpoint_.restart();
        const auto& state = slots_[slot];
        frame["slot"] = slot;
        frame["step"] = step_;
        frame["source_generation"] = state.loaded;
        frame["fixture_sha256"] = generations_[state.loaded].sha256;
        frame["file_generation"] = generation();
        frames_.append(frame);
        result_["frames"] = frames_;
        if (during_) {
            during_ = false;
            const auto action = plan_[step_-1].toObject();
            const int target = action["slot"].toInt();
            if (action["action"] == "reopen") {
                // The retiring native generation must release its target lease
                // before admitting the replacement window.
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                if (!createSlot(target)) { finish(QStringLiteral("peer_reopen_failed"), 1); return; }
            } else if (action["action"] == "hide") {
                auto& hidden = slots_[target];
                hidden.hdrValid = hidden.hdr && hidden.loaded == generation();
                hidden.window->show();
                hidden.window->requestActivate();
            }
            samplePair();
            return;
        }
        if (--pending_ > 0) return;
        if (step_ == plan_.size()) { finish(QStringLiteral("passed"), 0); return; }
        const auto entry = plan_[step_].toObject();
        QTimer::singleShot(entry["delay_ms"].toInt(), this, [this, entry] { advance(entry); });
    }
    void advance(const QJsonObject& entry)
    {
        if (finished_) return;
        ++step_;
        checkpoint_.restart();
        const int target = entry["slot"].toInt();
        const auto action = entry["action"].toString();
        auto& state = slots_[target];
        qInfo("LICASA_REPLACE_ACTION %d %d %s", step_, target, qPrintable(action));
        const auto observePeer = [this, target](int delay) {
            during_ = true;
            QTimer::singleShot(delay, this, [this, target] { if (!finished_) sample(1 - target, "during"); });
        };
        if (action.startsWith("replace-") || action == "reopen") {
            // Start a revision, then take its file or window away at a seeded moment.
            if (!reload(target)) { finish(QStringLiteral("reload_failed"), 1); return; }
            QTimer::singleShot(entry["barrier_ms"].toInt(), this, [this, entry, target, action, observePeer] {
                if (finished_) return;
                observe(target, entry);
                auto& state = slots_[target];
                if (action == "reopen") {
                    auto* old = state.window.data();
                    state.observer.reset();
                    manager_->destroyWindow(old);
                } else if (!replaceFile(action) || !reload(target)) {
                    finish(QStringLiteral("replacement_failed"), 1);
                    return;
                }
                observePeer(0);
            });
            return;
        }
        if (action == "reload") {
            if (!reload(target)) { finish(QStringLiteral("reload_failed"), 1); return; }
        } else if (action == "toggle") {
            state.hdr = !state.hdr;
            state.hdrValid = state.hdr && state.loaded == generation();
            if (!apply(target)) { finish(QStringLiteral("output_toggle_failed"), 1); return; }
        } else if (action == "calibrate") {
            state.hdr = true;
            state.highWhite = !state.highWhite;
            state.white = target == 0 ? (state.highWhite ? 300 : 200) : (state.highWhite ? 150 : 100);
            state.peak = state.white * (target == 0 ? 4 : 2);
            state.hdrValid = state.loaded == generation();
            if (!apply(target)) { finish(QStringLiteral("calibration_failed"), 1); return; }
        } else {
            state.window->hide();
        }
        observePeer(action == "hide" ? 150 : 0);
    }
    void finish(const QString& status, int exitCode)
    {
        if (finished_) return;
        finished_ = true;
        poll_.stop();
        result_["status"] = status;
        result_["completed_actions"] = step_;
        QJsonArray outputs, last;
        for (const auto& state : slots_) {
            outputs.append(state.window ? QJsonObject::fromVariantMap(Licasa::hdrMonitorSettings(state.window))
                                        : QJsonObject{});
            auto frame = state.observer ? state.observer->lastFrame() : QJsonObject{};
            frame["expected_generation"] = state.loaded;
            frame["expected_hdr"] = state.hdr;
            frame["expected_hdr_photo"] = state.hdrValid;
            if (const auto* viewport = state.window ? state.window->findChild<QQuickItem*>(QStringLiteral("imageViewport")) : nullptr) {
                const auto* loader = viewport->findChild<QQuickItem*>(QStringLiteral("hdrStillImage"));
                frame["request_key"] = viewport->property("activeRequestKey").toString();
                frame["provider_source"] = state.window->property("renderedProviderSource").toString();
                frame["decode_pending"] = viewport->property("decodePending").toBool();
                frame["hdr_loader_active"] = loader && loader->property("active").toBool();
                frame["composition_ready"] = state.window->property("hdrCompositionReady").toBool();
            }
            last.append(frame);
        }
        result_["outputs_at_finish"] = outputs;
        result_["last_frames"] = last;
        for (auto& state : slots_) state.observer.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        manager_->destroyAllWindows();
        drain_.start();
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer, exitCode] {
            const auto used = Licasa::viewerResourceBudget()->used();
            const bool drained = !used.decode && !used.retained && !used.upload && !used.targets;
            if (!drained && drain_.elapsed() < 5000) return;
            timer->stop();
            result_["drained"] = drained;
            result_["remaining_bytes"] = QJsonObject{{"decode",qint64(used.decode)},
                {"retained",qint64(used.retained)},{"upload",qint64(used.upload)},{"targets",qint64(used.targets)}};
            printJson(result_);
            QCoreApplication::exit(drained ? exitCode : 1);
        });
        timer->start(10);
    }

    Licasa::WindowManager* manager_;
    QTemporaryDir scratch_;
    QString path_;
    QUrl image_;
    std::array<QByteArray, 2> contents_;
    const QDateTime modified_ = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
    std::vector<Generation> generations_;
    std::array<Slot, 2> slots_;
    QJsonArray plan_, frames_, observed_;
    QJsonObject result_;
    QTimer poll_;
    QElapsedTimer drain_, checkpoint_;
    int step_ = 0, pending_ = 0;
    bool valid_ = false, during_ = false, finished_ = false;
};
#endif

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
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
        result_["hdr_output_status"]=QJsonObject::fromVariantMap(Licasa::hdrMonitorSettings(window_));
#endif
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

#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
class WaylandHdrSampler final : public HdrFrameSampler {
  public:
    WaylandHdrSampler(const QUrl& image, std::function<void(QJsonObject)> frame, bool matchPresented)
        : check_(image, QStringLiteral("hdr"), true, QString{}, std::move(frame))
    {
        check_.recordMotionState();
        if (matchPresented) {
            check_.expectPresented(WaylandHdrCheck::identityToken(
                Licasa::externalFileIdentityAtPath(image.toLocalFile())));
        }
    }
    bool attach(QQuickWindow* window) override { return check_.attach(window); }
    void sample(const QString& phase, float white, float peak, Content content, const QString& sdrCode) override
    {
        check_.sample(phase, white, peak, sdrCode,
                      content == Content::HdrStill, content == Content::MotionFrame,
                      content == Content::PendingSdrBaseOnHdr);
    }

  private:
    WaylandHdrCheck check_;
};
#endif

} // namespace

#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
std::unique_ptr<HdrFrameSampler> makeHdrFrameSampler(const QUrl& image,
                                                     std::function<void(QJsonObject)> frame,
                                                     bool matchPresented)
{
    return std::make_unique<WaylandHdrSampler>(image, std::move(frame), matchPresented);
}

std::unique_ptr<DiagnosticCheck> makeOutputMigrationCheck(const QUrl& image, Licasa::WindowManager* manager, quint32 seed,
                                                          int cycles)
{
    return std::make_unique<OutputMigrationCheck>(image,manager,seed,cycles);
}

std::unique_ptr<DiagnosticCheck> makeMultiwindowHdrCheck(const QUrl& image, Licasa::WindowManager* manager, quint32 seed,
                                                         int actions)
{
    return std::make_unique<MultiwindowHdrCheck>(image,manager,seed,actions);
}

std::unique_ptr<DiagnosticCheck> makeReplacementHdrCheck(const QUrl& gray, Licasa::WindowManager* manager,
                                                         quint32 seed, QUrl* opened)
{
    auto check = std::make_unique<ReplacementHdrCheck>(gray, manager, seed);
    *opened = check->image();
    return check;
}

std::unique_ptr<DiagnosticCheck>
makeWaylandHdrCheck(const QUrl& imageUrl, const QString& expectedOutput, bool windowChurn,
                    const QString& windowSeed)
{
    return std::make_unique<WaylandHdrCheck>(imageUrl, expectedOutput, windowChurn, windowSeed);
}
#endif

std::unique_ptr<DiagnosticCheck> makeViewerMeasurement(qint64 started, const QUrl& imageUrl,
                                                       int cycles)
{
    return std::make_unique<ViewerMeasurement>(started, imageUrl, cycles);
}

} // namespace LicasaDiagnostics

#include "diagnostics_viewer_measurement.moc"
