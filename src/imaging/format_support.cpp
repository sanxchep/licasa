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

QUrl FormatSupport::adjacentImage(const QUrl& current, int direction) const
{
    if (!current.isLocalFile() || direction == 0) {
        return {};
    }

    const QFileInfo currentFile(current.toLocalFile());
    const QDir folder(currentFile.absolutePath());
    const QString currentName = currentFile.fileName();
    const auto less = [](const QString& left, const QString& right) {
        const int comparison = left.localeAwareCompare(right);
        return comparison < 0 || (comparison == 0 && left < right);
    };
    QString first, last, before, after;
    bool foundCurrent = false;
    int count = 0;
    QDirIterator entries(folder.absolutePath(),
                         QDir::Files | QDir::Readable | QDir::NoDotAndDotDot);
    while (entries.hasNext()) {
        entries.next();
        const QFileInfo entry = entries.fileInfo();
        const QString name = entry.fileName();
        if (!readableExtensions_.contains(entry.suffix().toLower()) && name != currentName) {
            continue;
        }
        foundCurrent = foundCurrent || name == currentName;
        ++count;
        if (first.isEmpty() || less(name, first)) {
            first = name;
        }
        if (last.isEmpty() || less(last, name)) {
            last = name;
        }
        if (less(name, currentName) && (before.isEmpty() || less(before, name))) {
            before = name;
        }
        if (less(currentName, name) && (after.isEmpty() || less(name, after))) {
            after = name;
        }
    }

    if (!foundCurrent || count < 2) {
        return {};
    }
    const QString next =
        direction > 0 ? (after.isEmpty() ? first : after) : (before.isEmpty() ? last : before);
    return QUrl::fromLocalFile(folder.absoluteFilePath(next));
}

} // namespace Licasa
