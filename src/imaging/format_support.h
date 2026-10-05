#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>

namespace Licasa {

class FormatSupport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList nameFilters READ nameFilters CONSTANT)
    Q_PROPERTY(QStringList saveNameFilters READ saveNameFilters CONSTANT)

  public:
    explicit FormatSupport(QObject* parent = nullptr);

    QStringList nameFilters() const;
    QStringList saveNameFilters() const;
    Q_INVOKABLE bool canOpen(const QUrl& url) const;
    Q_INVOKABLE QUrl adjacentImage(const QUrl& current, int direction) const;
    // The default order interleaves directions; browsing can favor its travel direction.
    Q_INVOKABLE QList<QUrl> nearbyImages(const QUrl& current, int radius) const;
    Q_INVOKABLE QList<QUrl> nearbyImages(const QUrl& current, int radius,
                                         int preferredDirection) const;

  private:
    const QStringList& imagesInDirectory(const QString& directory) const;

    QSet<QString> readableExtensions_;
    QStringList nameFilters_;
    QStringList saveNameFilters_;
    mutable QString indexedDirectory_;
    mutable QDateTime indexedDirectoryModified_;
    mutable QStringList indexedNames_;
};

} // namespace Licasa
