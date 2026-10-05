#include "imaging/async_image_provider.h"
#include "imaging/compute/edit_compute.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_edit_pipeline.h"
#include "imaging/image_processing.h"
#include "imaging/image_resource_policy.h"
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
#include "imaging/fast_tiff_preview.h"
#endif
#include "io/external_file_identity.h"
#ifdef LICASA_EARLY_PROGRESSIVE_JPEG
#include "imaging/progressive_jpeg_preview.h"
#endif
#include <QColorSpace>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QSet>
#include <QUrlQuery>
#include <QWaitCondition>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace Licasa {
namespace {
QSize boundedPreviewSize(const QSize& requestedSize, quint64 maximumPixels);

struct NearbyFileStamp {
    QString canonicalPath;
    qint64 size = -1;
    QDateTime modified;
    QDateTime changed;
    std::optional<ExternalFileIdentity> nativeIdentity;

    bool isValid() const { return size >= 0 && !canonicalPath.isEmpty(); }
};

NearbyFileStamp nearbyFileStamp(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return {info.canonicalFilePath(), info.size(), info.lastModified(), info.metadataChangeTime(),
            externalFileIdentity(file)};
}

bool sameFileStamp(const NearbyFileStamp& left, const NearbyFileStamp& right)
{
    return left.isValid() && right.isValid() && left.canonicalPath == right.canonicalPath &&
           left.size == right.size && left.modified == right.modified &&
           left.changed == right.changed && left.nativeIdentity == right.nativeIdentity;
}

QSize admittedNativeJpegSize(const QSize& source, quint64 maximumPixels)
{
    if (!source.isValid() || source.isEmpty()) {
        return {};
    }
    for (const quint64 divider : {quint64(1), quint64(2), quint64(4), quint64(8)}) {
        const QSize nativeSize(int((quint64(source.width()) + divider - 1) / divider),
                               int((quint64(source.height()) + divider - 1) / divider));
        if (ImageDecodeContract::allows(nativeSize, maximumPixels)) {
            return nativeSize;
        }
    }
    return {};
}
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

    QImage lookup(const QFileInfo& file, const QSize& requested, int megapixels, bool full,
                  bool rawFast)
    {
        QMutexLocker lock(&mutex);
        if (matchesFile(file, megapixels) && requestedSize == normalizedRequestedSize(requested) &&
            fullDetail == full && rawFastDevelopment == rawFast) {
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

struct NearbyPreviewCache : std::enable_shared_from_this<NearbyPreviewCache> {
    struct Entry {
        QImage image;
        NearbyFileStamp stamp;
    };

    QMutex mutex;
    QStringList desired;
    QHash<QString, Entry> entries;
    QSet<QString> attempted;
    QSize requestedSize;
    qsizetype byteLimit = 0;
    qsizetype slotLimit = 0;
    qsizetype bytes = 0;
    quint64 hitCount = 0;
    std::shared_ptr<std::atomic_bool> activeCancellation;
    bool workerRunning = false;

    void cancelActive()
    {
        if (activeCancellation) {
            activeCancellation->store(true, std::memory_order_relaxed);
        }
    }

    void clear()
    {
        QMutexLocker lock(&mutex);
        cancelActive();
        desired.clear();
        entries.clear();
        attempted.clear();
        bytes = 0;
    }

    void cancelForNavigation()
    {
        QMutexLocker lock(&mutex);
        cancelActive();
        // Keep the overlapping ready entries until the new center is known.
        desired.clear();
        attempted.clear();
    }

    QImage lookup(const QString& path)
    {
        QMutexLocker lock(&mutex);
        auto it = entries.find(path);
        if (it == entries.end()) {
            return {};
        }
        if (!sameFileStamp(it->stamp, nearbyFileStamp(path))) {
            bytes -= it->image.sizeInBytes();
            entries.erase(it);
            attempted.remove(path);
            return {};
        }
        ++hitCount;
        return it->image;
    }

    bool setWindow(const QList<QUrl>& urls, const QSize& requested, int memoryMiB)
    {
        QStringList paths;
        paths.reserve(std::min<qsizetype>(urls.size(), 10));
        for (const QUrl& url : urls) {
            if (url.isLocalFile() && !paths.contains(url.toLocalFile())) {
                paths.append(url.toLocalFile());
            }
            if (paths.size() == 10) {
                break;
            }
        }
        const qsizetype limit =
            std::min<qsizetype>(96, std::max(0, memoryMiB) / 8) * qsizetype(1024 * 1024);
        // Ten slots at this size cannot retain more than the cache limit.
        const qsizetype perSlot = limit / 10;
        QMutexLocker lock(&mutex);
        if (desired == paths && requestedSize == requested && byteLimit == limit) {
            return !workerRunning && !paths.isEmpty();
        }
        cancelActive();
        desired = std::move(paths);
        requestedSize = requested;
        byteLimit = limit;
        slotLimit = perSlot;
        attempted.clear();
        for (auto it = entries.begin(); it != entries.end();) {
            if (!desired.contains(it.key()) || it->image.sizeInBytes() > slotLimit) {
                bytes -= it->image.sizeInBytes();
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
        return !workerRunning && !desired.isEmpty();
    }

    void run(ImageResourcePolicy& policy, QThreadPool& pool)
    {
        QString path;
        QSize target;
        qsizetype maxBytes = 0;
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        {
            QMutexLocker lock(&mutex);
            for (const QString& candidate : desired) {
                if (!entries.contains(candidate) && !attempted.contains(candidate)) {
                    path = candidate;
                    break;
                }
            }
            if (path.isEmpty()) {
                workerRunning = false;
                activeCancellation.reset();
                return;
            }
            attempted.insert(path);
            target = requestedSize;
            maxBytes = slotLimit;
            activeCancellation = cancellation;
        }

        QImage decoded;
        const NearbyFileStamp before = nearbyFileStamp(path);
        const QString suffix = QFileInfo(path).suffix().toLower();
        const bool raw = suffix == QStringLiteral("dng") || suffix == QStringLiteral("raw") ||
                         suffix == QStringLiteral("cr2") || suffix == QStringLiteral("cr3") ||
                         suffix == QStringLiteral("nef") || suffix == QStringLiteral("nrw") ||
                         suffix == QStringLiteral("arw") || suffix == QStringLiteral("srw") ||
                         suffix == QStringLiteral("rw2") || suffix == QStringLiteral("pef") ||
                         suffix == QStringLiteral("orf") || suffix == QStringLiteral("raf");
        const bool stillOrSmall =
            suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg") ||
            suffix == QStringLiteral("png") || suffix == QStringLiteral("webp") ||
            suffix == QStringLiteral("bmp") || suffix == QStringLiteral("tif") ||
            suffix == QStringLiteral("tiff");
        if ((stillOrSmall || raw) && before.isValid() && maxBytes > 0 && target.isValid()) {
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
            if (suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff")) {
                decoded = readFastTiffPreview(path, target, policy.maximumImagePixels(), maxBytes,
                                              cancellation.get());
            }
#endif
            if (decoded.isNull() && !cancellation->load(std::memory_order_relaxed)) {
                const quint64 maximumPixels =
                    std::min(policy.maximumImagePixels(), quint64(maxBytes) / 4);
                QImageReader reader(path);
                ImageDecodeContract::configure(reader, maximumPixels, cancellation.get(), false,
                                               false, false);
                reader.setAutoTransform(true);
                if (reader.canRead()) {
                    const QSize source = reader.size();
                    const bool jpeg =
                        suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
                    const QSize nativeJpegSize =
                        jpeg ? admittedNativeJpegSize(source, maximumPixels) : QSize();
                    const bool nativeJpegScale =
                        jpeg && nativeJpegSize.isValid() &&
                        reader.supportsOption(QImageIOHandler::ScaledSize) &&
                        ImageDecodeContract::allows(source, policy.maximumImagePixels());
                    const QSize embeddedRawPreview =
                        raw && reader.device()
                            ? reader.device()
                                  ->property(ImageDecodeContract::embeddedPreviewSizeProperty)
                                  .toSize()
                            : QSize();
                    const bool scalableEmbeddedJpeg =
                        raw && reader.device() &&
                        reader.device()
                            ->property(ImageDecodeContract::scalableEmbeddedJpegProperty)
                            .toBool();
                    const bool boundedRawPreview =
                        raw && (ImageDecodeContract::allows(embeddedRawPreview, maximumPixels) ||
                                scalableEmbeddedJpeg);
                    // Only a bounded source or a codec-owned native scale may
                    // run alongside the current full reader.
                    if (boundedRawPreview ||
                        (!raw && source.isValid() &&
                         (ImageDecodeContract::allows(source, maximumPixels) || nativeJpegScale))) {
                        const QSize bounded = boundedPreviewSize(target, maximumPixels);
                        QSize scaled = raw ? embeddedRawPreview : source;
                        scaled.scale(bounded, Qt::KeepAspectRatio);
                        if (nativeJpegScale &&
                            !ImageDecodeContract::allows(source, maximumPixels)) {
                            scaled.scale(nativeJpegSize, Qt::KeepAspectRatio);
                        }
                        if (scaled.isValid() && scaled != (raw ? embeddedRawPreview : source)) {
                            reader.setScaledSize(scaled);
                        }
                        if (!cancellation->load(std::memory_order_relaxed)) {
                            decoded = reader.read();
                            if (!ImageDecodeContract::allows(decoded.size(), maximumPixels)) {
                                decoded = {};
                            }
                        }
                    }
                }
            }
        }
        const NearbyFileStamp after = nearbyFileStamp(path);
        bool more = false;
        {
            QMutexLocker lock(&mutex);
            if (!cancellation->load(std::memory_order_relaxed) && desired.contains(path) &&
                !decoded.isNull() && decoded.sizeInBytes() > 0 &&
                decoded.sizeInBytes() <= slotLimit && sameFileStamp(before, after)) {
                bytes += decoded.sizeInBytes();
                entries.insert(path, {std::move(decoded), after});
            }
            for (const QString& candidate : desired) {
                if (!entries.contains(candidate) && !attempted.contains(candidate)) {
                    more = true;
                    break;
                }
            }
            if (!more) {
                workerRunning = false;
                activeCancellation.reset();
            }
        }
        if (more) {
            pool.start([self = shared_from_this(), &policy, &pool] { self->run(policy, pool); },
                       -1);
        }
    }
};

struct ParallelDecodePair {
    QMutex mutex;
    QWaitCondition previewFinishedCondition;
    std::atomic_bool previewPresented = false;
    int maximumMegapixels = 0;
    bool fullOwnsGate = false;
    bool previewBypassedGate = false;
    bool previewFinished = false;
};

namespace {
constexpr qsizetype kMaximumDecodedBaseCacheBytes = 256 * 1024 * 1024;

bool traceDecodeCache() { return qEnvironmentVariableIsSet("LICASA_TRACE_DECODE_CACHE"); }
QSize boundedPreviewSize(const QSize& requestedSize, quint64 maximumPixels)
{
    if (!requestedSize.isValid() || requestedSize.width() <= 0 || requestedSize.height() <= 0) {
        return {};
    }
    if (ImageDecodeContract::allows(requestedSize, maximumPixels)) {
        return requestedSize;
    }

    const long double requestedPixels =
        static_cast<long double>(requestedSize.width()) * requestedSize.height();
    const long double scale = std::sqrt(static_cast<long double>(maximumPixels) / requestedPixels);
    QSize boundedSize(std::max(1, static_cast<int>(std::floor(requestedSize.width() * scale))),
                      std::max(1, static_cast<int>(std::floor(requestedSize.height() * scale))));

    // Floating-point rounding should already leave the area at or below the
    // budget. Keep the final adjustment integer-only so that invariant is exact.
    while (!ImageDecodeContract::allows(boundedSize, maximumPixels)) {
        if (boundedSize.width() >= boundedSize.height()) {
            boundedSize.rwidth() -= 1;
        } else {
            boundedSize.rheight() -= 1;
        }
    }
    return boundedSize;
}

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
                        std::shared_ptr<std::atomic_bool> cancelled, bool priorityNavigationPreview,
                        std::function<void()> discardFailedPreview)
        : filePath_(std::move(filePath)), requestedSize_(requestedSize),
          editParameters_(std::move(editParameters)), colorManagedRendering_(colorManagedRendering),
          fullDetail_(fullDetail), rawFastDevelopment_(rawFastDevelopment),
          rawInteractiveDevelopment_(rawInteractiveDevelopment),
          editWarmupRequested_(editWarmupRequested), parallelPreview_(parallelPreview),
          speculativeFull_(speculativeFull), parallelPair_(std::move(parallelPair)),
          resourcePolicy_(resourcePolicy), decodedImageCache_(std::move(decodedImageCache)),
          nearbyPreviewCache_(std::move(nearbyPreviewCache)),
          discardFailedPreview_(std::move(discardFailedPreview)), cancelled_(std::move(cancelled)),
          priorityNavigationPreview_(priorityNavigationPreview)
    {
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
            if (!priorityPreviewReady && !isCancelled() && bypassGate) {
                // The paired full reader owns the heavy-operation gate until
                // this preview ends, including its decode-cache publication.
                decodeImage(pairedMaximumMegapixels);
            } else if (!priorityPreviewReady && !isCancelled()) {
                QMutexLocker processingLock(&resourcePolicy_.processingMutex());
                if (!isCancelled()) {
                    decodeImage(resourcePolicy_.prepareForProcessing());
                }
            }
            QMutexLocker pairLock(&parallelPair_->mutex);
            parallelPair_->previewFinished = true;
            if (isCancelled() || !error_.isEmpty()) {
                parallelPair_->previewPresented.store(true, std::memory_order_release);
            }
            parallelPair_->previewFinishedCondition.wakeAll();
        } else if (priorityPreviewReady) {
            // A bounded selected preview can be shown while the cancelled old
            // full reader is still retiring from LibRaw.
        } else if (!isCancelled() && speculativeFull_ && parallelPair_) {
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
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
            QMutexLocker processingLock(&resourcePolicy_.processingMutex());
            if (!isCancelled()) {
                decodeImage(resourcePolicy_.prepareForProcessing());
            }
        }
        // Destroy the reader and release the processing gate before publishing
        // completion: a consumer may start the next heavy operation immediately.
        const bool prewarm = prewarmAfterRawDecode_ && !isCancelled() &&
                             !qEnvironmentVariableIsSet("LICASA_DISABLE_RAW_EDIT_PREWARM");
        if (discardFailedPreview_ && (isCancelled() || !error_.isEmpty())) {
            parallelPair_->previewPresented.store(true, std::memory_order_release);
            discardFailedPreview_();
        }
        emit finished();
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
    }

  private:
    bool tryPriorityPreview()
    {
        // Prepared neighbors are already charged to the bounded nearby cache.
        // Reusing one needs no decoder or heavy-operation gate.
        QImage preparedNeighbor = nearbyPreviewCache_->lookup(filePath_);
        if (!preparedNeighbor.isNull()) {
            decodeImage(resourcePolicy_.maximumImageMegapixels(), std::move(preparedNeighbor));
            return !image_.isNull();
        }

        constexpr int previewMegapixels = 2;
        constexpr quint64 previewPixels = quint64(previewMegapixels) * 1000000;
        const QString suffix = QFileInfo(filePath_).suffix().toLower();
        const bool jpeg = suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
        const bool smallRaster = suffix == QStringLiteral("png") ||
                                 suffix == QStringLiteral("bmp") ||
                                 suffix == QStringLiteral("webp");
        const bool raw = suffix == QStringLiteral("dng") || suffix == QStringLiteral("cr2") ||
                         suffix == QStringLiteral("cr3") || suffix == QStringLiteral("nef") ||
                         suffix == QStringLiteral("arw") || suffix == QStringLiteral("rw2");
#ifdef LICASA_HAVE_FAST_TIFF_PREVIEW
        const bool tiff = suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff");
        if (tiff) {
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
            if (suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff")) {
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

#ifdef LICASA_EARLY_PROGRESSIVE_JPEG
            const QString extension = fileInfo.suffix().toLower();
            if (!fullDetail_ && !rawFastDevelopment_ && !rawInteractiveDevelopment_ &&
                (extension == QStringLiteral("jpg") || extension == QStringLiteral("jpeg")) &&
                reader.scaledSize().isValid() &&
                reader.transformation() == QImageIOHandler::TransformationNone) {
                image = readEarlyProgressiveJpegPreview(filePath_, reader.scaledSize(),
                                                        maximumPixels, cancelled_.get());
            }
#endif
            if (image.isNull()) {
                image = reader.read();
            }
            if (isCancelled()) {
                return;
            }
            if (image.isNull()) {
                error_ = ImageProcessing::readerErrorOrFallback(reader);
                return;
            }
            if (!ImageDecodeContract::allows(image.size(), maximumPixels)) {
                image = {};
                error_ = ImageProcessing::imageLimitError(maximumMegapixels);
                return;
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
    std::function<void()> discardFailedPreview_;
    QImage image_;
    QString error_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    bool priorityNavigationPreview_ = false;
};

} // namespace

AsyncImageProvider::AsyncImageProvider(ImageResourcePolicy& resourcePolicy)
    : resourcePolicy_(resourcePolicy), decodedImageCache_(std::make_shared<DecodedImageCache>()),
      nearbyPreviewCache_(std::make_shared<NearbyPreviewCache>())
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
        nextNavigationPreview_ = retainNearbyPreviews;
        nextNavigationPath_ =
            retainNearbyPreviews && nextImage.isLocalFile() ? nextImage.toLocalFile() : QString();
    }
    if (retainNearbyPreviews) {
        nearbyPreviewCache_->cancelForNavigation();
    } else {
        nearbyPreviewCache_->clear();
    }
    {
        QMutexLocker pairLock(&pairsMutex_);
        pendingPairs_.clear();
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
        {
            QMutexLocker lock(&nearbyPreviewCache_->mutex);
            nearbyPreviewCache_->workerRunning = true;
        }
        nearbyPreviewPool_.start([cache = nearbyPreviewCache_,
                                  this] { cache->run(resourcePolicy_, nearbyPreviewPool_); },
                                 -1);
    }
}

qsizetype AsyncImageProvider::nearbyPreviewCount() const
{
    QMutexLocker lock(&nearbyPreviewCache_->mutex);
    return nearbyPreviewCache_->entries.size();
}

qsizetype AsyncImageProvider::nearbyPreviewBytes() const
{
    QMutexLocker lock(&nearbyPreviewCache_->mutex);
    return nearbyPreviewCache_->bytes;
}

quint64 AsyncImageProvider::nearbyPreviewHitCount() const
{
    QMutexLocker lock(&nearbyPreviewCache_->mutex);
    return nearbyPreviewCache_->hitCount;
}

bool AsyncImageProvider::hasNearbyPreview(const QUrl& url) const
{
    return url.isLocalFile() && !nearbyPreviewCache_->lookup(url.toLocalFile()).isNull();
}

void AsyncImageProvider::markParallelPreviewPresented(const QString& pairId)
{
    std::shared_ptr<ParallelDecodePair> pair;
    {
        QMutexLocker pairLock(&pairsMutex_);
        pair = pendingPairs_.take(pairId);
    }
    if (pair) {
        pair->previewPresented.store(true, std::memory_order_release);
    }
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
    if (!pairId.isEmpty() && (parallelPreview || speculativeFull)) {
        QMutexLocker pairLock(&pairsMutex_);
        if (speculativeFull) {
            pair = std::make_shared<ParallelDecodePair>();
            pendingPairs_.insert(pairId, pair);
        } else {
            pair = pendingPairs_.value(pairId);
        }
    }
    bool warmupOk = false;
    const int editWarmupRevision = query.queryItemValue(QStringLiteral("ew")).toInt(&warmupOk);
    const bool editWarmupRequested = warmupOk && editWarmupRevision > 0;
    std::function<void()> discardFailedPreview;
    if (parallelPreview && pair) {
        // A failed or cancelled preview never reaches the frame-swapped signal.
        // Remove its registry entry so repeated attempts cannot retain pairs.
        // The provider drains both worker pools before its members are destroyed.
        discardFailedPreview = [this, pairId, pair] {
            QMutexLocker pairLock(&pairsMutex_);
            if (pendingPairs_.value(pairId) == pair) {
                pendingPairs_.remove(pairId);
            }
        };
    }
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    bool navigationPriority = false;
    {
        QMutexLocker requestsLock(&requestsMutex_);
        activeRequests_.erase(std::remove_if(activeRequests_.begin(), activeRequests_.end(),
                                             [](const auto& request) { return request.expired(); }),
                              activeRequests_.end());
        activeRequests_.push_back(cancelled);
        const bool selectedImage = nextNavigationPath_.isEmpty() || filePath == nextNavigationPath_;
        if (!fullDetail && nextNavigationPreview_ && selectedImage) {
            navigationPriority = true;
            nextNavigationPreview_ = false;
        } else if (fullDetail && !speculativeFull && selectedImage) {
            // A direct full request is the selected image. Do not save the
            // one-shot preview slot for an unrelated later edit request.
            nextNavigationPreview_ = false;
        }
    }
    auto* response = new AsyncDecodeResponse(
        filePath, requestedSize, editParametersFromQuery(query), colorManagedRendering, fullDetail,
        rawFastDevelopment, rawInteractiveDevelopment, editWarmupRequested,
        parallelPreview && bool(pair), speculativeFull && bool(pair), pair, resourcePolicy_,
        decodedImageCache_, nearbyPreviewCache_, std::move(cancelled), navigationPriority,
        std::move(discardFailedPreview));
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
