#include "imaging/format_support.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QSet>
#include <algorithm>

namespace Licasa {
FormatSupport::FormatSupport(QObject* parent) : QObject(parent)
{
    QSet<QString> patterns;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();

    for (const QByteArray& formatBytes : formats) {
        const QString format = QString::fromLatin1(formatBytes).trimmed().toLower();
        if (format.isEmpty()) {
            continue;
        }

        const auto addExtension = [this, &patterns](const QString& extension) {
            readableExtensions_.insert(extension);
            patterns.insert(QStringLiteral("*.") + extension);
        };

        if (format == QStringLiteral("jpeg") || format == QStringLiteral("jpg")) {
            addExtension(QStringLiteral("jpg"));
            addExtension(QStringLiteral("jpeg"));
        } else if (format == QStringLiteral("tif") || format == QStringLiteral("tiff")) {
            addExtension(QStringLiteral("tif"));
            addExtension(QStringLiteral("tiff"));
        } else if (format == QStringLiteral("svg") || format == QStringLiteral("svgz")) {
            addExtension(QStringLiteral("svg"));
            addExtension(QStringLiteral("svgz"));
        } else {
            addExtension(format);
        }
    }

    QStringList allPatterns = patterns.values();
    std::sort(allPatterns.begin(), allPatterns.end(),
              [](const QString& left, const QString& right) {
                  return left.localeAwareCompare(right) < 0;
              });

    if (!allPatterns.isEmpty()) {
        nameFilters_ << QStringLiteral("Images (%1)").arg(allPatterns.join(' '));
    }
    nameFilters_ << QStringLiteral("All Files (*)");

    QSet<QString> writablePatterns;
    const QList<QByteArray> writableFormats = QImageWriter::supportedImageFormats();
    for (QByteArray formatBytes : writableFormats) {
        const QString format = QString::fromLatin1(formatBytes).trimmed().toLower();
        if (format == QStringLiteral("jpeg") || format == QStringLiteral("jpg")) {
            writablePatterns.insert(QStringLiteral("*.jpg"));
            writablePatterns.insert(QStringLiteral("*.jpeg"));
        } else if (!format.isEmpty()) {
            writablePatterns.insert(QStringLiteral("*.") + format);
        }
    }

    const auto hasWritablePattern = [&writablePatterns](const QString& pattern) {
        return writablePatterns.contains(pattern);
    };
    if (hasWritablePattern(QStringLiteral("*.png"))) {
        saveNameFilters_ << QStringLiteral("PNG image (*.png)");
    }
    if (hasWritablePattern(QStringLiteral("*.jpg"))) {
        saveNameFilters_ << QStringLiteral("JPEG image (*.jpg *.jpeg)");
    }
    if (hasWritablePattern(QStringLiteral("*.webp"))) {
        saveNameFilters_ << QStringLiteral("WebP image (*.webp)");
    }

    QStringList allWritablePatterns = writablePatterns.values();
    std::sort(allWritablePatterns.begin(), allWritablePatterns.end(),
              [](const QString& left, const QString& right) {
                  return left.localeAwareCompare(right) < 0;
              });
    if (!allWritablePatterns.isEmpty()) {
        saveNameFilters_
            << QStringLiteral("All writable images (%1)").arg(allWritablePatterns.join(' '));
    }
}

QStringList FormatSupport::nameFilters() const { return nameFilters_; }

QStringList FormatSupport::saveNameFilters() const { return saveNameFilters_; }

bool FormatSupport::canOpen(const QUrl& url) const
{
    if (!url.isValid() || !url.isLocalFile()) {
        return false;
    }

    const QFileInfo fileInfo(url.toLocalFile());
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return false;
    }

    const QString suffix = fileInfo.suffix().trimmed().toLower();
    if (!suffix.isEmpty() && readableExtensions_.contains(suffix)) {
        // Known registered formats are admitted by extension. Calling
        // QImageReader::canRead() here can execute a native decoder's metadata
        // path synchronously on the UI thread; camera RAW plugins in particular
        // may open LibRaw before the asynchronous provider has even been queued.
        return true;
    }

    // Preserve best-effort support for extensionless or mislabeled images. The
    // potentially expensive decoder probe is used only when the cheap registry
    // admission above cannot decide.
    QImageReader reader(fileInfo.absoluteFilePath());
    return reader.canRead();
}

const QStringList& FormatSupport::imagesInDirectory(const QString& directory) const
{
    const QDateTime modified = QFileInfo(directory).lastModified();
    if (indexedDirectory_ == directory && indexedDirectoryModified_ == modified) {
        return indexedNames_;
    }

    indexedDirectory_ = directory;
    indexedDirectoryModified_ = modified;
    indexedNames_.clear();
    QDirIterator entries(directory, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot);
    while (entries.hasNext()) {
        entries.next();
        const QFileInfo entry = entries.fileInfo();
        if (readableExtensions_.contains(entry.suffix().toLower())) {
            indexedNames_.append(entry.fileName());
        }
    }
    const auto less = [](const QString& left, const QString& right) {
        const int comparison = left.localeAwareCompare(right);
        return comparison < 0 || (comparison == 0 && left < right);
    };
    std::sort(indexedNames_.begin(), indexedNames_.end(), less);
    return indexedNames_;
}

QList<QUrl> FormatSupport::nearbyImages(const QUrl& current, int radius) const
{
    return nearbyImages(current, radius, 0);
}

QList<QUrl> FormatSupport::nearbyImages(const QUrl& current, int radius,
                                        int preferredDirection) const
{
    if (!current.isLocalFile() || radius <= 0) {
        return {};
    }

    const QFileInfo currentFile(current.toLocalFile());
    if (!currentFile.isFile()) {
        return {};
    }

    const QString directory = currentFile.absolutePath();
    const QStringList& indexed = imagesInDirectory(directory);
    const auto less = [](const QString& left, const QString& right) {
        const int comparison = left.localeAwareCompare(right);
        return comparison < 0 || (comparison == 0 && left < right);
    };
    const QString currentName = currentFile.fileName();
    QStringList additional;
    const QStringList* names = &indexed;
    auto currentIt = std::lower_bound(indexed.cbegin(), indexed.cend(), currentName, less);
    if (currentIt == indexed.cend() || *currentIt != currentName) {
        if (!canOpen(current)) {
            return {};
        }
        additional = indexed;
        additional.insert(int(currentIt - indexed.cbegin()), currentName);
        names = &additional;
        currentIt = names->cbegin() + (currentIt - indexed.cbegin());
    }

    const int count = names->size();
    if (count < 2) {
        return {};
    }
    const int index = int(currentIt - names->cbegin());
    QList<QUrl> nearby;
    nearby.reserve(int(std::min<qint64>(count - 1, qint64(radius) * 2)));
    const auto appendNeighbor = [&](int distance, int direction) {
        const int neighbor =
            direction > 0 ? (index + distance) % count : (index - distance + count) % count;
        const QUrl url = QUrl::fromLocalFile(QDir(directory).absoluteFilePath(names->at(neighbor)));
        if (!nearby.contains(url)) {
            nearby.append(url);
        }
    };
    if (preferredDirection != 0) {
        for (const int direction :
             {preferredDirection > 0 ? 1 : -1, preferredDirection > 0 ? -1 : 1}) {
            for (int distance = 1; distance <= radius && nearby.size() < count - 1; ++distance) {
                appendNeighbor(distance, direction);
            }
        }
    } else {
        for (int distance = 1; distance <= radius && nearby.size() < count - 1; ++distance) {
            for (const int direction : {1, -1}) {
                appendNeighbor(distance, direction);
            }
        }
    }
    return nearby;
}

QUrl FormatSupport::adjacentImage(const QUrl& current, int direction) const
{
    if (direction == 0) {
        return {};
    }
    const QList<QUrl> nearby = nearbyImages(current, 1);
    if (nearby.isEmpty()) {
        return {};
    }
    return direction > 0 || nearby.size() == 1 ? nearby.first() : nearby.at(1);
}

} // namespace Licasa
