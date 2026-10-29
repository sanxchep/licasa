#include "imaging/format_support.h"
#include "export/image_export_contract.h"
#include <QCollator>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QMetaObject>
#include <QSet>
#include <algorithm>
#include <utility>
#include <vector>

namespace Licasa {
namespace {
bool lessFileName(const QString& left, const QString& right)
{
    const int comparison = left.localeAwareCompare(right);
    return comparison < 0 || (comparison == 0 && left < right);
}

void sortImageNames(QStringList& names, const std::atomic<quint64>* generation,
                    quint64 expectedGeneration)
{
    const QCollator collator;
    // The C locale has no reusable collation keys. Small directories also do
    // too little comparison work to repay allocating one key per name.
    if (names.size() < 64 || collator.locale().language() == QLocale::C) {
        std::sort(names.begin(), names.end(), lessFileName);
        return;
    }

    struct KeyedName {
        QString name;
        QCollatorSortKey key;
    };
    std::vector<KeyedName> keyed;
    keyed.reserve(size_t(names.size()));
    for (const QString& name : std::as_const(names)) {
        if (generation && generation->load(std::memory_order_relaxed) != expectedGeneration) {
            names.clear();
            return;
        }
        keyed.push_back({name, collator.sortKey(name)});
    }
    std::sort(keyed.begin(), keyed.end(), [](const KeyedName& left, const KeyedName& right) {
        const int comparison = left.key.compare(right.key);
        return comparison < 0 || (comparison == 0 && left.name < right.name);
    });
    for (qsizetype index = 0; index < names.size(); ++index) {
        names[index] = std::move(keyed[size_t(index)].name);
    }
}

QStringList scanImageNames(const QString& directory, const QSet<QString>& extensions,
                           const std::atomic<quint64>* generation = nullptr,
                           quint64 expectedGeneration = 0)
{
    QStringList names;
    QDirIterator entries(directory, QDir::Files | QDir::NoDotAndDotDot);
    while (entries.hasNext()) {
        if (generation && generation->load(std::memory_order_relaxed) != expectedGeneration) {
            return {};
        }
        entries.next();
        const QFileInfo entry = entries.fileInfo();
        // Only supported candidates need a readability stat. Mixed folders can
        // contain many unrelated files; checking them cannot admit a photo.
        if (extensions.contains(entry.suffix().toLower()) && entry.isReadable()) {
            names.append(entry.fileName());
        }
    }
    sortImageNames(names, generation, expectedGeneration);
    return names;
}
} // namespace

FormatSupport::FormatSupport(QObject* parent) : QObject(parent)
{
    indexPool_.setMaxThreadCount(1);
    indexPool_.setExpiryTimeout(1000);
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

    for (const auto& entry : ImageExport::formats()) {
        const auto item = entry.toMap();
        const QString key = item.value(QStringLiteral("key")).toString();
        QString extensions = QStringLiteral("*.") + item.value(QStringLiteral("suffix")).toString();
        if (key == QStringLiteral("jpeg")) {
            extensions += QStringLiteral(" *.jpeg");
        }
        if (key == QStringLiteral("tiff")) {
            extensions += QStringLiteral(" *.tif");
        }
        saveNameFilters_ << item.value(QStringLiteral("label")).toString() +
                                QStringLiteral(" image (") + extensions + ')';
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

FormatSupport::~FormatSupport()
{
    indexGeneration_.fetch_add(1, std::memory_order_relaxed);
    indexPool_.clear();
    indexPool_.waitForDone();
}

QStringList FormatSupport::nameFilters() const { return nameFilters_; }

QVariantList FormatSupport::exportFormats() const { return ImageExport::formats(); }

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

void FormatSupport::prepareDirectoryIndex(const QUrl& current)
{
    if (!current.isLocalFile()) {
        return;
    }
    const QFileInfo file(current.toLocalFile());
    if (!file.isFile()) {
        return;
    }
    const QString directory = file.absolutePath();
    const QDateTime modified = QFileInfo(directory).lastModified();
    if ((indexedDirectory_ == directory && indexedDirectoryModified_ == modified) ||
        (preparingDirectory_ == directory && preparingDirectoryModified_ == modified)) {
        return;
    }

    preparingDirectory_ = directory;
    preparingDirectoryModified_ = modified;
    const quint64 generation = indexGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    indexPool_.clear();
    const QSet<QString> extensions = readableExtensions_;
    indexPool_.start([this, current, directory, modified, extensions, generation] {
        const bool trace = qEnvironmentVariableIsSet("LICASA_TRACE_DIRECTORY_INDEX");
        QElapsedTimer elapsed;
        if (trace) {
            elapsed.start();
        }
        QStringList names = scanImageNames(directory, extensions, &indexGeneration_, generation);
        if (indexGeneration_.load(std::memory_order_relaxed) != generation) {
            return;
        }
        if (trace) {
            qInfo().noquote() << "[Licasa] Background directory index:" << names.size()
                              << "images in" << elapsed.elapsed() << "ms";
        }
        QMetaObject::invokeMethod(
            this,
            [this, current, directory, modified, generation, names = std::move(names)]() mutable {
                if (indexGeneration_.load(std::memory_order_relaxed) != generation) {
                    return;
                }
                preparingDirectory_.clear();
                preparingDirectoryModified_ = {};
                if (QFileInfo(directory).lastModified() != modified) {
                    prepareDirectoryIndex(current);
                    return;
                }
                indexedDirectory_ = directory;
                indexedDirectoryModified_ = modified;
                indexedNames_ = std::move(names);
                emit directoryIndexReady();
            },
            Qt::QueuedConnection);
    });
}

bool FormatSupport::hasDirectoryIndex(const QUrl& current) const
{
    if (!current.isLocalFile()) {
        return false;
    }
    const QString directory = QFileInfo(current.toLocalFile()).absolutePath();
    return indexedDirectory_ == directory &&
           indexedDirectoryModified_ == QFileInfo(directory).lastModified();
}

const QStringList& FormatSupport::imagesInDirectory(const QString& directory) const
{
    const QDateTime modified = QFileInfo(directory).lastModified();
    if (indexedDirectory_ == directory && indexedDirectoryModified_ == modified) {
        return indexedNames_;
    }

    indexedDirectory_ = directory;
    indexedDirectoryModified_ = modified;
    const bool trace = qEnvironmentVariableIsSet("LICASA_TRACE_DIRECTORY_INDEX");
    QElapsedTimer elapsed;
    if (trace) {
        elapsed.start();
    }
    indexedNames_ = scanImageNames(directory, readableExtensions_);
    if (trace) {
        qInfo().noquote() << "[Licasa] Synchronous directory index:" << indexedNames_.size()
                          << "images in" << elapsed.elapsed() << "ms";
    }
    return indexedNames_;
}

QList<QUrl> FormatSupport::nearbyImages(const QUrl& current, int radius) const
{
    return nearbyImages(current, radius, 0);
}

QList<QUrl> FormatSupport::nearbyImages(const QUrl& current, int radius,
                                        int preferredDirection) const
{
    return nearbyImagesImpl(current, radius, preferredDirection, true);
}

QList<QUrl> FormatSupport::nearbyImagesImpl(const QUrl& current, int radius, int preferredDirection,
                                            bool deferWhileIndexing) const
{
    if (!current.isLocalFile() || radius <= 0) {
        return {};
    }

    const QFileInfo currentFile(current.toLocalFile());
    if (!currentFile.isFile()) {
        return {};
    }

    const QString directory = currentFile.absolutePath();
    if (deferWhileIndexing && directory == preparingDirectory_ &&
        QFileInfo(directory).lastModified() == preparingDirectoryModified_ &&
        (indexedDirectory_ != directory ||
         indexedDirectoryModified_ != preparingDirectoryModified_)) {
        return {};
    }
    const QStringList& indexed = imagesInDirectory(directory);
    const QString currentName = currentFile.fileName();
    const auto currentIt =
        std::lower_bound(indexed.cbegin(), indexed.cend(), currentName, lessFileName);
    const bool currentIndexed = currentIt != indexed.cend() && *currentIt == currentName;
    if (!currentIndexed) {
        if (!canOpen(current)) {
            return {};
        }
    }

    // An extensionless or mislabeled current photo belongs at this sorted
    // position, but choosing a few neighbors need not copy the entire index.
    const qsizetype index = currentIt - indexed.cbegin();
    const qsizetype count = indexed.size() + (currentIndexed ? 0 : 1);
    if (count < 2) {
        return {};
    }
    QList<QUrl> nearby;
    nearby.reserve(std::min<qint64>(count - 1, qint64(radius) * 2));
    const auto appendNeighbor = [&](int distance, int direction) {
        const qsizetype neighbor =
            direction > 0 ? (index + distance) % count : (index - distance + count) % count;
        const qsizetype storedIndex = neighbor - (!currentIndexed && neighbor > index ? 1 : 0);
        const QUrl url =
            QUrl::fromLocalFile(QDir(directory).absoluteFilePath(indexed.at(storedIndex)));
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
    const QList<QUrl> nearby = nearbyImagesImpl(current, 1, 0, false);
    if (nearby.isEmpty()) {
        return {};
    }
    return direction > 0 || nearby.size() == 1 ? nearby.first() : nearby.at(1);
}

} // namespace Licasa
