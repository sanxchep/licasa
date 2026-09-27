#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class QThreadPool;

namespace Licasa {

class ImageResourcePolicy;

struct AnimationExportResult {
    QUrl destinationUrl;
    QString errorMessage;
};

// Byte-identical animation-copy primitive. The source is revalidated as an
// animated image at the export boundary, copied from a pinned open descriptor,
// and committed atomically. No frame decode/transcode output is generated.
AnimationExportResult copyAnimationFile(ImageResourcePolicy& resourcePolicy, const QUrl& sourceUrl,
                                        const QUrl& destinationUrl);

// Shared async bridge for animation-copy export. The source URL is an ordinary
// viewer asset path already known to QML; animation authority is revalidated in
// C++ on the worker immediately before any destination is committed.
class AnimationExportService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

  public:
    explicit AnimationExportService(ImageResourcePolicy& resourcePolicy, QObject* parent = nullptr);
    ~AnimationExportService() override;

    bool busy() const noexcept { return busy_; }

    Q_INVOKABLE bool exportCopy(const QUrl& sourceUrl, const QUrl& destinationUrl);

  signals:
    void busyChanged();
    void exportCompleted(const QUrl& sourceUrl, const QUrl& destinationUrl);
    void exportFailed(const QUrl& sourceUrl, const QString& message);

  private:
    QThreadPool& workerPool();
    void finishExport(const QUrl& sourceUrl, const AnimationExportResult& result);

    std::unique_ptr<QThreadPool> pool_;
    ImageResourcePolicy& resourcePolicy_;
    bool busy_ = false;
};

} // namespace Licasa
