#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <atomic>

namespace Licasa {

class FormatSupport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList nameFilters READ nameFilters CONSTANT)
    Q_PROPERTY(QStringList saveNameFilters READ saveNameFilters CONSTANT)
    Q_PROPERTY(QVariantList exportFormats READ exportFormats CONSTANT)

  public:
    explicit FormatSupport(QObject* parent = nullptr);
    ~FormatSupport() override;

    QStringList nameFilters() const;
    QStringList saveNameFilters() const;
    QVariantList exportFormats() const;
    Q_INVOKABLE bool canOpen(const QUrl& url) const;
    Q_INVOKABLE void prepareDirectoryIndex(const QUrl& current);
    Q_INVOKABLE bool hasDirectoryIndex(const QUrl& current) const;
    Q_INVOKABLE QUrl adjacentImage(const QUrl& current, int direction) const;
    // The default order interleaves directions; browsing can favor its travel direction.
    Q_INVOKABLE QList<QUrl> nearbyImages(const QUrl& current, int radius) const;
    Q_INVOKABLE QList<QUrl> nearbyImages(const QUrl& current, int radius,
                                         int preferredDirection) const;

  signals:
    void directoryIndexReady();

  private:
    const QStringList& imagesInDirectory(const QString& directory) const;
    QList<QUrl> nearbyImagesImpl(const QUrl& current, int radius, int preferredDirection,
                                 bool deferWhileIndexing) const;

    QSet<QString> readableExtensions_;
    QStringList nameFilters_;
    QStringList saveNameFilters_;
    mutable QString indexedDirectory_;
    mutable QDateTime indexedDirectoryModified_;
    mutable QStringList indexedNames_;
    QThreadPool indexPool_;
    std::atomic<quint64> indexGeneration_{0};
    QString preparingDirectory_;
    QDateTime preparingDirectoryModified_;
};

} // namespace Licasa
