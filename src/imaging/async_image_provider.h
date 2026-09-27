#pragma once

#include <QHash>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <functional>
#include <memory>

namespace Licasa {

class ImageResourcePolicy;
struct DecodedImageCache;
struct ParallelDecodePair;

class AsyncImageProvider final : public QQuickAsyncImageProvider {
  public:
    explicit AsyncImageProvider(ImageResourcePolicy& resourcePolicy);
    ~AsyncImageProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;
    // Timed image frames use the same single decode worker as still images.
    void enqueueWork(std::function<void()> work);
    void releasePictureResources();
    void markParallelPreviewPresented(const QString& pairId);
    // Qt may retain image providers past QQmlEngine teardown. Drain every
    // reader before the referenced ImageResourcePolicy leaves scope.
    void waitForPendingWork();

  private:
    QThreadPool pool_;
    QThreadPool speculativeFullPool_;
    QThreadPool parallelPreviewPool_;
    ImageResourcePolicy& resourcePolicy_;
    std::shared_ptr<DecodedImageCache> decodedImageCache_;
    QMutex pairsMutex_;
    QHash<QString, std::shared_ptr<ParallelDecodePair>> pendingPairs_;
};

} // namespace Licasa
