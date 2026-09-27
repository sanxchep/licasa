#pragma once

#include "media/photo_asset_probe.h"

#include <QObject>
#include <QUrl>

#include <memory>

class QThreadPool;

namespace Licasa {

struct MotionPhotoExportResult {
    QUrl destinationUrl;
    QString errorMessage;
};

// Native byte-copy primitive for an already-validated Motion Photo authority.
// It writes only the validated motion component, never the still/container
// bytes surrounding an embedded video payload. The destination is committed
// atomically and the source remains read-only throughout the operation.
MotionPhotoExportResult copyMotionPhotoComponent(const PhotoAssetInfo& asset,
                                                 const QUrl& destinationUrl);

// Per-window export bridge. setAsset()/clearAsset() are C++-only authority
// handoffs from PhotoAssetProbe. QML will eventually receive only capability,
// busy state and the destination action; it never receives source paths, byte
// ranges, MIME metadata or media-backend objects.
class MotionPhotoExportService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString suggestedSuffix READ suggestedSuffix NOTIFY suggestedSuffixChanged)

  public:
    explicit MotionPhotoExportService(QObject* parent = nullptr);
    ~MotionPhotoExportService() override;

    void setAsset(const PhotoAssetInfo& asset);
    void clearAsset();

    bool available() const noexcept { return available_; }
    bool busy() const noexcept { return busy_; }
    QString suggestedSuffix() const { return suggestedSuffix_; }

    Q_INVOKABLE bool exportCopy(const QUrl& destinationUrl);

  signals:
    void availableChanged();
    void busyChanged();
    void suggestedSuffixChanged();
    void exportCompleted(const QUrl& sourceUrl, const QUrl& destinationUrl);
    void exportFailed(const QUrl& sourceUrl, const QString& message);

  private:
    QThreadPool& workerPool();
    void finishExport(const QUrl& sourceUrl, const MotionPhotoExportResult& result);

    std::unique_ptr<QThreadPool> pool_;
    QUrl sourceUrl_;
    QString suggestedSuffix_;
    bool available_ = false;
    bool busy_ = false;
};

} // namespace Licasa
