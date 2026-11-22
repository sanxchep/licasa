#include "platform/application_identity.h"

#include "platform/single_instance_endpoint.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#if defined(LICASA_EXECUTABLE_HASH_OPENSSL)
#include <array>
#include <memory>
#include <openssl/evp.h>
#endif

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace Licasa {
namespace {

QString hashFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
#if defined(LICASA_EXECUTABLE_HASH_OPENSSL)
    // Keep the same complete SHA-256 digest for portable executable receipts.
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1) {
        std::array<char, 64 * 1024> bytes;
        qint64 count = 0;
        while ((count = file.read(bytes.data(), bytes.size())) > 0) {
            if (EVP_DigestUpdate(context.get(), bytes.data(), size_t(count)) != 1) {
                return {};
            }
        }
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        if (count < 0 || EVP_DigestFinal_ex(context.get(), digest, &size) != 1 || size != 32) {
            return {};
        }
        return QString::fromLatin1(
            QByteArray(reinterpret_cast<const char*>(digest), size).toHex());
    }
#endif
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString{};
}

QString absolutePath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isHeadlessLifecycleControl(const QByteArray& commandLine)
{
    if (commandLine.isEmpty() || commandLine.size() > 65536 || !commandLine.endsWith('\0')) {
        return false;
    }
    auto arguments = commandLine.split('\0');
    arguments.removeLast();
    if (arguments.isEmpty() || arguments.first().isEmpty()) {
        return false;
    }
    if (arguments.size() == 2) {
        return arguments.at(1) == "--quit";
    }
    if (arguments.size() != 3 || arguments.at(1) != "--lifecycle") {
        return false;
    }
    const QList<QByteArray> operations{
        "identity",        "status",        "health", "prepare-update",
        "stop-for-update", "cancel-update", "quit"};
    return operations.contains(arguments.at(2));
}

} // namespace

QString processExecutablePath(qint64 pid)
{
#if defined(Q_OS_LINUX)
    const QByteArray name = QFile::encodeName(QStringLiteral("/proc/%1/exe").arg(pid));
    QByteArray target(4096, '\0');
    const auto count = ::readlink(name.constData(), target.data(), size_t(target.size()));
    if (count <= 0 || count == target.size()) {
        return {};
    }
    target.resize(qsizetype(count));
    if (target.endsWith(" (deleted)")) {
        target.chop(10);
    }
    return QString::fromLocal8Bit(target);
#else
    return pid == QCoreApplication::applicationPid() ? QCoreApplication::applicationFilePath()
                                                     : QString{};
#endif
}

QString processExecutableHash(qint64 pid)
{
#if defined(Q_OS_LINUX)
    return hashFile(QStringLiteral("/proc/%1/exe").arg(pid));
#else
    return hashFile(processExecutablePath(pid));
#endif
}

QString processExecutableObject(qint64 pid)
{
#if defined(Q_OS_LINUX)
    struct stat status {};
    const QByteArray path = QFile::encodeName(QStringLiteral("/proc/%1/exe").arg(pid));
    if (::stat(path.constData(), &status) == 0) {
        return QString::number(qulonglong(status.st_dev)) + ':' +
               QString::number(qulonglong(status.st_ino)) + ':' + QString::number(status.st_size) +
               ':' + QString::number(status.st_mtim.tv_sec) + ':' +
               QString::number(status.st_mtim.tv_nsec) + ':' +
               QString::number(status.st_ctim.tv_sec) + ':' +
               QString::number(status.st_ctim.tv_nsec);
    }
#else
    Q_UNUSED(pid);
#endif
    return {};
}

bool sameProcessExecutable(qint64 firstPid, qint64 secondPid)
{
#if defined(Q_OS_LINUX)
    struct stat first {};
    struct stat second {};
    const QByteArray firstPath = QFile::encodeName(QStringLiteral("/proc/%1/exe").arg(firstPid));
    const QByteArray secondPath = QFile::encodeName(QStringLiteral("/proc/%1/exe").arg(secondPid));
    // This identifies the kernel file object. Reusing its generation additionally
    // requires write denial or a read-only filesystem; inode equality alone is
    // insufficient on kernels/filesystems with pre-content/HSM write exceptions.
    return ::stat(firstPath.constData(), &first) == 0 &&
           ::stat(secondPath.constData(), &second) == 0 && first.st_dev == second.st_dev &&
           first.st_ino == second.st_ino;
#else
    Q_UNUSED(firstPid);
    Q_UNUSED(secondPid);
    return false;
#endif
}

bool canReuseExecutableGeneration(qint64 firstPid, qint64 secondPid)
{
#if defined(Q_OS_LINUX)
    if (!sameProcessExecutable(firstPid, secondPid)) {
        return false;
    }
    const QByteArray path = QFile::encodeName(QStringLiteral("/proc/%1/exe").arg(firstPid));
    struct statvfs filesystem {};
    if (::statvfs(path.constData(), &filesystem) == 0 && (filesystem.f_flag & ST_RDONLY) != 0) {
        return true;
    }
    const int descriptor = ::open(path.constData(), O_WRONLY | O_CLOEXEC);
    if (descriptor >= 0) {
        ::close(descriptor); // Probe only: no truncation, writes or mutations.
        return false;
    }
    return errno == ETXTBSY;
#else
    Q_UNUSED(firstPid);
    Q_UNUSED(secondPid);
    return false;
#endif
}

QString processStartIdentity(qint64 pid)
{
#if defined(Q_OS_LINUX)
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    QFile boot(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!stat.open(QIODevice::ReadOnly) || !boot.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray record = stat.read(8192);
    const qsizetype endName = record.lastIndexOf(')');
    if (endName < 0) {
        return {};
    }
    const auto fields = record.mid(endName + 2).simplified().split(' ');
    // Field 3 is the first field after the parenthesized command name.
    if (fields.size() <= 19 || fields.at(0) == "Z") {
        return {};
    }
    return QString::fromLatin1(boot.read(128).trimmed() + ':' + fields.at(19));
#else
    Q_UNUSED(pid);
    return {};
#endif
}

ApplicationIdentity applicationIdentity(bool includeGeneration)
{
    ApplicationIdentity identity;
    identity.executablePath = processExecutablePath(QCoreApplication::applicationPid());
    if (identity.executablePath.isEmpty()) {
        identity.executablePath = QCoreApplication::applicationFilePath();
    }
    identity.executablePath = absolutePath(identity.executablePath);
    if (includeGeneration) {
        static QMutex receiptMutex;
        QMutexLocker receiptLock(&receiptMutex);
        static auto receipt = [] {
            const qint64 pid = QCoreApplication::applicationPid();
            const QString before = processExecutableObject(pid);
            const QString hash = processExecutableHash(pid);
            const QString after = processExecutableObject(pid);
            return qMakePair(before == after ? hash : QString{}, after);
        }();
        const qint64 pid = QCoreApplication::applicationPid();
        const QString current = processExecutableObject(pid);
        if (current != receipt.second) {
            // Unlink/rename can change metadata without changing the executing
            // bytes. Revalidate once, preserving the original running digest.
            const QString hash = processExecutableHash(pid);
            const QString after = processExecutableObject(pid);
            if (hash != receipt.first || current != after) {
                receipt.first.clear();
            }
            receipt.second = after;
        }
        identity.generation = receipt.first;
        identity.generationObject = receipt.second;
    }
#ifdef LICASA_SOURCE_RECEIPT
    identity.sourceReceipt = QStringLiteral(LICASA_SOURCE_RECEIPT);
#else
    identity.sourceReceipt = QStringLiteral("unrecorded");
#endif
    QString anchor;
    const QString snap =
        qEnvironmentVariable("SNAP_INSTANCE_NAME", qEnvironmentVariable("SNAP_NAME"));
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    const QString appDir = qEnvironmentVariable("APPDIR");
    if (!snap.isEmpty()) {
        identity.packageFamily = QStringLiteral("snap");
        identity.packageRevision = qEnvironmentVariable("SNAP_REVISION");
        anchor = snap;
        identity.installationRoot = QStringLiteral("/snap/") + snap + QStringLiteral("/current");
    } else if (!appImage.isEmpty()) {
        identity.packageFamily = QStringLiteral("portable");
        anchor = absolutePath(appImage);
        identity.installationRoot = anchor;
    } else if (!appDir.isEmpty()) {
        identity.packageFamily = QStringLiteral("appimage");
        anchor = absolutePath(appDir);
        identity.installationRoot = anchor;
    } else {
        identity.packageFamily = QStringLiteral("filesystem");
        anchor = identity.executablePath;
        const QDir directory(QFileInfo(anchor).absolutePath());
        identity.installationRoot = directory.dirName() == QStringLiteral("bin")
                                        ? QFileInfo(directory.path()).absolutePath()
                                        : directory.path();
    }
#if defined(Q_OS_UNIX)
    const QByteArray uid = QByteArray::number(::geteuid());
#else
    const QByteArray uid = qgetenv("USERNAME");
#endif
    identity.installationId =
        QString::fromLatin1(QCryptographicHash::hash(uid + '\n' + identity.packageFamily.toUtf8() +
                                                         '\n' + anchor.toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toHex()
                                .left(32));
    return identity;
}

QString applicationLaunchPath()
{
    const ApplicationIdentity identity = applicationIdentity(false);
    if (identity.packageFamily == QStringLiteral("portable")) {
        return identity.installationRoot;
    }
    if (identity.packageFamily == QStringLiteral("appimage")) {
        const QFileInfo appRun(QDir(identity.installationRoot).filePath(QStringLiteral("AppRun")));
        if (appRun.isFile()) {
            return appRun.absoluteFilePath();
        }
    }
    if (identity.executablePath == QStringLiteral("/usr/bin/licasa.bin")) {
        return QStringLiteral("/usr/bin/licasa");
    }
    const QFileInfo launcher(
        QFileInfo(identity.executablePath).dir().filePath(QStringLiteral("licasa-launch")));
    if (launcher.isFile() && !launcher.isSymLink() && launcher.isExecutable() &&
        launcher.ownerId() == QFileInfo(identity.executablePath).ownerId()) {
        return launcher.absoluteFilePath();
    }
    return identity.executablePath;
}

QJsonObject ApplicationIdentity::toJson() const
{
    return {{QStringLiteral("protocol"), 1},
            {QStringLiteral("installationId"), installationId},
            {QStringLiteral("generation"), generation},
            {QStringLiteral("generationObject"), generationObject},
            {QStringLiteral("sourceReceipt"), sourceReceipt},
            {QStringLiteral("sessionScope"), QStringLiteral("user-runtime")},
            {QStringLiteral("desktopSessionId"),
             qEnvironmentVariable(
                 "XDG_SESSION_ID",
                 qEnvironmentVariable("WAYLAND_DISPLAY", qEnvironmentVariable("DISPLAY")))},
            {QStringLiteral("packageFamily"), packageFamily},
            {QStringLiteral("packageRevision"), packageRevision},
            {QStringLiteral("installationRoot"), installationRoot},
            {QStringLiteral("executablePath"), executablePath},
            {QStringLiteral("pid"), double(QCoreApplication::applicationPid())},
            {QStringLiteral("startIdentity"),
             processStartIdentity(QCoreApplication::applicationPid())}};
}

QString lifecycleEndpointName()
{
    const QString legacy = singleInstanceEndpointName();
    if (legacy.isEmpty()) {
        return {};
    }
    const QString name =
        QDir(QFileInfo(legacy).absolutePath())
            .filePath(QStringLiteral("licasa-lc-") + applicationIdentity(false).installationId +
                      QStringLiteral(".sock"));
#if defined(Q_OS_UNIX)
    // sockaddr_un includes its trailing NUL. Do not silently use a shared fallback.
    if (QFile::encodeName(name).size() > 103) {
        return {};
    }
#endif
    return name;
}

namespace {

enum class ProcessProbeMode { StrictAbsence, ScopedAbsence, GuiStartup };

bool installationProcessExists(QString* reason, ProcessProbeMode mode,
                               int* unknownProcessCount = nullptr,
                               bool* controlClientPending = nullptr)
{
    if (unknownProcessCount) {
        *unknownProcessCount = 0;
    }
    if (controlClientPending) {
        *controlClientPending = false;
    }
#if defined(Q_OS_LINUX)
    const QString self = processExecutablePath(QCoreApplication::applicationPid());
    const auto entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        bool numeric = false;
        const qint64 pid = entry.toLongLong(&numeric);
        if (!numeric || pid == QCoreApplication::applicationPid()) {
            continue;
        }
        const QString directory = QStringLiteral("/proc/") + entry;
        const auto inspectionUnavailable = [&](bool knownViewer = false) {
            if (!knownViewer && unknownProcessCount) {
                ++*unknownProcessCount;
            }
            if (!knownViewer && mode != ProcessProbeMode::StrictAbsence) {
                return false;
            }
            if (reason) {
                *reason = QStringLiteral("Process inspection is unavailable for process %1; "
                                         "the absence of another viewer cannot be established.")
                              .arg(pid);
            }
            return true;
        };
        // A nondumpable process's /proc directory can become root-owned. Its
        // kernel Uid fields, rather than directory ownership, identify its user.
        QFile status(directory + QStringLiteral("/status"));
        if (!status.open(QIODevice::ReadOnly)) {
            if (!QFileInfo::exists(directory)) {
                continue;
            }
            if (inspectionUnavailable()) {
                return true;
            }
            continue;
        }
        const QByteArray statusRecord = status.read(65537);
        if (statusRecord.size() > 65536) {
            if (inspectionUnavailable()) {
                return true;
            }
            continue;
        }
        QByteArray uidRecord;
        bool zombie = false;
        for (const QByteArray& line : statusRecord.split('\n')) {
            if (line.startsWith("Uid:")) {
                uidRecord = line.mid(4).simplified();
            } else if (line.startsWith("State:")) {
                zombie = line.mid(6).trimmed().startsWith('Z');
            }
        }
        if (zombie) {
            continue;
        }
        const auto userIds = uidRecord.split(' ');
        if (userIds.size() != 4) {
            if (inspectionUnavailable()) {
                return true;
            }
            continue;
        }
        bool sameUser = false;
        bool validUserIds = true;
        for (const QByteArray& userId : userIds) {
            bool valid = false;
            const qulonglong value = userId.toULongLong(&valid);
            if (!valid) {
                validUserIds = false;
                break;
            }
            sameUser |= value == ::geteuid();
        }
        if (!validUserIds) {
            if (inspectionUnavailable()) {
                return true;
            }
            continue;
        }
        if (!sameUser) {
            continue;
        }
        const QString executable = processExecutablePath(pid);
        if (executable.isEmpty()) {
            if (!QFileInfo::exists(directory)) {
                continue;
            }
            if (inspectionUnavailable()) {
                return true;
            }
            continue;
        }
        bool same = executable == self;
        // Portable mounts and Snap revision mounts change between generations.
        // Match their stable package lineage as well, never only a basename.
        if (!same && QFileInfo(executable).fileName() == QStringLiteral("licasa")) {
            QFile environment(QStringLiteral("/proc/%1/environ").arg(pid));
            if (environment.open(QIODevice::ReadOnly)) {
                const auto variables = environment.read(1024 * 1024).split('\0');
                const auto has = [&variables](const QByteArray& key, const QByteArray& value) {
                    return !value.isEmpty() && variables.contains(key + '=' + value);
                };
                const auto identity = applicationIdentity(false);
                if (identity.packageFamily == QStringLiteral("snap")) {
                    same = has("SNAP_INSTANCE_NAME", qgetenv("SNAP_INSTANCE_NAME")) ||
                           (qEnvironmentVariableIsEmpty("SNAP_INSTANCE_NAME") &&
                            has("SNAP_NAME", qgetenv("SNAP_NAME")));
                } else if (identity.packageFamily == QStringLiteral("portable")) {
                    same = has("APPIMAGE", qgetenv("APPIMAGE"));
                } else if (identity.packageFamily == QStringLiteral("appimage")) {
                    same = has("APPDIR", qgetenv("APPDIR"));
                }
            } else {
                same = true; // An inaccessible possible viewer cannot establish absence.
            }
        }
        if (same) {
            if (processStartIdentity(pid).isEmpty()) {
                if (!QFileInfo::exists(directory)) {
                    continue;
                }
                return inspectionUnavailable(true);
            }
            QFile commandLine(QStringLiteral("/proc/%1/cmdline").arg(pid));
            if (commandLine.open(QIODevice::ReadOnly) &&
                isHeadlessLifecycleControl(commandLine.read(65537))) {
                if (mode == ProcessProbeMode::GuiStartup) {
                    // Control clients return before GUI initialization. A probe
                    // must not prevent ordinary GUI startup.
                    continue;
                }
                // Mutable argv does not prove absence. An update/status probe
                // must observe exit or report this client as still completing.
                if (controlClientPending) {
                    *controlClientPending = true;
                }
                if (reason) {
                    *reason = QStringLiteral("A lifecycle control client (%1) is still "
                                             "completing; retry shortly.")
                                  .arg(pid);
                }
                return true;
            }
            if (reason) {
                *reason =
                    QStringLiteral("An existing process (%1) could not be identified through "
                                   "the lifecycle protocol. Fully Quit that copy before retrying.")
                        .arg(pid);
            }
            return true;
        }
    }
#else
    Q_UNUSED(reason);
    Q_UNUSED(mode);
#endif
    return false;
}

} // namespace

bool sameInstallationProcessExists(QString* reason, bool requireCompleteInspection)
{
    return installationProcessExists(reason, requireCompleteInspection
                                                 ? ProcessProbeMode::StrictAbsence
                                                 : ProcessProbeMode::GuiStartup);
}

bool scopedInstallationProcessExists(QString* reason, int* unknownProcessCount,
                                     bool* controlClientPending)
{
    return installationProcessExists(reason, ProcessProbeMode::ScopedAbsence, unknownProcessCount,
                                     controlClientPending);
}

} // namespace Licasa
