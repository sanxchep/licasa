#include "imaging/async_image_provider.h"
#include "imaging/compute/edit_compute.h"
#include "imaging/fast_pnm_preview.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_processing.h"
#include "imaging/image_resource_policy.h"
#include "imaging/nearby_preview_cache.h"
#include "imaging/parallel_decode_pair.h"
#include "imaging/preview_decode_support.h"
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
#include "imaging/fast_png_preview.h"
#endif
#ifdef LICASA_HAVE_FAST_WEBP_PREVIEW
#include "imaging/fast_webp_preview.h"
#endif
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
#include "imaging/fast_tiff_preview.h"
#endif
#ifdef LICASA_EARLY_PROGRESSIVE_JPEG
#include "imaging/progressive_jpeg_preview.h"
#endif
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QUrlQuery>
#include <QWaitCondition>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>

namespace Licasa {
namespace {
using namespace PreviewDecodeSupport;

} // namespace

struct DecodedImageCache {
    static QSize normalizedRequestedSize(const QSize& requested)
    {
        return requested.width() > 0 && requested.height() > 0 ? requested : QSize();
    }

    QMutex mutex;
    QString filePath;
    QSize requestedSize;
    qint64 fileSize = -1;
    QDateTime lastModified;
    int maximumMegapixels = 0;
    bool fullDetail = false;
    bool rawFastDevelopment = false;
    QImage image;
    quint64 hitCount = 0;

    bool matchesFile(const QFileInfo& file, int megapixels) const
    {
        return !image.isNull() && filePath == file.absoluteFilePath() && fileSize == file.size() &&
               lastModified == file.lastModified() && maximumMegapixels == megapixels;
    }

    bool preserveFullFor(const QFileInfo& file, int megapixels)
    {
        QMutexLocker lock(&mutex);
        return fullDetail && matchesFile(file, megapixels);
    }

    bool containsFullFor(const QFileInfo& file, int megapixels)
    {
        QMutexLocker lock(&mutex);
        return fullDetail && !rawFastDevelopment && requestedSize.isEmpty() &&
               matchesFile(file, megapixels);
    }

    QImage lookup(const QFileInfo& file, const QSize& requested, int megapixels, bool full,
                  bool rawFast)
    {
        QMutexLocker lock(&mutex);
        if (matchesFile(file, megapixels) && requestedSize == normalizedRequestedSize(requested) &&
            fullDetail == full && rawFastDevelopment == rawFast) {
            ++hitCount;
            return image;
        }
        return {};
    }

    bool store(const QFileInfo& file, const QSize& requested, int megapixels, bool full,
               bool rawFast, const QImage& decoded)
    {
        QMutexLocker lock(&mutex);
        // A late preview edit must not evict the full sensor base that later
        // full-detail edits need. This can race with the speculative reader.
        if (!full && fullDetail && matchesFile(file, megapixels)) {
            return false;
        }
        filePath = file.absoluteFilePath();
        requestedSize = normalizedRequestedSize(requested);
        fileSize = file.size();
        lastModified = file.lastModified();
        maximumMegapixels = megapixels;
        fullDetail = full;
        rawFastDevelopment = rawFast;
        image = decoded;
        return true;
    }

    void clear()
    {
        QMutexLocker lock(&mutex);
        filePath.clear();
        requestedSize = {};
        fileSize = -1;
        lastModified = {};
        maximumMegapixels = 0;
        fullDetail = false;
        rawFastDevelopment = false;
        image = {};
    }
};

namespace {
constexpr qsizetype kMaximumDecodedBaseCacheBytes =
    qsizetype(ImageDecodeContract::maximumDecodedBaseCacheBytes);

bool traceDecodeCache() { return qEnvironmentVariableIsSet("LICASA_TRACE_DECODE_CACHE"); }

bool queryBooleanValue(const QUrlQuery& query, const QString& key, bool defaultValue)
{
    if (!query.hasQueryItem(key)) {
        return defaultValue;
    }

    const QString value = query.queryItemValue(key).trimmed().toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("true") ||
           value == QStringLiteral("yes") || value == QStringLiteral("on");
}

void reportEditBackendChange(const QString& backend)
{
    // Paired preview and full responses may report from different worker pools.
    static QMutex backendMutex;
    static QString lastReportedBackend;
    QMutexLocker lock(&backendMutex);
    if (backend != lastReportedBackend) {
        lastReportedBackend = backend;
        qInfo().noquote() << "[Licasa] Image edit backend:" << backend;
    }
}

void convertToDisplayColorSpace(QImage& image)
{
    const QColorSpace displayColorSpace(QColorSpace::SRgb);
    if (!image.colorSpace().isValid()) {
        image.setColorSpace(displayColorSpace);
        return;
    }
    if (image.colorSpace() == displayColorSpace) {
        return;
    }

    QImage converted = image.convertedToColorSpace(displayColorSpace);
    if (!converted.isNull()) {
        image = std::move(converted);
    }
}

class AsyncDecodeResponse final : public QQuickImageResponse, public QRunnable {
  public:
    AsyncDecodeResponse(QString filePath, QSize requestedSize, ImageEditParameters editParameters,
                        bool colorManagedRendering, bool fullDetail, bool rawFastDevelopment,
                        bool rawInteractiveDevelopment, bool editWarmupRequested,
                        bool parallelPreview, bool speculativeFull,
                        std::shared_ptr<ParallelDecodePair> parallelPair,
                        ImageResourcePolicy& resourcePolicy,
                        std::shared_ptr<DecodedImageCache> decodedImageCache,
                        std::shared_ptr<NearbyPreviewCache> nearbyPreviewCache,
                        std::shared_ptr<std::atomic_bool> cancelled, bool priorityNavigationPreview)
        : filePath_(std::move(filePath)), requestedSize_(requestedSize),
          editParameters_(std::move(editParameters)), colorManagedRendering_(colorManagedRendering),
          fullDetail_(fullDetail), rawFastDevelopment_(rawFastDevelopment),
          rawInteractiveDevelopment_(rawInteractiveDevelopment),
          editWarmupRequested_(editWarmupRequested), parallelPreview_(parallelPreview),
          speculativeFull_(speculativeFull), parallelPair_(std::move(parallelPair)),
          resourcePolicy_(resourcePolicy), decodedImageCache_(std::move(decodedImageCache)),
          nearbyPreviewCache_(std::move(nearbyPreviewCache)), cancelled_(std::move(cancelled)),
          priorityNavigationPreview_(priorityNavigationPreview)
    {
        if (traceDecodePhases()) {
            queuedAt_ = Clock::now();
        }
        setAutoDelete(false);
    }

    QQuickTextureFactory* textureFactory() const override
    {
        return image_.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(image_);
    }

    QString errorString() const override { return error_; }

    void cancel() override { cancelled_->store(true, std::memory_order_relaxed); }

    void run() override
    {
        const bool tracing = traceDecodePhases();
        const auto startedAt = tracing ? Clock::now() : Clock::time_point{};
        Clock::duration gateWait{};
        const auto recordGateWait = [&](Clock::time_point waitingAt) {
            if (tracing) {
                gateWait += Clock::now() - waitingAt;
            }
        };
        const bool priorityPreviewReady =
            !isCancelled() && priorityNavigationPreview_ && tryPriorityPreview();
        if (parallelPreview_ && parallelPair_) {
            bool bypassGate = false;
            int pairedMaximumMegapixels = 0;
            if (!priorityPreviewReady && !isCancelled()) {
                QMutexLocker pairLock(&parallelPair_->mutex);
                bypassGate = parallelPair_->fullOwnsGate;
                if (bypassGate) {
                    pairedMaximumMegapixels = parallelPair_->maximumMegapixels;
                    parallelPair_->previewBypassedGate = true;
                }
            }
            if (!priorityPreviewReady && !isCancelled()) {
                const auto waitingAt = tracing ? Clock::now() : Clock::time_point{};
                std::unique_lock<QMutex> processingLock(resourcePolicy_.processingMutex(),
                                                        std::defer_lock);
                // The full reader can take the processing gate between the
                // pair-state check above and this lock. Recheck that state
                // while waiting so an absent RAW preview does not sit behind
                // its own full decode and add the 200 ms presentation timeout.
                while (!bypassGate && !isCancelled() &&
                       !processingLock.try_lock_for(std::chrono::milliseconds(5))) {
                    QMutexLocker pairLock(&parallelPair_->mutex);
                    if (parallelPair_->fullOwnsGate) {
                        bypassGate = true;
                        pairedMaximumMegapixels = parallelPair_->maximumMegapixels;
                        parallelPair_->previewBypassedGate = true;
                    }
                }
                recordGateWait(waitingAt);
                if (!isCancelled()) {
                    // The paired full reader owns the heavy gate until this
                    // preview ends, including cache publication.
                    decodeImage(bypassGate ? pairedMaximumMegapixels
                                           : resourcePolicy_.prepareForProcessing());
                }
            }
            parallelPair_->finishPreview(isCancelled() || !error_.isEmpty());
        } else if (priorityPreviewReady) {
            // A bounded selected preview can be shown while the cancelled old
            // full reader is still retiring from LibRaw.
        } else if (!isCancelled() && speculativeFull_ && parallelPair_) {
            const auto waitingAt = tracing ? Clock::now() : Clock::time_point{};
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            recordGateWait(waitingAt);
            const int maximumMegapixels = resourcePolicy_.prepareForProcessing();
            {
                QMutexLocker pairLock(&parallelPair_->mutex);
                parallelPair_->maximumMegapixels = maximumMegapixels;
                parallelPair_->fullOwnsGate = true;
            }
            decodeImage(maximumMegapixels);
            QMutexLocker pairLock(&parallelPair_->mutex);
            parallelPair_->fullOwnsGate = false;
            while (parallelPair_->previewBypassedGate && !parallelPair_->previewFinished) {
                parallelPair_->previewFinishedCondition.wait(&parallelPair_->mutex);
            }
        } else if (!isCancelled()) {
            const auto waitingAt = tracing ? Clock::now() : Clock::time_point{};
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            recordGateWait(waitingAt);
            if (!isCancelled()) {
                decodeImage(resourcePolicy_.prepareForProcessing());
            }
        }
        // Destroy the reader and release the processing gate before publishing
        // completion: a consumer may start the next heavy operation immediately.
        const bool prewarm = prewarmAfterRawDecode_ && !isCancelled() &&
                             !qEnvironmentVariableIsSet("LICASA_DISABLE_RAW_EDIT_PREWARM");
        if (tracing) {
            const auto finishedAt = Clock::now();
            const auto milliseconds = [](Clock::duration duration) {
                return std::chrono::duration<double, std::milli>(duration).count();
            };
            const char* stage = priorityNavigationPreview_   ? "navigation-preview"
                                : parallelPreview_           ? "parallel-preview"
                                : speculativeFull_           ? "speculative-full"
                                : rawFastDevelopment_        ? "raw-fast"
                                : rawInteractiveDevelopment_ ? "raw-interactive"
                                : fullDetail_                ? "full"
                                                             : "preview";
            qInfo().noquote() << "[Licasa] Decode phases:" << stage
                              << "queue_ms=" << milliseconds(startedAt - queuedAt_)
                              << "gate_ms=" << milliseconds(gateWait)
                              << "worker_ms=" << milliseconds(finishedAt - startedAt)
                              << "cancelled=" << isCancelled() << "error=" << !error_.isEmpty()
                              << "output=" << image_.size();
        }
        if (prewarm) {
            // Keep driver setup off the serial decoder and the full-detail
            // publication path. The shared compute lock makes this safe if an
            // edit starts before the low-priority task gets a worker.
            QThreadPool::globalInstance()->start(
                [] {
                    ImageEditExecution execution;
                    warmGpuImageEdits(execution);
                },
                -1);
        }
        // Qt can retire the response as soon as it observes completion. Queue
        // publication on the response's owner thread so it cannot be destroyed
        // by the pixmap reader while this worker is still emitting its signal.
        // This also lets Qt finish registering a very fast/cancelled response.
        QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
    }

  private:
    using Clock = std::chrono::steady_clock;

    static bool traceDecodePhases()
    {
        static const bool enabled = qEnvironmentVariableIsSet("LICASA_TRACE_DECODE_PHASES");
        return enabled;
    }

    bool tryPriorityPreview()
    {
        // Prepared neighbors are already charged to the bounded nearby cache.
        // Reusing one needs no decoder or heavy-operation gate.
        const QString suffix = QFileInfo(filePath_).suffix().toLower();
        QImage preparedNeighbor = nearbyPreviewCache_->lookup(filePath_);
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
        if (preparedNeighbor.isNull() && suffix == QStringLiteral("png")) {
            nearbyPreviewCache_->waitForActivePng(filePath_, cancelled_.get());
            if (!isCancelled()) {
                preparedNeighbor = nearbyPreviewCache_->lookup(filePath_);
            }
        }
#endif
        if (!preparedNeighbor.isNull()) {
            decodeImage(resourcePolicy_.maximumImageMegapixels(), std::move(preparedNeighbor));
            return !image_.isNull();
        }

        constexpr int previewMegapixels = 2;
        constexpr quint64 previewPixels = quint64(previewMegapixels) * 1000000;
        const bool jpeg = isJpegSuffix(suffix);
        const bool smallRaster = suffix == QStringLiteral("png") ||
                                 suffix == QStringLiteral("bmp") ||
                                 suffix == QStringLiteral("webp");
        const bool raw = isRawSuffix(suffix);
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
        if (isTiffSuffix(suffix)) {
            QImage fastPreview = readFastTiffPreview(filePath_, requestedSize_, previewPixels,
                                                     8 * 1024 * 1024, cancelled_.get());
            if (!fastPreview.isNull()) {
                decodeImage(previewMegapixels, std::move(fastPreview));
                return !image_.isNull();
            }
            return false;
        }
#endif
        if (!jpeg && !smallRaster && !raw) {
            return false;
        }

        bool safe = false;
        {
            QImageReader reader(filePath_);
            ImageDecodeContract::configure(reader, previewPixels, cancelled_.get());
            if (!reader.canRead() || isCancelled()) {
                return false;
            }
            const QSize source = reader.size();
            const QSize embedded = reader.device()
                                       ->property(ImageDecodeContract::embeddedPreviewSizeProperty)
                                       .toSize();
            const bool scalableEmbeddedJpeg =
                reader.device()
                    ->property(ImageDecodeContract::scalableEmbeddedJpegProperty)
                    .toBool();
            const bool boundedSource = ImageDecodeContract::allows(source, previewPixels);
            safe =
                (jpeg && (boundedSource || reader.supportsOption(QImageIOHandler::ScaledSize))) ||
                (smallRaster && boundedSource) ||
                (raw &&
                 (ImageDecodeContract::allows(embedded, previewPixels) || scalableEmbeddedJpeg));
        }
        if (!safe) {
            return false;
        }

        decodeImage(previewMegapixels);
        if (!image_.isNull() || isCancelled()) {
            return !image_.isNull();
        }
        // If a codec declined the bounded route, retry through the normal
        // resource gate with its usual policy and error reporting.
        error_.clear();
        return false;
    }

    void decodeImage(int maximumMegapixels, QImage preparedNeighbor = {})
    {
        const quint64 maximumPixels = ImageProcessing::pixelsForMegapixels(maximumMegapixels);
        const QFileInfo fileInfo(filePath_);
        if (!fileInfo.exists() || !fileInfo.isFile()) {
            error_ = QStringLiteral("File not found: %1").arg(filePath_);
            return;
        }

        QImage image = std::move(preparedNeighbor);
        if (image.isNull() && !fullDetail_) {
            image = nearbyPreviewCache_->lookup(filePath_);
        }
        if (image.isNull() && !parallelPreview_) {
            image = decodedImageCache_->lookup(fileInfo, requestedSize_, maximumMegapixels,
                                               fullDetail_, rawFastDevelopment_);
        }
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
        if (image.isNull() && !fullDetail_ && !isCancelled()) {
            const QString suffix = fileInfo.suffix().toLower();
            if (isTiffSuffix(suffix)) {
                image = readFastTiffPreview(filePath_, requestedSize_, maximumPixels,
                                            8 * 1024 * 1024, cancelled_.get());
            }
        }
#endif
        const bool decodedCacheHit = !image.isNull();
        if (decodedCacheHit) {
            if (traceDecodeCache()) {
                qInfo().noquote()
                    << QStringLiteral("[Licasa] Decoded base cache hit: %1 MiB (%2x%3, %4)")
                           .arg(double(image.sizeInBytes()) / (1024.0 * 1024.0), 0, 'f', 1)
                           .arg(image.width())
                           .arg(image.height())
                           .arg(fullDetail_ ? QStringLiteral("full") : QStringLiteral("preview"));
            }
        } else {
            // Do not retain a stale large base raster while LibRaw or another
            // decoder allocates the next one. The active scene-graph texture is
            // independent of this CPU-side edit cache.
            if (!parallelPreview_ && (fullDetail_ || !decodedImageCache_->preserveFullFor(
                                                         fileInfo, maximumMegapixels))) {
                decodedImageCache_->clear();
            }
            QImageReader reader(filePath_);
            ImageDecodeContract::configure(reader, maximumPixels, cancelled_.get(), fullDetail_,
                                           rawFastDevelopment_, rawInteractiveDevelopment_);
            const QString suffix = fileInfo.suffix().toLower();
            const bool retainDecodedFullBase =
                !fullDetail_ && !parallelPreview_ && !rawFastDevelopment_ &&
                (suffix == QStringLiteral("heic") || suffix == QStringLiteral("heif") ||
                 suffix == QStringLiteral("hif") || suffix == QStringLiteral("jxl") ||
                 suffix == QStringLiteral("avif"));
            if (retainDecodedFullBase && reader.device()) {
                reader.device()->setProperty(ImageDecodeContract::retainFullBaseProperty, true);
            }
            if (speculativeFull_ && parallelPair_ && reader.device()) {
                reader.device()->setProperty(ImageDecodeContract::previewPresentedProperty,
                                             QVariant::fromValue(reinterpret_cast<quintptr>(
                                                 &parallelPair_->previewPresented)));
            }
            reader.setAutoTransform(true);

            if (!reader.canRead()) {
                if (!isCancelled()) {
                    error_ = ImageProcessing::readerErrorOrFallback(reader);
                }
                return;
            }

            if (speculativeFull_) {
                // Two readers may allocate concurrently. Reserve room for two
                // conservative full-raster estimates under the one image budget.
                // Unknown dimensions and near-limit images use the serial path.
                const QSize dimensions = reader.size();
                const QSize sensor =
                    reader.device()->property(ImageDecodeContract::sensorSizeProperty).toSize();
                const auto fitsPair = [maximumPixels](const QSize& size) {
                    return ImageDecodeContract::allows(size, maximumPixels / 2);
                };
                if (!fitsPair(dimensions) || (sensor.isValid() && !fitsPair(sensor))) {
                    error_ = QStringLiteral("Parallel decoding exceeds the image memory budget.");
                    return;
                }
            }

            if (!configurePreview(reader, maximumMegapixels, maximumPixels) || isCancelled()) {
                return;
            }

            if (!fullDetail_ && !parallelPreview_ && !rawFastDevelopment_ &&
                !rawInteractiveDevelopment_ &&
                (suffix == QStringLiteral("ppm") || suffix == QStringLiteral("pnm")) &&
                reader.format() == QByteArrayLiteral("ppm") && reader.scaledSize().isValid() &&
                reader.transformation() == QImageIOHandler::TransformationNone) {
                image = readFastPnmPreview(filePath_, reader.scaledSize(), maximumPixels,
                                           8 * 1024 * 1024, cancelled_.get());
                if (isCancelled()) {
                    return;
                }
            }

#ifdef LICASA_HAVE_FAST_WEBP_PREVIEW
            const bool onePassWebpBase =
                suffix == QStringLiteral("webp") && reader.size().isValid() &&
                !ImageDecodeContract::allows(reader.size(), maximumPixels / 2);
            if (!fullDetail_ && (!parallelPreview_ || onePassWebpBase) && !rawFastDevelopment_ &&
                !rawInteractiveDevelopment_ && suffix == QStringLiteral("webp") &&
                reader.scaledSize().isValid() &&
                reader.transformation() == QImageIOHandler::TransformationNone) {
                QImage fullBase;
                image = readFastWebpPreview(
                    filePath_, reader.scaledSize(), maximumPixels, 8 * 1024 * 1024,
                    cancelled_.get(), onePassWebpBase ? &fullBase : nullptr, 400 * 1024 * 1024);
                if (!image.isNull() && !fullBase.isNull() && !isCancelled()) {
                    // A large, complex WebP spends nearly as long on its
                    // scaled preview as on full decode. Keep that one full
                    // raster through the immediate full-detail promotion.
                    // This is limited to an admitted 400 MiB source and only
                    // when the paired full reader cannot run concurrently.
                    decodedImageCache_->store(fileInfo, QSize(), maximumMegapixels, true, false,
                                              fullBase);
                }
            }
#endif

            const bool qtReusePngBase =
                !fullDetail_ && !parallelPreview_ && !rawFastDevelopment_ &&
                !rawInteractiveDevelopment_ && suffix == QStringLiteral("png") &&
                reader.scaledSize().isValid() &&
                reader.transformation() == QImageIOHandler::TransformationNone &&
                (!reader.supportsAnimation() || reader.imageCount() == 1) &&
                ImageDecodeContract::reusablePngFullBase(reader.size(), reader.imageFormat(),
                                                         maximumPixels);
            bool fastPngBase = false;
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
            fastPngBase = !fullDetail_ && !parallelPreview_ && !rawFastDevelopment_ &&
                          !rawInteractiveDevelopment_ && suffix == QStringLiteral("png") &&
                          reader.scaledSize().isValid() &&
                          reader.transformation() == QImageIOHandler::TransformationNone &&
                          ImageDecodeContract::boundedPngFullBase(reader.size(), maximumPixels) &&
                          fastPngBaseEligible(filePath_, maximumPixels);
#endif

#ifdef LICASA_EARLY_PROGRESSIVE_JPEG
            const QString extension = fileInfo.suffix().toLower();
            if (!fullDetail_ && !rawFastDevelopment_ && !rawInteractiveDevelopment_ &&
                isJpegSuffix(extension) && reader.scaledSize().isValid() &&
                reader.transformation() == QImageIOHandler::TransformationNone) {
                image = readEarlyProgressiveJpegPreview(filePath_, reader.scaledSize(),
                                                        maximumPixels, cancelled_.get());
            }
#endif
            if (image.isNull() && fastPngBase) {
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
                FastPngPreview decoded =
                    readFastPngPreview(filePath_, reader.scaledSize(), maximumPixels,
                                       8 * 1024 * 1024, cancelled_.get());
                if (!decoded.preview.isNull() && !decoded.fullBase.isNull() && !isCancelled()) {
                    image = std::move(decoded.preview);
                    const bool stored = decodedImageCache_->store(
                        fileInfo, QSize(), maximumMegapixels, true, false, decoded.fullBase);
                    if (traceDecodeCache() && stored) {
                        qInfo().noquote()
                            << QStringLiteral("[Licasa] Decoded full PNG base retained from "
                                              "fast preview: %1 MiB (%2x%3)")
                                   .arg(double(decoded.fullBase.sizeInBytes()) / (1024.0 * 1024.0),
                                        0, 'f', 1)
                                   .arg(decoded.fullBase.width())
                                   .arg(decoded.fullBase.height());
                    }
                }
#endif
            }
            if (image.isNull() && (qtReusePngBase || fastPngBase)) {
                const QSize previewSize = reader.scaledSize();
                const QSize sourceSize = reader.size();
                reader.setScaledSize(QSize());
                QImage fullBase = reader.read();
                if (!fullBase.isNull() && !isCancelled()) {
                    if (!fullBase.colorSpace().isValid()) {
                        fullBase.setColorSpace(QColorSpace::SRgb);
                    }
                    image = fullBase.scaled(previewSize, Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation);
                    if (!image.isNull() && !isCancelled() && fullBase.size() == sourceSize &&
                        fullBase.sizeInBytes() > 0 &&
                        fullBase.sizeInBytes() <= kMaximumDecodedBaseCacheBytes) {
                        const bool stored = decodedImageCache_->store(
                            fileInfo, QSize(), maximumMegapixels, true, false, fullBase);
                        if (traceDecodeCache() && stored) {
                            qInfo().noquote()
                                << QStringLiteral("[Licasa] Decoded full PNG base retained from "
                                                  "preview: %1 MiB (%2x%3)")
                                       .arg(double(fullBase.sizeInBytes()) / (1024.0 * 1024.0), 0,
                                            'f', 1)
                                       .arg(fullBase.width())
                                       .arg(fullBase.height());
                        }
                    }
                }
            } else if (image.isNull()) {
                image = reader.read();
            }
            if (isCancelled()) {
                return;
            }
            if (image.isNull()) {
                error_ = ImageProcessing::readerErrorOrFallback(reader);
                return;
            }
            // Unprofiled PNG pixels are treated as sRGB by the display path.
            // Tag before cache publication, while the raster is still owned
            // here, so a later full/edit request does not detach a huge copy.
            if (suffix == QStringLiteral("png") && !image.colorSpace().isValid()) {
                image.setColorSpace(QColorSpace::SRgb);
            }
            if (!ImageDecodeContract::allows(image.size(), maximumPixels)) {
                image = {};
                error_ = ImageProcessing::imageLimitError(maximumMegapixels);
                return;
            }
            if (retainDecodedFullBase && reader.device()) {
                const QImage fullBase = reader.device()
                                            ->property(ImageDecodeContract::decodedFullBaseProperty)
                                            .value<QImage>();
                if (!fullBase.isNull() && fullBase.sizeInBytes() > 0 &&
                    fullBase.sizeInBytes() <= kMaximumDecodedBaseCacheBytes &&
                    ImageDecodeContract::allows(fullBase.size(), maximumPixels) &&
                    fullBase.size() == reader.size()) {
                    const bool stored = decodedImageCache_->store(
                        fileInfo, QSize(), maximumMegapixels, true, false, fullBase);
                    if (traceDecodeCache() && stored) {
                        qInfo().noquote()
                            << QStringLiteral("[Licasa] Decoded full base retained from preview: "
                                              "%1 MiB (%2x%3)")
                                   .arg(double(fullBase.sizeInBytes()) / (1024.0 * 1024.0), 0, 'f',
                                        1)
                                   .arg(fullBase.width())
                                   .arg(fullBase.height());
                    }
                }
            }
            // Cache by retained bytes, not megapixels. Camera RAWs commonly
            // exceed 16 MP while their decoded RGB8 base raster is still a
            // modest 50-100 MiB. Keeping that immutable base means a slider
            // request after full-detail promotion does not reopen, unpack and
            // demosaic the RAW again. QImage is implicitly shared, so the cache
            // does not duplicate the raster until the edit path detaches it.
            if (!parallelPreview_ && image.sizeInBytes() > 0 &&
                image.sizeInBytes() <= kMaximumDecodedBaseCacheBytes) {
                const bool stored =
                    decodedImageCache_->store(fileInfo, requestedSize_, maximumMegapixels,
                                              fullDetail_, rawFastDevelopment_, image);
                if (traceDecodeCache() && stored) {
                    qInfo().noquote()
                        << QStringLiteral("[Licasa] Decoded base cached: %1 MiB (%2x%3, %4)")
                               .arg(double(image.sizeInBytes()) / (1024.0 * 1024.0), 0, 'f', 1)
                               .arg(image.width())
                               .arg(image.height())
                               .arg(fullDetail_ ? QStringLiteral("full")
                                                : QStringLiteral("preview"));
                }
            } else if (traceDecodeCache()) {
                qInfo().noquote() << QStringLiteral(
                                         "[Licasa] Decoded base not cached: %1 MiB exceeds %2 MiB")
                                         .arg(double(image.sizeInBytes()) / (1024.0 * 1024.0), 0,
                                              'f', 1)
                                         .arg(kMaximumDecodedBaseCacheBytes / (1024 * 1024));
            }
        }

        if (editWarmupRequested_ && !editParameters_.hasColorAdjustments() &&
            editParameters_.sharpen <= 0.0 && !isCancelled()) {
            ImageEditExecution warmupExecution;
            warmGpuImageEdits(warmupExecution);
        }

        ImageEditExecution editExecution;
        if (!applyImageEdits(image, editParameters_, cancelled_.get(), &editExecution)) {
            image = {};
            if (!isCancelled()) {
                error_ = QStringLiteral("The image could not be processed safely.");
            }
            return;
        }
        if (editParameters_.hasColorAdjustments() || editParameters_.sharpen > 0.0) {
            reportEditBackendChange(editExecution.detail);
        }
        if (isCancelled()) {
            image = {};
            return;
        }

        // Keep the edit math identical to the save pipeline, then perform the
        // display-only color conversion as the final preview step.
        if (colorManagedRendering_) {
            convertToDisplayColorSpace(image);
        }

        image_ = std::move(image);
        prewarmAfterRawDecode_ = rawInteractiveDevelopment_ && !decodedCacheHit && !image_.isNull();
    }

    bool configurePreview(QImageReader& reader, int maximumMegapixels, quint64 maximumPixels)
    {
        QSize expectedDecodeSize = reader.size();
        const QSize previewSize = boundedPreviewSize(requestedSize_, maximumPixels);
        const bool sourceWithinBudget =
            ImageDecodeContract::allows(expectedDecodeSize, maximumPixels);
        if (!sourceWithinBudget &&
            (!previewSize.isValid() || !reader.supportsOption(QImageIOHandler::ScaledSize))) {
            // QImageReader can emulate setScaledSize by reading a full raster
            // and scaling afterwards. A small requested output alone is not
            // evidence that an oversized/unknown input is safe to decode.
            error_ = QStringLiteral("This decoder cannot create a bounded preview without "
                                    "decoding the full image. ") +
                     ImageProcessing::imageLimitError(maximumMegapixels);
            return false;
        }
        if (previewSize.isValid()) {
            const QSize sourceSize = expectedDecodeSize;
            if (!sourceSize.isValid()) {
                reader.setScaledSize(previewSize);
                expectedDecodeSize = previewSize;
            } else if (previewSize.width() < sourceSize.width() ||
                       previewSize.height() < sourceSize.height()) {
                QSize scaledSize = sourceSize;
                scaledSize.scale(previewSize, Qt::KeepAspectRatio);
                if (scaledSize.isValid() && scaledSize.width() > 0 && scaledSize.height() > 0) {
                    reader.setScaledSize(scaledSize);
                    expectedDecodeSize = scaledSize;
                }
            }
        }

        if (expectedDecodeSize.isValid() &&
            !ImageDecodeContract::allows(expectedDecodeSize, maximumPixels)) {
            error_ = ImageProcessing::imageLimitError(maximumMegapixels);
            return false;
        }

        return true;
    }

    bool isCancelled() const noexcept { return cancelled_->load(std::memory_order_relaxed); }

    QString filePath_;
    QSize requestedSize_;
    ImageEditParameters editParameters_;
    bool colorManagedRendering_ = true;
    bool fullDetail_ = false;
    bool rawFastDevelopment_ = false;
    bool rawInteractiveDevelopment_ = false;
    bool editWarmupRequested_ = false;
    bool parallelPreview_ = false;
    bool speculativeFull_ = false;
    std::shared_ptr<ParallelDecodePair> parallelPair_;
    bool prewarmAfterRawDecode_ = false;
    ImageResourcePolicy& resourcePolicy_;
    std::shared_ptr<DecodedImageCache> decodedImageCache_;
    std::shared_ptr<NearbyPreviewCache> nearbyPreviewCache_;
    QImage image_;
    QString error_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    bool priorityNavigationPreview_ = false;
    Clock::time_point queuedAt_;
};

} // namespace

AsyncImageProvider::AsyncImageProvider(ImageResourcePolicy& resourcePolicy)
    : resourcePolicy_(resourcePolicy), decodedImageCache_(std::make_shared<DecodedImageCache>()),
      nearbyPreviewCache_(std::make_shared<NearbyPreviewCache>()),
      pendingPairs_(std::make_unique<ParallelDecodePairRegistry>())
{
    // Ordinary edits/animation work remain serial. Only an explicitly paired
    // bounded preview and admitted full read may overlap.
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(1000);
    navigationPreviewPool_.setMaxThreadCount(1);
    navigationPreviewPool_.setExpiryTimeout(1000);
    nearbyPreviewPool_.setMaxThreadCount(1);
    nearbyPreviewPool_.setExpiryTimeout(1000);
    speculativeFullPool_.setMaxThreadCount(1);
    speculativeFullPool_.setExpiryTimeout(1000);
    parallelPreviewPool_.setMaxThreadCount(1);
    parallelPreviewPool_.setExpiryTimeout(1000);
}

AsyncImageProvider::~AsyncImageProvider()
{
    // Drain queued retirement work as well as active decodes. Clearing the
    // queue can destroy captured readers on the GUI thread and discard the
    // completion of a cancelled image response.
    waitForPendingWork();
}

void AsyncImageProvider::waitForPendingWork()
{
    pool_.waitForDone();
    navigationPreviewPool_.waitForDone();
    nearbyPreviewPool_.waitForDone();
    parallelPreviewPool_.waitForDone();
    speculativeFullPool_.waitForDone();
}

void AsyncImageProvider::enqueueWork(std::function<void()> work) { pool_.start(std::move(work)); }

void AsyncImageProvider::releasePictureResources(bool retainNearbyPreviews, const QUrl& nextImage)
{
    // Navigation can happen while a full RAW reader still owns the processing
    // gate. Cancel its native decode before the next image requests a preview.
    // Keep the already-presented QML texture; only in-flight readers stop.
    {
        QMutexLocker requestsLock(&requestsMutex_);
        for (const auto& request : activeRequests_) {
            if (const auto cancelled = request.lock()) {
                cancelled->store(true, std::memory_order_relaxed);
            }
        }
        activeRequests_.clear();
        pendingPairs_->clear();
        nextNavigationPreview_ = retainNearbyPreviews;
        nextNavigationPath_ =
            retainNearbyPreviews && nextImage.isLocalFile() ? nextImage.toLocalFile() : QString();
    }
    // Keep filling the current neighbor window until the new center is ready.
    // Most prepared entries still overlap after a single navigation step.
    if (!retainNearbyPreviews) {
        nearbyPreviewCache_->clear();
    } else {
        // A selected image has priority over expensive nearby codec work.
        // The completed nearby entries remain available for this navigation.
        nearbyPreviewCache_->cancelActiveHeavyCodec();
    }
    // Decode-cache ownership lives on this serial pool. Release both its CPU
    // raster and GPU staging buffers after earlier image work has finished.
    pool_.start([cache = decodedImageCache_] {
        cache->clear();
        releaseGpuImageEditBuffers();
    });
    speculativeFullPool_.start([cache = decodedImageCache_] { cache->clear(); });
}

void AsyncImageProvider::prepareNearbyPreviews(const QList<QUrl>& urls, const QSize& requestedSize,
                                               int maximumImageMemoryMiB)
{
    if (nearbyPreviewCache_->setWindow(urls, requestedSize, maximumImageMemoryMiB)) {
        nearbyPreviewPool_.start([cache = nearbyPreviewCache_,
                                  this] { cache->run(resourcePolicy_, nearbyPreviewPool_); },
                                 -1);
    }
}

qsizetype AsyncImageProvider::nearbyPreviewCount() const { return nearbyPreviewCache_->count(); }

qsizetype AsyncImageProvider::nearbyPreviewBytes() const
{
    return nearbyPreviewCache_->retainedBytes();
}

quint64 AsyncImageProvider::nearbyPreviewHitCount() const { return nearbyPreviewCache_->hits(); }

quint64 AsyncImageProvider::decodedBaseCacheHitCount() const
{
    QMutexLocker lock(&decodedImageCache_->mutex);
    return decodedImageCache_->hitCount;
}

bool AsyncImageProvider::hasDecodedFullBase(const QUrl& url) const
{
    return url.isLocalFile() &&
           decodedImageCache_->containsFullFor(QFileInfo(url.toLocalFile()),
                                               resourcePolicy_.maximumImageMegapixels());
}

bool AsyncImageProvider::hasNearbyPreview(const QUrl& url) const
{
    return url.isLocalFile() && !nearbyPreviewCache_->lookup(url.toLocalFile()).isNull();
}

void AsyncImageProvider::markParallelPreviewPresented(const QString& pairId)
{
    pendingPairs_->markPresented(pairId);
}

QQuickImageResponse* AsyncImageProvider::requestImageResponse(const QString& id,
                                                              const QSize& requestedSize)
{
    QString payload = id;
    if (payload.startsWith('/')) {
        payload.remove(0, 1);
    }

    QString queryString;
    const int queryPosition = payload.indexOf('?');
    if (queryPosition >= 0) {
        queryString = payload.mid(queryPosition + 1);
        payload = payload.left(queryPosition);
    }

    const int slashPosition = payload.indexOf('/');
    const QString encodedPath = slashPosition >= 0 ? payload.mid(slashPosition + 1) : payload;
    const QString filePath = QUrl::fromPercentEncoding(encodedPath.toUtf8());

    const QUrlQuery query(queryString);
    const bool colorManagedRendering = queryBooleanValue(query, QStringLiteral("cm"), true);
    const QString decodeStage = query.queryItemValue(QStringLiteral("licasa_stage"));
    const bool fullDetail = decodeStage == QStringLiteral("full") ||
                            decodeStage == QStringLiteral("raw-fast") ||
                            decodeStage == QStringLiteral("raw-interactive");
    const bool rawFastDevelopment = decodeStage == QStringLiteral("raw-fast");
    const bool rawInteractiveDevelopment = decodeStage == QStringLiteral("raw-interactive");
    const QString parallelStage = query.queryItemValue(QStringLiteral("licasa_parallel"));
    const bool parallelPreview = parallelStage == QStringLiteral("preview") && !fullDetail;
    const bool speculativeFull = parallelStage == QStringLiteral("full") && fullDetail;
    const QString pairId = query.queryItemValue(QStringLiteral("licasa_pair"));
    std::shared_ptr<ParallelDecodePair> pair;
    QByteArray pairFingerprint;
    if (!pairId.isEmpty() && (parallelPreview || speculativeFull)) {
        QUrlQuery commonQuery(query);
        for (const auto* key :
             {"licasa_stage", "licasa_width", "licasa_height", "licasa_parallel", "licasa_pair"}) {
            commonQuery.removeAllQueryItems(QString::fromLatin1(key));
        }
        pairFingerprint = QCryptographicHash::hash(
            payload.toUtf8() + '\n' + commonQuery.query(QUrl::FullyEncoded).toUtf8(),
            QCryptographicHash::Sha256);
    }
    bool warmupOk = false;
    const int editWarmupRevision = query.queryItemValue(QStringLiteral("ew")).toInt(&warmupOk);
    const bool editWarmupRequested = warmupOk && editWarmupRevision > 0;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    bool navigationPriority = false;
    {
        QMutexLocker requestsLock(&requestsMutex_);
        // Pair ownership and cancellation admission share the resource-scope
        // boundary. Release cannot clear a new pair but miss its request flag.
        if (!pairFingerprint.isEmpty()) {
            pair = pendingPairs_->claim(pairId, pairFingerprint,
                                        parallelPreview ? ParallelDecodePairRegistry::Role::Preview
                                                        : ParallelDecodePairRegistry::Role::Full);
        }
        activeRequests_.erase(std::remove_if(activeRequests_.begin(), activeRequests_.end(),
                                             [](const auto& request) { return request.expired(); }),
                              activeRequests_.end());
        activeRequests_.push_back(cancelled);
        const bool selectedImage = nextNavigationPath_.isEmpty() || filePath == nextNavigationPath_;
        if (!fullDetail && nextNavigationPreview_ && selectedImage) {
            navigationPriority = true;
            nextNavigationPreview_ = false;
        } else if (fullDetail && !(speculativeFull && pair) && selectedImage) {
            // A direct full request is the selected image. Do not save the
            // one-shot preview slot for an unrelated later edit request.
            nextNavigationPreview_ = false;
        }
    }
    auto* response = new AsyncDecodeResponse(
        filePath, requestedSize, editParametersFromQuery(query), colorManagedRendering, fullDetail,
        rawFastDevelopment, rawInteractiveDevelopment, editWarmupRequested,
        parallelPreview && bool(pair), speculativeFull && bool(pair), pair, resourcePolicy_,
        decodedImageCache_, nearbyPreviewCache_, std::move(cancelled), navigationPriority);
    if (speculativeFull && pair) {
        speculativeFullPool_.start(response);
    } else if (navigationPriority) {
        navigationPreviewPool_.start(response);
    } else if (parallelPreview && pair) {
        parallelPreviewPool_.start(response);
    } else {
        pool_.start(response);
    }
    return response;
}

} // namespace Licasa
