#include "platform/single_instance_endpoint.h"

#include "app/app_constants.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Licasa {
namespace Internal {

#if defined(Q_OS_UNIX)
namespace {

bool trustedRuntimeOwner(uid_t owner) noexcept
{
    struct stat rootStatus {};
    const uid_t filesystemAdministrator =
        (::lstat("/", &rootStatus) == 0 && S_ISDIR(rootStatus.st_mode)) ? rootStatus.st_uid
                                                                        : uid_t(0);
    return owner == 0 || owner == filesystemAdministrator || owner == ::geteuid();
}

bool hasProtectedRuntimeAncestry(const QString& directory, uid_t leafOwner)
{
    QString childPath = directory;
    uid_t childOwner = leafOwner;
    while (childPath != QStringLiteral("/")) {
        const QString parentPath = QFileInfo(childPath).absolutePath();
        const QByteArray encodedParent = QFile::encodeName(parentPath);
        struct stat parentStatus {};
        if (::lstat(encodedParent.constData(), &parentStatus) != 0 ||
            !S_ISDIR(parentStatus.st_mode) || !trustedRuntimeOwner(parentStatus.st_uid)) {
            return false;
        }

        const mode_t sharedWrite = parentStatus.st_mode & (S_IWGRP | S_IWOTH);
        if (sharedWrite != 0) {
            // A sticky shared directory such as /tmp protects only entries
            // owned by the current user or root. Its own owner must also be a
            // trusted principal, checked above.
            if ((parentStatus.st_mode & S_ISVTX) == 0 || !trustedRuntimeOwner(childOwner)) {
                return false;
            }
        }

        childPath = parentPath;
        childOwner = parentStatus.st_uid;
    }
    return true;
}

} // namespace
#endif

bool isPrivateSingleInstanceRuntimeDirectory(const QString& directory)
{
#if defined(Q_OS_UNIX)
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory)) {
        return false;
    }

    const QString absolutePath = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    const QByteArray encodedPath = QFile::encodeName(absolutePath);
    struct stat status {};
    if (::lstat(encodedPath.constData(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid()) {
        return false;
    }

    // XDG_RUNTIME_DIR is required to be owned by the user and mode 0700.
    // Requiring that exact boundary also rejects group/other access bits and
    // special directory modes that are inappropriate for private IPC.
    if ((status.st_mode & 07777) != 0700) {
        return false;
    }

    const QString canonicalPath = QFileInfo(absolutePath).canonicalFilePath();
    return !canonicalPath.isEmpty() && canonicalPath == absolutePath &&
           hasProtectedRuntimeAncestry(absolutePath, status.st_uid);
#else
    Q_UNUSED(directory);
    return false;
#endif
}

} // namespace Internal

QString singleInstanceEndpointName()
{
#if defined(Q_OS_UNIX)
    const QString configuredRuntime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (configuredRuntime.isEmpty() || !QDir::isAbsolutePath(configuredRuntime)) {
        qWarning() << "Single-instance IPC disabled: XDG_RUNTIME_DIR is unavailable or relative.";
        return {};
    }

    const QString expectedRuntime = QDir::cleanPath(configuredRuntime);
    const QString runtimeLocation =
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    if (runtimeLocation != expectedRuntime ||
        !Internal::isPrivateSingleInstanceRuntimeDirectory(runtimeLocation)) {
        qWarning() << "Single-instance IPC disabled: the runtime directory is not private.";
        return {};
    }

    return QDir(runtimeLocation)
        .absoluteFilePath(QString::fromLatin1(Constants::singleInstanceServerName));
#else
    // Windows uses a protected named-pipe namespace rather than a filesystem
    // socket. Keep Qt's native server name on that platform.
    return QString::fromLatin1(Constants::singleInstanceServerName);
#endif
}

} // namespace Licasa
