#pragma once

#include <QFile>
#include <QString>
#include <QtGlobal>

#include <cerrno>
#include <cstring>
#include <optional>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Licasa {

// Immutable identity captured from an already-open regular file descriptor.
// Apple Live Photo pairing records this identity while it parses the MOV's
// content identifier; playback must observe the same identity before using the
// file. Size + mtime + ctime also make in-place mutation detectable on later
// ByteRangeDevice reads, while a path replacement cannot affect the pinned fd.
struct ExternalFileIdentity {
    quint64 device = 0;
    quint64 inode = 0;
    quint64 linkCount = 0;
    quint64 size = 0;
    qint64 modifiedSeconds = 0;
    qint64 modifiedNanoseconds = 0;
    qint64 changedSeconds = 0;
    qint64 changedNanoseconds = 0;
};

inline bool operator==(const ExternalFileIdentity& left, const ExternalFileIdentity& right) noexcept
{
    return left.device == right.device && left.inode == right.inode &&
           left.linkCount == right.linkCount && left.size == right.size &&
           left.modifiedSeconds == right.modifiedSeconds &&
           left.modifiedNanoseconds == right.modifiedNanoseconds &&
           left.changedSeconds == right.changedSeconds &&
           left.changedNanoseconds == right.changedNanoseconds;
}

inline bool operator!=(const ExternalFileIdentity& left, const ExternalFileIdentity& right) noexcept
{
    return !(left == right);
}

#if defined(Q_OS_UNIX)
namespace ExternalFileIdentityDetail {
inline std::optional<ExternalFileIdentity> fromStat(const struct stat& status)
{
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        return std::nullopt;
    }
#if defined(Q_OS_DARWIN)
    const auto modified = status.st_mtimespec;
    const auto changed = status.st_ctimespec;
#else
    const auto modified = status.st_mtim;
    const auto changed = status.st_ctim;
#endif
    return ExternalFileIdentity{
        quint64(status.st_dev),  quint64(status.st_ino),  quint64(status.st_nlink),
        quint64(status.st_size), qint64(modified.tv_sec), qint64(modified.tv_nsec),
        qint64(changed.tv_sec),  qint64(changed.tv_nsec),
    };
}
} // namespace ExternalFileIdentityDetail

// Resolve the current path without reopening it on cache hits. A decoder
// still compares its open descriptor with this identity before using it.
inline std::optional<ExternalFileIdentity> externalFileIdentityAtPath(const QString& path)
{
    const QByteArray name = QFile::encodeName(path);
    struct stat status {};
    if (::stat(name.constData(), &status) != 0) {
        return std::nullopt;
    }
    return ExternalFileIdentityDetail::fromStat(status);
}
#endif

inline std::optional<ExternalFileIdentity> externalFileIdentity(const QFile& file,
                                                                QString* reason = nullptr)
{
#if defined(Q_OS_UNIX)
    if (!file.isOpen() || file.handle() < 0) {
        if (reason) {
            *reason = QStringLiteral("External video file is not open");
        }
        return std::nullopt;
    }
    struct stat status {};
    if (::fstat(file.handle(), &status) != 0) {
        if (reason) {
            *reason = QStringLiteral("Unable to inspect external video file identity: %1")
                          .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return std::nullopt;
    }
    const auto identity = ExternalFileIdentityDetail::fromStat(status);
    if (!identity && reason) {
        *reason = QStringLiteral("External video source is not a regular file");
    }
    return identity;
#else
    Q_UNUSED(file);
    if (reason) {
        *reason = QStringLiteral("External video identity pinning is unavailable on this platform");
    }
    return std::nullopt;
#endif
}

} // namespace Licasa
