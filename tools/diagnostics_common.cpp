#include "diagnostics_common.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>

#include <chrono>
#include <climits>
#include <cstdlib>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <sys/resource.h>
#include <dirent.h>
#include <unistd.h>

namespace LicasaDiagnostics {
qint64 monotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Open descriptors by kind; sockets, sync files and DMA buffers are the
// compositor and GPU resources a lifecycle must retire.
QJsonObject descriptorKinds(const QString& photo)
{
    QJsonObject kinds;
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return kinds;
    const int own = dirfd(directory);
    int total = 0;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.' || std::atoi(entry->d_name) == own) continue;
        char target[PATH_MAX];
        const QByteArray link = QByteArrayLiteral("/proc/self/fd/") + entry->d_name;
        const ssize_t length = readlink(link.constData(), target, sizeof target - 1);
        if (length <= 0) continue;
        const QString path = QString::fromLocal8Bit(target, int(length));
        QString kind = QStringLiteral("file");
        if (path.startsWith(QLatin1String("socket:"))) kind = QStringLiteral("socket");
        else if (path.startsWith(QLatin1String("pipe:"))) kind = QStringLiteral("pipe");
        else if (path.startsWith(QLatin1String("anon_inode:")))
            kind = path.mid(11).remove(QLatin1Char('[')).remove(QLatin1Char(']'));
        else if (path.startsWith(QLatin1String("/dev/dri/"))) kind = QStringLiteral("drm");
        else if (path.startsWith(QLatin1String("/dmabuf:"))) kind = QStringLiteral("dmabuf");
        else if (path.startsWith(QLatin1String("/memfd:"))) kind = QStringLiteral("memfd");
        else if (path == photo) kind = QStringLiteral("photo");
        else if (path.startsWith(QLatin1String("/dev/"))) kind = QStringLiteral("device");
        kinds[kind] = kinds.value(kind).toInt() + 1;
        ++total;
    }
    closedir(directory);
    kinds[QStringLiteral("total")] = total;
    return kinds;
}


QJsonObject processMetrics()
{
    QJsonObject result;
    QJsonObject threadNames;
    const QDir tasks(QStringLiteral("/proc/self/task"));
    for (const auto& id : tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile name(tasks.filePath(id + QStringLiteral("/comm")));
        if (name.open(QIODevice::ReadOnly)) {
            const QString label = QString::fromLocal8Bit(name.readAll().trimmed());
            threadNames[label] = threadNames.value(label).toInt() + 1;
        }
    }
    result["thread_names"] = threadNames;
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly)) {
        const auto lines = status.readAll().split('\n');
        for (const auto& line : lines) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 2) {
                continue;
            }
            if (fields[0] == "VmRSS:") {
                result["rss_kib"] = fields[1].toDouble();
            }
            if (fields[0] == "VmHWM:") {
                result["peak_rss_kib"] = fields[1].toDouble();
            }
            if (fields[0] == "Threads:") {
                result["threads"] = fields[1].toInt();
            }
        }
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        result["cpu_ms"] = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
                           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
        result["peak_rss_kib"] = static_cast<double>(usage.ru_maxrss);
    }
#if defined(__GLIBC__)
    // Total RSS is not a reliable object-lifetime metric. glibc may retain
    // completely free worker-arena top chunks after a large Qt/codec operation
    // even after malloc_trim(). Record live allocator bytes separately so the
    // lifecycle gate can distinguish live image ownership from free-but-
    // resident allocator capacity. Active mmap allocations are included.
    const struct mallinfo2 allocator = mallinfo2();
    result["malloc_live_kib"] = static_cast<double>(allocator.uordblks + allocator.hblkhd) / 1024.0;
    result["malloc_arena_kib"] = static_cast<double>(allocator.arena) / 1024.0;
    result["malloc_free_kib"] = static_cast<double>(allocator.fordblks) / 1024.0;
    result["malloc_mmap_kib"] = static_cast<double>(allocator.hblkhd) / 1024.0;
#endif
    QFile maps(QStringLiteral("/proc/self/maps"));
    QJsonArray codecLibraries;
    QJsonArray qtLibraries;
    if (maps.open(QIODevice::ReadOnly)) {
        QSet<QByteArray> paths;
        QSet<QByteArray> qtPaths;
        for (const auto& line : maps.readAll().split('\n')) {
            if (!line.contains('/')) {
                continue;
            }
            const auto path = line.mid(line.indexOf('/'));
            if (path.contains("libQt6")) {
                qtPaths.insert(path);
            }
            if (path.contains("libheif") || path.contains("libde265") || path.contains("libjxl") ||
                path.contains("libraw.") || path.contains("libraw_r.") ||
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
                path.contains("libjpeg.so") ||
#endif
                path.contains("libuhdr.so") || path.contains("libavif") ||
                path.contains("libQt6Multimedia") ||
                path.contains("libffmpegmediaplugin") || path.contains("libavcodec.so") ||
                path.contains("libavformat.so") || path.contains("libavutil.so") ||
                path.contains("libswresample.so") || path.contains("libswscale.so") ||
                path.contains("libOpenCL.so") || path.contains("libcuda.so") ||
                path.contains("libnvrtc")) {
                paths.insert(path);
            }
        }
        for (const auto& path : paths) {
            codecLibraries.append(QString::fromLocal8Bit(path));
        }
        for (const auto& path : qtPaths) {
            qtLibraries.append(QString::fromLocal8Bit(path));
        }
    }
    result["mapped_optional_libraries"] = codecLibraries;
    result["mapped_qt_libraries"] = qtLibraries;
    QFile io(QStringLiteral("/proc/self/io"));
    if (io.open(QIODevice::ReadOnly)) {
        for (const auto& line : io.readAll().split('\n')) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() != 2) {
                continue;
            }
            if (fields[0] == "write_bytes:") {
                result["storage_write_bytes"] = fields[1].toDouble();
            }
            if (fields[0] == "wchar:") {
                result["written_bytes_including_pipes"] = fields[1].toDouble();
            }
        }
    }
    return result;
}

QString fileSha256(const QString& path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file)
        ? QString::fromLatin1(hash.result().toHex()) : QString();
}

void printJson(const QJsonObject& value)
{
    QTextStream(stdout) << QJsonDocument(value).toJson(QJsonDocument::Compact) << Qt::endl;
}

} // namespace LicasaDiagnostics
