#include "../../src/imaging/hdr/render_target_budget.h"
#include "../../src/imaging/hdr/adapter_budget.h"
#include "surface_format.h"
#include "../../src/imaging/hdr/wayland_hdr.h"
#include <functional>

#ifdef LICASA_HDR_PROBE_GAINMAP
#include "gainmap_photo.h"
#include "native_retirement.h"
#include "../gainmap-lab/async_decoder.h"
#include <QEventLoop>
#include <QImageReader>
#endif
#ifdef LICASA_HDR_PROBE_GAINMAP
#include "photo_readback.h"
#endif

#include <QColor>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickGraphicsConfiguration>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QVersionNumber>
#include <QColorSpace>
#include <QVulkanInstance>
#include <QtCore/qfloat16.h>

#include <rhi/qrhi.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

namespace {
constexpr int patchSize = 64;
constexpr QSize patternSize(4 * patchSize, patchSize);
constexpr float backing = 0.25f;
constexpr int unavailable = 77;
constexpr std::array<float, 4> hdrLevels{0.18f, 1.0f, 2.0f, 2.0f};
constexpr std::array<float, 4> sdrLevels{0.18f, 0.75f, 1.0f, 1.0f};
constexpr std::array<float, 4> alphas{1.0f, 1.0f, 1.0f, 0.5f};

int report(const char* mode, const char* status, int exitCode, QJsonObject result = {})
{
    result["mode"] = QLatin1String(mode);
    result["status"] = QLatin1String(status);
    result["qt"] = QString::fromLatin1(qVersion());
    result["platform"] = QGuiApplication::platformName();
    result["physical_hdr_verified"] = false;
    const QByteArray output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fprintf(stdout, "%s\n", output.constData());
    std::fflush(stdout);
    return exitCode;
}

QImage pattern(bool hdr)
{
    QImage image(patternSize, hdr ? QImage::Format_RGBA16FPx4_Premultiplied
                                  : QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    const auto& levels = hdr ? hdrLevels : sdrLevels;
    for (int y = 0; y < image.height(); ++y) {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const int patch = x / patchSize;
            const float alpha = alphas[patch];
            const float premultiplied = levels[patch] * alpha;
            if (hdr) {
                const qfloat16 rgb(premultiplied);
                const qfloat16 rgba[]{rgb, rgb, rgb, qfloat16(alpha)};
                std::memcpy(row + x * sizeof(rgba), rgba, sizeof(rgba));
            } else {
                const uchar rgb = uchar(std::lround(premultiplied * 255));
                const uchar rgba[]{rgb, rgb, rgb, uchar(std::lround(alpha * 255))};
                std::memcpy(row + x * sizeof(rgba), rgba, sizeof(rgba));
            }
        }
    }
    return image;
}

bool waylandSession()
{
    const QString platform = QGuiApplication::platformName();
    return platform == QStringLiteral("wayland") || platform.startsWith(QStringLiteral("wayland-"));
}

bool hasLinearHdrSurface(QQuickWindow& window, bool appTagged = false)
{
    const auto format = nativeSwapchainFormat(window.swapChain());
    if (!format || !window.rhi()->isTextureFormatSupported(QRhiTexture::RGBA16F)) return false;
    const auto surface = classifySurfaceFormat(*format);
    return surface == SurfaceFormat::LinearFp16 || (appTagged && surface == SurfaceFormat::PassThrough);
}

QJsonObject surfaceDetails(QQuickWindow& window, std::optional<VkSurfaceFormatKHR> format,
                           QJsonObject details = {})
{
    details["headroom_verified"] = false;
    if (const auto* screen = window.screen()) {
        details["screen"] = screen->name();
    }
    if (const auto* rhi = window.rhi()) {
        details["device"] = QString::fromUtf8(rhi->driverInfo().deviceName);
    }
    if (format) {
        details["vk_format"] = int(format->format);
        details["vk_color_space"] = int(format->colorSpace);
    }
    return details;
}

class ProbeImageItem final : public QQuickItem {
  public:
    explicit ProbeImageItem(QQuickWindow& window, QImage fp16Image = {})
        : QQuickItem(window.contentItem()), fp16Image_(std::move(fp16Image))
    {
        setFlag(ItemHasContents);
        setSize(fp16Image_.isNull() ? patternSize : fp16Image_.size());
        connect(&window, &QQuickWindow::sceneGraphInvalidated, this,
                [this] { texture_ = nullptr; });
    }

    void setHdrGate(std::function<bool()> gate) { hdrGate_=std::move(gate); update(); }
    bool uploadedFp16() const
    {
        const auto* rhiTexture = texture_ ? texture_->rhiTexture() : nullptr;
        return rhiTexture && rhiTexture->format() == QRhiTexture::RGBA16F &&
               rhiTexture->pixelSize() == size().toSize();
    }

  private:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override
    {
        const bool hdr = !fp16Image_.isNull() || (hdrGate_ && hdrGate_());
        if (oldNode && hdr == hdr_) {
            return oldNode;
        }
        texture_ = nullptr;
        delete oldNode;
        const QImage image = fp16Image_.isNull() ? pattern(hdr) : fp16Image_;
        if (image.isNull()) {
            return nullptr;
        }
        std::unique_ptr<QSGTexture> texture(
            window()->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel));
        if (!texture) {
            return nullptr;
        }
        auto* node = window()->createImageNode();
        if (!node) {
            return nullptr;
        }
        texture_ = texture.release();
        node->setTexture(texture_);
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest);
        node->setRect(boundingRect());
        hdr_ = hdr;
        return node;
    }

    std::function<bool()> hdrGate_;
    QImage fp16Image_;
    bool hdr_ = false;
    QSGTexture* texture_ = nullptr; // Owned by the scene graph node.
};

void configureWindow(QQuickWindow& window, QSize size = patternSize)
{
    window.resize(size);
    window.setColor(QColor::fromRgbF(backing, backing, backing));
}

bool createVulkanInstance(QVulkanInstance& instance)
{
    instance.setApiVersion(QVersionNumber(1, 1));
    instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
    return instance.create();
}

int checkPixels(const char* mode, const QImage& frame, bool hdr, QJsonObject details = {})
{
    if (frame.size() != patternSize) {
        return report(mode, "readback_failed", 1, details);
    }

    const auto& levels = hdr ? hdrLevels : sdrLevels;
    const float tolerance = hdr ? 0.003f : 0.01f;
    QJsonArray patches;
    bool matched = true;
    for (int index = 0; index < int(levels.size()); ++index) {
        const int x = index * patchSize + patchSize / 2;
        const QColor pixel = frame.pixelColor(x, patchSize / 2);
        const float expected = levels[index] * alphas[index] + backing * (1 - alphas[index]);
        patches.append(QJsonObject{
            {"expected", expected},
            {"rgba", QJsonArray{pixel.redF(), pixel.greenF(), pixel.blueF(), pixel.alphaF()}}});
        matched &= std::abs(pixel.redF() - expected) <= tolerance &&
                   std::abs(pixel.greenF() - expected) <= tolerance &&
                   std::abs(pixel.blueF() - expected) <= tolerance &&
                   std::abs(pixel.alphaF() - 1) <= tolerance;
    }
    details["patches"] = patches;
    return report(mode, matched ? "passed" : "unexpected_pixels", matched ? 0 : 1, details);
}

int checkSoftware()
{
    QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
    QQuickRenderControl control;
    QQuickWindow window(&control);
    configureWindow(window);
    new ProbeImageItem(window);
    if (hasLinearHdrSurface(window)) {
        return report("software", "unexpected_hdr_surface", 1);
    }
    return checkPixels("software", window.grabWindow(), false);
}

template <typename Verify>
int readbackVulkan(const QImage& input, const char* mode, QJsonObject details, Verify verify,
                   std::shared_ptr<Licasa::GainmapLab::ResourceBudget> budget = Licasa::viewerResourceBudget(),
                   Licasa::GainmapLab::ResourceBudget::Lease sourceReservation = {})
{
    if (input.isNull()) {
        return report(mode, "image_allocation_failed", 1, details);
    }
    const QSize size = input.size();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QVulkanInstance instance;
    if (!createVulkanInstance(instance)) {
        return report(mode, "vulkan_unavailable", unavailable, details);
    }

    Licasa::GainmapLab::ResourceBudget::Lease targetLease;
    QQuickRenderControl control;
    QQuickWindow window(&control);
    configureWindow(window, size);
    window.setVulkanInstance(&instance);
    if (!control.initialize()) {
        return report(mode, "scene_graph_unavailable", unavailable, details);
    }
    QRhi* rhi = control.rhi();
    if (!rhi || !rhi->isTextureFormatSupported(QRhiTexture::RGBA16F)) {
        return report(mode, "fp16_unavailable", unavailable, details);
    }

    {
        const std::unique_ptr<QRhiSwapChain> request(rhi->newSwapChain());
        request->setFormat(QRhiSwapChain::HDRExtendedSrgbLinear);
        if (nativeSwapchainFormat(request.get())) {
            return report(mode, "uncreated_swapchain_reported_native_format", 1, details);
        }
    }

    const auto adapter = Licasa::describeHdrAdapter(rhi);
    if (!adapter) return report(mode, "adapter_budget_unavailable", 1, details);
    budget->setAdapterLimit(adapter->key, adapter->limit);
    if (sourceReservation) {
        if (!sourceReservation->bindAdapter(adapter->key)) return report(mode, "adapter_upload_rejected", 1, details);
    } else sourceReservation = budget->acquire({0, 0, quint64(input.sizeInBytes())}, adapter->key);
    if (!sourceReservation) return report(mode, "adapter_upload_rejected", 1, details);
    details["adapter_uuid"] = QString::fromLatin1(adapter->key.toHex());
    details["adapter_gpu_limit_bytes"] = qint64(adapter->limit);
    const auto targetBytes = Licasa::fp16TargetBytes(size);
    targetLease = targetBytes ? budget->acquire({0, 0, 0, *targetBytes}, adapter->key) : nullptr;
    if (!targetLease) return report(mode, "render_target_budget_rejected", 1, details);
    details["render_target_reserved_bytes"] = qint64(*targetBytes);
    const auto usage = budget->used();
    details["gpu_reserved_bytes"] = qint64(usage.upload + usage.targets);
    std::unique_ptr<QRhiTexture> texture(
        rhi->newTexture(QRhiTexture::RGBA16F, size, 1,
                        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        return report(mode, "texture_creation_failed", 1, details);
    }
    std::unique_ptr<QRhiTextureRenderTarget> target(
        rhi->newTextureRenderTarget({QRhiColorAttachment(texture.get())}));
    std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    if (!target->create()) {
        return report(mode, "target_creation_failed", 1, details);
    }
    window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));
    const auto cleanup = qScopeGuard([&] {
        control.invalidate();
        window.setRenderTarget(QQuickRenderTarget());
        target.reset();
    });
    const auto* item = new ProbeImageItem(window, input);

    QRhiReadbackResult result;
    control.polishItems();
    control.beginFrame();
    QRhiCommandBuffer* commands = control.commandBuffer();
    if (!commands) {
        return report(mode, "frame_creation_failed", 1, details);
    }
    control.sync();
    control.render();
    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    updates->readBackTexture(QRhiReadbackDescription(texture.get()), &result);
    commands->resourceUpdate(updates);
    control.endFrame();

    const qsizetype rowBytes = qsizetype(size.width()) * 4 * sizeof(qfloat16);
    if (!item->uploadedFp16()) {
        return report(mode, "unexpected_input_texture_format", 1, details);
    }
    if (result.format != QRhiTexture::RGBA16F || result.pixelSize != size ||
        result.data.size() != rowBytes * size.height()) {
        return report(mode, "unexpected_target_format", 1, details);
    }
    const QImage frame(reinterpret_cast<const uchar*>(result.data.constData()), size.width(),
                       size.height(), rowBytes, QImage::Format_RGBA16FPx4_Premultiplied);
    details["device"] = QString::fromUtf8(rhi->driverInfo().deviceName);
    details["upload_rgba16f_bytes"] = input.sizeInBytes();
    details["target_rgba16f_bytes"] = rowBytes * size.height();
    details["readback_bytes"] = result.data.size();
    return verify(frame, std::move(details));
}

#ifdef LICASA_HDR_PROBE_GAINMAP
int readbackPhoto(const char* mode, const QImage& upload, const QStringList& coordinates,
                  QJsonObject details,
                  std::shared_ptr<Licasa::GainmapLab::ResourceBudget> budget = Licasa::viewerResourceBudget(),
                   Licasa::GainmapLab::ResourceBudget::Lease sourceReservation = {})
{
    const auto points = PhotoReadback::samplePoints(upload.size(), coordinates);
    if (!points) {
        return report(mode, "invalid_sample", 1);
    }
    details["width"] = upload.width();
    details["height"] = upload.height();
    return readbackVulkan(
        upload, mode, std::move(details), [&](const QImage& frame, QJsonObject result) {
            const bool passed = PhotoReadback::verify(frame, upload, backing, *points, result);
            return report(mode, passed ? "passed" : "unexpected_pixels", passed ? 0 : 1, result);
        }, std::move(budget), std::move(sourceReservation));
}
#endif


#ifdef LICASA_HDR_PROBE_GAINMAP
// Exercise the actual QRhi texture and a queued upload after its decoder owner
// has closed. The lease stays with the texture until an explicit GPU finish.
int checkGainmapLease(const QString& path, bool fenceMode = false)
{
    namespace Lab = Licasa::GainmapLab;
    const auto mode = fenceMode ? "gainmap_fence_retirement" : "gainmap_gpu_lease";
    const auto identity = Licasa::externalFileIdentityAtPath(path);
    const QSize size = QImageReader(path).size();
    if (!identity || !size.isValid() || quint64(size.width()) * size.height() > 4 * 1024 * 1024)
        return report(mode, "invalid_source", 1);
    const quint64 bytes = quint64(size.width()) * size.height() * 8;
    auto pool = std::make_shared<Lab::ResourceBudget>(Lab::ResourceBudget::Bytes{
        320 * 1024 * 1024, 64 * 1024 * 1024, bytes * (fenceMode ? 4 : 1)});
    auto load = [&](Lab::AsyncDecoder& worker) {
        QEventLoop loop;
        bool accepted = false;
        const auto connection = QObject::connect(&worker, &Lab::AsyncDecoder::settled, &loop,
            [&](quint64, bool ok, const QString&) { accepted = ok; loop.quit(); });
        worker.submit(path, *identity, 4);
        loop.exec();
        QObject::disconnect(connection);
        return accepted;
    };
    std::shared_ptr<Lab::RequestCoordinator::Upload> transfer;
    {
        Lab::AsyncDecoder first({}, {}, nullptr, pool);
        if (!load(first)) return report(mode, "decode_failed", 1);
        if (!first.withUpload([&](QImage& image) {
            QString primaries, error;
            if (!GainmapPhotoProbe::prepareUpload(image, primaries, error)) return false;
            transfer = first.uploadLease();
            return true;
        })) return report(mode, "upload_admission_failed", 1);
    } // The pending transfer retains its source and allowances past owner close.
    if (pool->used().decode || pool->used().retained != bytes || pool->used().upload != bytes)
        return report(mode, "closed_owner_released_budget", 1);

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QVulkanInstance instance;
    if (!createVulkanInstance(instance)) return report(mode, "vulkan_unavailable", unavailable);
    QQuickRenderControl control;
    QQuickWindow window(&control);
    const QSize textureSize = transfer->raster->image.size();
    configureWindow(window, textureSize);
    window.setVulkanInstance(&instance);
    if (!control.initialize()) return report(mode, "scene_graph_unavailable", unavailable);
    const auto cleanup = qScopeGuard([&] { control.invalidate(); });
    QRhi* rhi = control.rhi();
    if (!rhi || !rhi->isTextureFormatSupported(QRhiTexture::RGBA16F))
        return report(mode, "fp16_unavailable", unavailable);
    auto texture = std::make_shared<Lab::LeasedResource<QRhiTexture>>(transfer, std::unique_ptr<QRhiTexture>(
        rhi->newTexture(QRhiTexture::RGBA16F, textureSize, 1, QRhiTexture::UsedAsTransferSource)));
    if (!texture->get()->create()) return report(mode, "texture_creation_failed", 1);
    QRhiCommandBuffer* commands = nullptr;
    if (rhi->beginOffscreenFrame(&commands) != QRhi::FrameOpSuccess)
        return report(mode, "frame_creation_failed", 1);
    QRhiReadbackResult readback;
    auto* updates = rhi->nextResourceUpdateBatch();
    updates->uploadTexture(texture->get(), transfer->raster->image);
    updates->readBackTexture(QRhiReadbackDescription(texture->get()), &readback);
    commands->resourceUpdate(updates);
    rhi->endOffscreenFrame();
    if (rhi->finish() != QRhi::FrameOpSuccess) return report(mode, "gpu_finish_failed", 1);
    if (readback.data.size() != qsizetype(bytes) ||
        std::memcmp(readback.data.constData(), transfer->raster->image.constBits(), size_t(bytes)))
        return report(mode, "unexpected_pixels", 1);
    if (fenceMode) {
        const QImage& source = transfer->raster->image;
        transfer.reset();
        std::shared_ptr<void> persistent = std::move(texture);
        QJsonObject details;
        const bool passed = checkNativeRetirement(rhi, source, persistent, pool, details);
        if (passed && (pool->used().decode || pool->used().retained || pool->used().upload))
            return report(mode, "budget_not_drained", 1);
        return report(mode, passed ? "passed" : "retirement_failed", passed ? 0 : 1, details);
    }
    transfer.reset(); // The persistent texture now holds the last upload lease.

    Lab::AsyncDecoder second({}, {}, nullptr, pool);
    if (!load(second)) return report(mode, "second_decode_failed", 1);
    if (second.withUpload([](QImage&) { return true; }))
        return report(mode, "gpu_budget_overcommitted", 1);
    texture.reset(); // GPU work is finished; destroy the texture before its lease.
    if (pool->used().upload || pool->used().retained != bytes)
        return report(mode, "texture_budget_not_released", 1);
    if (!second.withUpload([](QImage&) { return true; }))
        return report(mode, "gpu_budget_not_reusable", 1);
    second.cancel();
    if (pool->used().decode || pool->used().retained || pool->used().upload)
        return report(mode, "budget_not_drained", 1);
    return report(mode, "passed", 0, {{"gpu_lease_bytes", qint64(bytes)},
                                      {"pixels_checked", qint64(bytes / 8)},
                                      {"owner_closed_before_transfer", true},
                                      {"blocked_competing_upload", true},
                                      {"all_budgets_released", true}});
}

int readbackGainmap(const QCommandLineParser& parser)
{
    constexpr auto mode = "gainmap_fp16_readback";
    bool validHeadroom;
    const float headroom = parser.value("gainmap-headroom").toFloat(&validHeadroom);
    if (!validHeadroom || !std::isfinite(headroom)) {
        return report(mode, "invalid_headroom", 1);
    }
    const bool fullResolution = parser.isSet("gainmap-full-resolution");
    namespace Lab = Licasa::GainmapLab;
    Lab::Limits limits;
    Lab::RequestCoordinator::Budgets budgets;
    if (fullResolution) {
        limits.rasterPixels = 16 * 1024 * 1024;
        budgets = {quint64(1088) * 1024 * 1024, quint64(128) * 1024 * 1024,
                   quint64(128) * 1024 * 1024};
    }
    const QString path = parser.value("readback-gainmap");
    const auto identity = Licasa::externalFileIdentityAtPath(path);
    if (!identity) return report(mode, "gainmap_decode_failed", 1);
    auto pool = std::make_shared<Lab::ResourceBudget>(Lab::ResourceBudget::Bytes{
        budgets.decode, budgets.retained, budgets.upload, 960ull * 1024 * 1024}, 1024ull * 1024 * 1024);
    Lab::AsyncDecoder worker(budgets, {}, nullptr, pool);
    QEventLoop loop;
    bool accepted = false;
    QString error;
    QObject::connect(&worker, &Lab::AsyncDecoder::settled, &loop,
                     [&](quint64, bool ok, const QString& reason) {
                         accepted = ok;
                         error = reason;
                         loop.quit();
                     });
    worker.submit(path, *identity, headroom, limits);
    loop.exec();
    if (!accepted) return report(mode, "gainmap_decode_failed", 1, {{"reason", error}});
    int result = 1;
    const bool uploaded = worker.withUpload([&](QImage& image) {
        const auto rasterBytes = image.sizeInBytes();
        QString primaries;
        if (!GainmapPhotoProbe::prepareUpload(image, primaries, error)) return false;
        QJsonObject details{{"headroom", headroom},
                            {"source_primaries", primaries},
                            {"gain_width", worker.gainSize().width()},
                            {"gain_height", worker.gainSize().height()},
                            {"jpeg_bytes", worker.jpegBytes()},
                            {"full_resolution_allowance", fullResolution},
                            {"async_decode", true},
                            {"decode_reservation_after_completion", qint64(worker.coordinator().decodeBytes())},
                            {"retained_raster_bytes", qint64(worker.coordinator().retainedBytes())},
                            {"upload_reserved_bytes", qint64(worker.coordinator().uploadBytes())},
                            {"decoded_rgba16f_bytes", rasterBytes},
                            {"conversion_raster_bytes", rasterBytes},
                            {"reused_decode_raster", true}};
        result = readbackPhoto(mode, image, parser.values("sample"), std::move(details), pool, worker.uploadLease()->lease);
        return true;
    });
    // The decode loop has already stopped. Drain deferred renderer/watcher
    // destruction after synchronous GPU teardown, before process shutdown.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (!uploaded) return report(mode, "gainmap_decode_failed", 1, {{"reason", error}});
    return result;
}
#endif

QJsonObject waylandDetails(const Licasa::WaylandHdrReport& info)
{
    return {{"color_manager",info.protocol},{"description_accepted",info.descriptionAccepted},{"surface_attached",info.surfacePresent},
            {"preferred_identity",qint64(info.preferredIdentity)},{"preferred_fingerprint",QString::fromLatin1(info.preferredFingerprint.toHex())},{"reference_nits",info.referenceNits},
            {"target_nits",info.targetNits},{"reason",info.reason}};
}
int probeWaylandCapabilities(QGuiApplication& app)
{
    if (!waylandSession()) return report("wayland_capabilities","sdr_fallback",unavailable,{{"reason","not_wayland"}});
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QVulkanInstance instance;
    if (!createVulkanInstance(instance)) return report("wayland_capabilities","sdr_fallback",unavailable,{{"reason","vulkan_unavailable"}});
    std::unique_ptr<Licasa::WaylandHdrObserver> observer;
    QQuickWindow window;
    configureWindow(window); window.setVulkanInstance(&instance);
    new ProbeImageItem(window,pattern(false));
    bool frame=false,finished=false;
    const auto finish=[&] {
        if (!observer || !frame || finished) return;
        const auto info=observer->report();
        if (info.state==Licasa::WaylandHdrReport::State::Pending) return;
        finished=true;
        auto details=waylandDetails(info);
        details["native_surface"]=surfaceDetails(window,nativeSwapchainFormat(window.swapChain()));
        const auto image=window.grabWindow(); details["sdr_frame_captured"]=!image.isNull();
        if (!image.isNull()) details["first_patch_red"]=image.pixelColor(patchSize/2,patchSize/2).redF();
        const bool ready=info.state==Licasa::WaylandHdrReport::State::Ready;
        app.exit(report("wayland_capabilities",ready?"capabilities_ready":"sdr_fallback",ready?0:unavailable,details));
    };
    observer=Licasa::observeWaylandHdr(&window,finish);
    QObject::connect(&window,&QQuickWindow::frameSwapped,&window,[&] { frame=true; finish(); });
    QTimer::singleShot(5000,&window,[&] { if (!finished) app.exit(report("wayland_capabilities","sdr_fallback",unavailable,{{"reason","no_completed_capability_frame"}})); });
    window.show(); return app.exec();
}
int probeSurface(QGuiApplication& app, bool unattended)
{
    if (!waylandSession()) {
        return report("wayland_surface", "sdr_fallback", unavailable, {{"reason", "not_wayland"}});
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QVulkanInstance instance;
    if (!createVulkanInstance(instance)) {
        return report("wayland_surface", "sdr_fallback", unavailable,
                      {{"reason", "vulkan_unavailable"}});
    }
    bool submitted = false;
    int taggedFrames = 0;
    const auto fallback = [&app](const QString& reason, QJsonObject details = {}) {
        details["reason"] = reason;
        app.exit(report("wayland_surface", "sdr_fallback", unavailable, details));
    };
    std::unique_ptr<Licasa::WaylandHdrObserver> observer;
    QQuickWindow window;
    configureWindow(window);
    window.setMinimumSize(patternSize);
    window.setMaximumSize(patternSize);
    window.setTitle(QStringLiteral("Licasa HDR lab · linear signal patches"));
    window.setVulkanInstance(&instance);
    window.setProperty("_qt_sg_hdr_format", QByteArrayLiteral("scrgb"));
    const bool appTagEnabled = waylandTaggedPassthroughEnabled() && !window.requestedFormat().colorSpace().isValid();
    auto* item = new ProbeImageItem(window);
    if (appTagEnabled) { window.setColor(Qt::black); item->setVisible(false); }
    observer=Licasa::observeWaylandHdr(&window,[&] {
        item->update(); window.update();
        if (submitted && observer->report().preferredChanged) fallback(QStringLiteral("preferred_color_changed_restart_probe"));
    });
    item->setHdrGate([&] {
        return observer && observer->report().state==Licasa::WaylandHdrReport::State::Ready
            && hasLinearHdrSurface(window, observer->linearSurfaceTagged());
    });
    QObject::connect(
        &window, &QQuickWindow::sceneGraphError, &window,
        [&fallback](QQuickWindow::SceneGraphError, const QString& message) { fallback(message); },
        Qt::QueuedConnection);
    QObject::connect(&window, &QQuickWindow::frameSwapped, &window, [&] {
        if (!observer) { fallback(QStringLiteral("no_native_wayland_observer")); return; }
        const auto capabilities=observer->report();
        if (capabilities.state==Licasa::WaylandHdrReport::State::Pending) return;
        if (capabilities.state!=Licasa::WaylandHdrReport::State::Ready) { fallback(capabilities.reason,waylandDetails(capabilities)); return; }
        const auto format = nativeSwapchainFormat(window.swapChain());
        const auto surface = format ? classifySurfaceFormat(*format) : SurfaceFormat::Unsupported;
        const auto details = [&] { return surfaceDetails(window, format, waylandDetails(capabilities)); };
        if (surface == SurfaceFormat::PassThrough && !appTagEnabled) {
            fallback(QStringLiteral("unverified_passthrough_color_space"), details());
        } else if (surface == SurfaceFormat::Unsupported) {
            fallback(QStringLiteral("no_linear_fp16_swapchain"), details());
        } else if (surface == SurfaceFormat::PassThrough && !observer->tagLinearSurface()) {
            fallback(QStringLiteral("surface_tag_failed"), details());
        } else if (!item->uploadedFp16()) {
            item->setVisible(true);
            item->update(); window.update();
        } else if (!submitted) {
            if (appTagEnabled && unattended && ++taggedFrames < 4) {
                window.update(); // Allow compositor presentation feedback before closing.
                return;
            }
            submitted = true;
            auto result = details();
            result["swapchain"] = QStringLiteral("extended_linear_srgb");
            result["app_surface_tagged"] = observer->linearSurfaceTagged();
            const int status = report("wayland_surface", "linear_fp16_frame_submitted", 0, result);
            if (unattended) app.exit(status);
        }
    });
    QObject::connect(&window, &QQuickWindow::screenChanged, &window,
                     [&] { fallback(QStringLiteral("screen_changed_restart_probe")); });
    QTimer::singleShot(5000, &window, [&] {
        if (!submitted) {
            fallback(QStringLiteral("no_frame"));
        }
    });
    QTimer::singleShot(0, &window, [&window] { window.show(); });
    return app.exec();
}
} // namespace

int main(int argc, char* argv[])
{
    // Isolate the lab run from renderer overrides and keep probe callbacks on
    // the GUI thread. This process never changes the viewer's configuration.
    qunsetenv("QSG_RHI_HDR");
    qunsetenv("QT_QUICK_BACKEND");
    qunsetenv("QSG_RHI_BACKEND");
    qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    QGuiApplication app(argc, argv);
    Licasa::configureHdrMemoryBudget(8192); // Standalone lab policy; app binds the actual preference.
    if (std::strcmp(qVersion(), QT_VERSION_STR) != 0) {
        return report("startup", "qt_build_runtime_version_mismatch", 1,
                      {{"build_qt", QT_VERSION_STR}});
    }
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Licasa native HDR lab probe"));
    parser.addHelpOption();
    parser.addOption({"software-check", "Verify SDR pixels without a window."});
    parser.addOption({"readback", "Render and verify FP16 pixels without showing a window."});
    parser.addOption({"wayland-capabilities", "Inspect native compositor color capabilities with an SDR surface."});
    parser.addOption({"surface", "Show signal patches on a Wayland Vulkan surface if available."});
    parser.addOption({"surface-check", "Exit after the first confirmed Wayland FP16 frame or fallback."});
#ifdef LICASA_HDR_PROBE_GAINMAP
    parser.addOption({"gainmap-fence-check", "Verify queued Vulkan fence retirement.", "file"});
    parser.addOption({"gainmap-lease-check", "Verify a persistent FP16 texture lease across decoder close.", "file"});
    parser.addOption({"readback-gainmap", "Render and verify a JPEG gain map offscreen.", "file"});
    parser.addOption(
        {"gainmap-headroom", "Gain-map headroom relative to SDR white.", "factor", "1"});
    parser.addOption({"gainmap-full-resolution", "Use the full-resolution gain-map lab budget."});
#endif
#ifdef LICASA_HDR_PROBE_GAINMAP
    parser.addOption({"sample", "Report a readback pixel. Repeat for up to 16 samples.", "x,y"});
#endif
    parser.process(app);
    int photoModes = 0;
#ifdef LICASA_HDR_PROBE_GAINMAP
    const bool gainmapMode = parser.isSet("readback-gainmap");
    photoModes += int(gainmapMode);
    const bool leaseMode = parser.isSet("gainmap-lease-check");
    photoModes += int(leaseMode);
    const bool fenceMode = parser.isSet("gainmap-fence-check");
    photoModes += int(fenceMode);
    if (!gainmapMode &&
        (parser.isSet("gainmap-headroom") || parser.isSet("gainmap-full-resolution"))) {
        parser.showHelp(2);
    }
#endif
#ifdef LICASA_HDR_PROBE_GAINMAP
    if (parser.isSet("sample") && photoModes == 0) {
        parser.showHelp(2);
    }
#endif
    const int modes = photoModes + int(parser.isSet("software-check")) +
                      int(parser.isSet("readback")) + int(parser.isSet("surface")) +
                      int(parser.isSet("surface-check")) + int(parser.isSet("wayland-capabilities"));
    if (modes != 1) {
        parser.showHelp(2);
    }
    if (parser.isSet("software-check")) {
        return checkSoftware();
    }
    if (parser.isSet("readback")) {
        return readbackVulkan(pattern(true), "fp16_readback", {},
                              [](const QImage& frame, QJsonObject details) {
                                  return checkPixels("fp16_readback", frame, true, details);
                              });
    }
#ifdef LICASA_HDR_PROBE_GAINMAP
    if (leaseMode || fenceMode) {
        const int result = checkGainmapLease(parser.value(fenceMode ? "gainmap-fence-check" : "gainmap-lease-check"), fenceMode);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return result;
    }
    if (gainmapMode) {
        return readbackGainmap(parser);
    }
#endif
    if (parser.isSet("wayland-capabilities")) return probeWaylandCapabilities(app);
    return probeSurface(app, parser.isSet("surface-check"));
}
