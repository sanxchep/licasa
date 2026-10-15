#include "app/runtime_error_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <utility>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Licasa {
namespace {

constexpr qint64 maximumLogBytes = 512 * 1024;
std::atomic<RuntimeErrorLog*> activeLog{nullptr};

const char* severityName(QtMsgType type)
{
    switch (type) {
    case QtWarningMsg:
        return "WARNING";
    case QtCriticalMsg:
        return "ERROR";
    case QtFatalMsg:
        return "FATAL";
    default:
        return nullptr;
    }
}

void printToStderr(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QByteArray line = qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

#if defined(Q_OS_UNIX)
int openPrivateFile(const QByteArray& encodedPath, off_t* size)
{
    const int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW;
    const int descriptor = ::open(encodedPath.constData(), flags, 0600);
    if (descriptor < 0) {
        return -1;
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() || ::fchmod(descriptor, 0600) != 0) {
        ::close(descriptor);
        return -1;
    }
    *size = status.st_size;
    return descriptor;
}

bool appendPrivateFile(const QString& path, const QByteArray& line)
{
    const QByteArray encoded = QFile::encodeName(path);
    off_t currentSize = 0;
    int descriptor = openPrivateFile(encoded, &currentSize);
    if (descriptor < 0) {
        return false;
    }
    if (currentSize > maximumLogBytes - line.size()) {
        ::close(descriptor);
        const QString backup = path + QStringLiteral(".1");
        QFile::remove(backup);
        if (!QFile::rename(path, backup)) {
            return false;
        }
        descriptor = openPrivateFile(encoded, &currentSize);
        if (descriptor < 0) {
            return false;
        }
    }
    const char* cursor = line.constData();
    qsizetype remaining = line.size();
    while (remaining > 0) {
        const ssize_t written = ::write(descriptor, cursor, size_t(remaining));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ::close(descriptor);
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return ::close(descriptor) == 0;
}
#else
bool appendPrivateFile(const QString& path, const QByteArray& line)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.size() + line.size() > maximumLogBytes) {
        file.close();
        QFile::remove(path + QStringLiteral(".1"));
        if (!QFile::rename(path, path + QStringLiteral(".1")) ||
            !file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            return false;
        }
    }
    return file.write(line) == line.size();
}
#endif

} // namespace

QString defaultRuntimeErrorLogPath()
{
    QString stateRoot = qEnvironmentVariable("XDG_STATE_HOME");
    if (!QDir::isAbsolutePath(stateRoot)) {
        const QString snapData = qEnvironmentVariable("SNAP_USER_DATA");
        const QString home = QDir::isAbsolutePath(snapData) ? snapData : QDir::homePath();
        stateRoot = QDir(home).filePath(QStringLiteral(".local/state"));
    }
    return QDir(stateRoot).filePath(QStringLiteral("licasa/errors.log"));
}

RuntimeErrorLog::RuntimeErrorLog(QString path)
    : path_(path.isEmpty() ? defaultRuntimeErrorLogPath() : std::move(path))
{
    const QString directory = QFileInfo(path_).absolutePath();
    if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink() ||
        !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner)) {
        std::fprintf(stderr, "Licasa could not create its private error log directory.\n");
        return;
    }
    if (activeLog.load() != nullptr) {
        std::fprintf(stderr, "Licasa error logging was already initialized.\n");
        return;
    }
    activeLog.store(this);
    previousHandler_ = qInstallMessageHandler(&RuntimeErrorLog::handleMessage);
    installed_ = true;
}

RuntimeErrorLog::~RuntimeErrorLog()
{
    if (installed_) {
        qInstallMessageHandler(previousHandler_);
        activeLog.store(nullptr);
    }
}

QString RuntimeErrorLog::path() const { return installed_ ? path_ : QString{}; }

void RuntimeErrorLog::handleMessage(QtMsgType type, const QMessageLogContext& context,
                                    const QString& message)
{
    RuntimeErrorLog* log = activeLog.load();
    if (log && log->previousHandler_) {
        log->previousHandler_(type, context, message);
    } else {
        printToStderr(type, context, message);
    }
    if (log && severityName(type)) {
        log->append(type, message);
    }
}

void RuntimeErrorLog::append(QtMsgType type, const QString& message)
{
    QMutexLocker locker(&mutex_);
    QString oneLine = message.left(2048);
    oneLine.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    oneLine.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    const QByteArray line = QStringLiteral("%1 %2 %3\n")
                                .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                                     QString::fromLatin1(severityName(type)), oneLine)
                                .toUtf8();
    if (!appendPrivateFile(path_, line)) {
        std::fprintf(stderr, "Licasa could not write its error log.\n");
    }
}

} // namespace Licasa
