#pragma once

#include <QHash>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <QUrl>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace Licasa {

class ImageResourcePolicy;
struct DecodedImageCache;
struct ParallelDecodePair;
struct NearbyPreviewCache;

class AsyncImageProvider final : public QQuickAsyncImageProvider {
  public:
    explicit AsyncImageProvider(ImageResourcePolicy& resourcePolicy);
    ~AsyncImageProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;
    // Timed image frames use the same single decode worker as still images.
    void enqueueWork(std::function<void()> work);
    void releasePictureResources(bool retainNearbyPreviews = false, const QUrl& nextImage = QUrl());
    void prepareNearbyPreviews(const QList<QUrl>& urls, const QSize& requestedSize,
                               int maximumImageMemoryMiB);
    qsizetype nearbyPreviewCount() const;
    qsizetype nearbyPreviewBytes() const;
    quint64 nearbyPreviewHitCount() const;
    bool hasNearbyPreview(const QUrl& url) const;
    void markParallelPreviewPresented(const QString& pairId);
    // Qt may retain image providers past QQmlEngine teardown. Drain every
    // reader before the referenced ImageResourcePolicy leaves scope.
    void waitForPendingWork();

  private:
    QThreadPool pool_;
    QThreadPool navigationPreviewPool_;
    QThreadPool nearbyPreviewPool_;
    QThreadPool speculativeFullPool_;
    QThreadPool parallelPreviewPool_;
    ImageResourcePolicy& resourcePolicy_;
    std::shared_ptr<DecodedImageCache> decodedImageCache_;
    std::shared_ptr<NearbyPreviewCache> nearbyPreviewCache_;
    QMutex pairsMutex_;
    QHash<QString, std::shared_ptr<ParallelDecodePair>> pendingPairs_;
    QMutex requestsMutex_;
    std::vector<std::weak_ptr<std::atomic_bool>> activeRequests_;
    bool nextNavigationPreview_ = false;
    QString nextNavigationPath_;
};

} // namespace Licasa
