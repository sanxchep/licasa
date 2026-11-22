#include "platform/lifecycle_control.h"

#include "platform/application_identity.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTextStream>
#include <QThread>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Licasa {

QJsonObject lifecycleError(const QString& result, const QString& message)
{
    QJsonObject reply = applicationIdentity().toJson();
    reply.insert(QStringLiteral("result"), result);
    if (!message.isEmpty()) {
        reply.insert(QStringLiteral("message"), message);
    }
    return reply;
}

bool lifecycleSocketIsOwned(const QString& path)
{
#if defined(Q_OS_UNIX)
    struct stat status {};
    const QByteArray encoded = QFile::encodeName(path);
    return ::lstat(encoded.constData(), &status) == 0 && S_ISSOCK(status.st_mode) &&
           status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0;
#else
    Q_UNUSED(path);
    return true;
#endif
}

bool lifecyclePeerIdentity(QLocalSocket& socket, qint64* pid)
{
#if defined(Q_OS_LINUX)
    struct ucred credentials {};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(int(socket.socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &credentials,
                     &size) != 0 ||
        size != sizeof(credentials) || credentials.uid != ::geteuid()) {
        return false;
    }
    if (pid) {
        *pid = credentials.pid;
    }
    return true;
#else
    Q_UNUSED(socket);
    Q_UNUSED(pid);
    // A platform needs a qualified credential implementation before control is enabled.
    return false;
#endif
}

QJsonObject lifecycleRequest(const QString& operation, const QJsonObject& arguments, int timeoutMs,
                             bool* writeAttempted)
{
    if (writeAttempted) {
        *writeAttempted = false;
    }
    const ApplicationIdentity identity = applicationIdentity(false);
    const auto fail = [&identity](const QString& result, const QString& message = QString{}) {
        auto reply = identity.toJson();
        reply.insert(QStringLiteral("result"), result);
        if (!message.isEmpty()) {
            reply.insert(QStringLiteral("message"), message);
        }
        return reply;
    };
    const QString endpoint = lifecycleEndpointName();
    if (endpoint.isEmpty()) {
        return fail(QStringLiteral("broken"),
                    QStringLiteral("Private lifecycle IPC is unavailable."));
    }
    const QFileInfo endpointInfo(endpoint);
    if (!endpointInfo.exists() && !endpointInfo.isSymLink()) {
        return fail(QStringLiteral("absent"));
    }
    if (!lifecycleSocketIsOwned(endpoint)) {
        return fail(QStringLiteral("access-denied"), QStringLiteral("Unsafe lifecycle endpoint."));
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const auto remaining = [&]() { return qMax(1, timeoutMs - int(elapsed.elapsed())); };
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(remaining())) {
        return fail(socket.error() == QLocalSocket::ConnectionRefusedError
                        ? QStringLiteral("absent")
                        : QStringLiteral("timeout"),
                    socket.errorString());
    }
    qint64 peerPid = 0;
    if (!lifecyclePeerIdentity(socket, &peerPid)) {
        return fail(QStringLiteral("access-denied"));
    }
    const QString peerStart = processStartIdentity(peerPid);
    const QString peerObject = processExecutableObject(peerPid);
    const bool sharedExecutable =
        canReuseExecutableGeneration(QCoreApplication::applicationPid(), peerPid);
    const QString peerHash = sharedExecutable ? QString{} : processExecutableHash(peerPid);
    if (peerStart.isEmpty() || peerObject.isEmpty() || (!sharedExecutable && peerHash.isEmpty())) {
        return fail(QStringLiteral("broken"),
                    QStringLiteral("Cannot validate the running process."));
    }
    QJsonObject command = arguments;
    command.insert(QStringLiteral("protocol"), 1);
    command.insert(QStringLiteral("installationId"), identity.installationId);
    command.insert(QStringLiteral("operation"), operation);
    const QByteArray bytes = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > maximumLifecycleBytes) {
        return fail(QStringLiteral("broken"));
    }
    if (writeAttempted) {
        *writeAttempted = true;
    }
    if (socket.write(bytes) != bytes.size() || !socket.waitForBytesWritten(remaining())) {
        return fail(QStringLiteral("broken"));
    }
    QByteArray received;
    while (!received.contains('\n') && elapsed.elapsed() < timeoutMs) {
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(remaining())) {
            break;
        }
        if (socket.bytesAvailable() > maximumLifecycleBytes - received.size()) {
            return fail(QStringLiteral("broken"), QStringLiteral("Oversized lifecycle response."));
        }
        received += socket.readAll();
    }
    const auto newline = received.indexOf('\n');
    if (newline < 0) {
        return fail(QStringLiteral("timeout"), QStringLiteral("No complete lifecycle response."));
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(received.left(newline), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("broken"));
    }
    QJsonObject reply = document.object();
    if (reply.value(QStringLiteral("protocol")).toInt() != 1) {
        return fail(QStringLiteral("unsupported"));
    }
    if (reply.value(QStringLiteral("installationId")).toString() != identity.installationId) {
        return fail(QStringLiteral("wrong-installation"));
    }
    const QString receipt = reply.value(QStringLiteral("generation")).toString();
    const bool validDigest =
        receipt.size() == 64 && std::all_of(receipt.cbegin(), receipt.cend(), [](QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
                   (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
        });
    const QString currentStart = processStartIdentity(peerPid);
    const bool exitAcknowledged =
        (operation == QStringLiteral("stop-for-update") || operation == QStringLiteral("quit")) &&
        (reply.value(QStringLiteral("result")).toString() == QStringLiteral("accepted") ||
         reply.value(QStringLiteral("result")).toString() == QStringLiteral("ready"));
    if (currentStart.isEmpty() && !exitAcknowledged) {
        return fail(QStringLiteral("absent"),
                    QStringLiteral("The resident process exited during control."));
    }
    const bool validExecutable =
        currentStart.isEmpty()
            ? exitAcknowledged
            : processExecutableObject(peerPid) == peerObject &&
                  (sharedExecutable
                       ? canReuseExecutableGeneration(QCoreApplication::applicationPid(), peerPid)
                       : receipt == peerHash);
    if (reply.value(QStringLiteral("pid")).toDouble() != double(peerPid) ||
        reply.value(QStringLiteral("startIdentity")).toString() != peerStart || !validDigest ||
        !validExecutable ||
        reply.value(QStringLiteral("generationObject")).toString() != peerObject ||
        (!currentStart.isEmpty() && currentStart != peerStart)) {
        return fail(QStringLiteral("broken"), QStringLiteral("Running process identity changed."));
    }
    if (operation == QStringLiteral("health") &&
        !canReuseExecutableGeneration(QCoreApplication::applicationPid(), peerPid) &&
        reply.value(QStringLiteral("generation")).toString() != applicationIdentity().generation) {
        reply.insert(QStringLiteral("result"), QStringLiteral("wrong-installation"));
        reply.insert(QStringLiteral("message"),
                     QStringLiteral("The running generation differs from this executable."));
    }
    return reply;
}

int lifecycleResultExitCode(const QJsonObject& reply)
{
    const QString result = reply.value(QStringLiteral("result")).toString();
    if (result == QStringLiteral("ready") || result == QStringLiteral("accepted")) {
        return 0;
    }
    if (result == QStringLiteral("absent")) {
        return 3;
    }
    if (result == QStringLiteral("busy")) {
        return 4;
    }
    if (result == QStringLiteral("wrong-installation")) {
        return 5;
    }
    if (result == QStringLiteral("unsupported")) {
        return 6;
    }
    if (result == QStringLiteral("access-denied")) {
        return 7;
    }
    if (result == QStringLiteral("timeout")) {
        return 8;
    }
    return 9;
}

int runLifecycleControl(const QString& operation, int timeoutMs)
{
    const QStringList supported{QStringLiteral("identity"),
                                QStringLiteral("status"),
                                QStringLiteral("health"),
                                QStringLiteral("prepare-update"),
                                QStringLiteral("stop-for-update"),
                                QStringLiteral("cancel-update"),
                                QStringLiteral("quit")};
    QJsonObject reply;
    if (!supported.contains(operation)) {
        reply = lifecycleError(QStringLiteral("unsupported"),
                               QStringLiteral("Unknown lifecycle operation."));
    } else if (operation == QStringLiteral("identity")) {
        const auto identity = applicationIdentity();
        reply = identity.generation.isEmpty() ||
                        processStartIdentity(QCoreApplication::applicationPid()).isEmpty()
                    ? lifecycleError(QStringLiteral("broken"),
                                     QStringLiteral("Cannot establish executable identity."))
                    : lifecycleError(QStringLiteral("ready"));
        reply.insert(QStringLiteral("endpoint"), lifecycleEndpointName());
    } else {
        reply = lifecycleRequest(operation, {}, qMin(timeoutMs, 2000));
        if (reply.value(QStringLiteral("result")).toString() == QStringLiteral("absent")) {
            QString reason;
            int unknownProcessCount = 0;
            bool controlClientPending = false;
            if (scopedInstallationProcessExists(&reason, &unknownProcessCount,
                                                &controlClientPending)) {
                reply = lifecycleError(controlClientPending ? QStringLiteral("busy")
                                                            : QStringLiteral("unsupported"),
                                       reason);
                if (controlClientPending) {
                    reply.insert(QStringLiteral("busyReason"), reason);
                }
            }
            reply.insert(QStringLiteral("inspectionScope"), QStringLiteral("installation"));
            reply.insert(QStringLiteral("unknownProcessCount"), unknownProcessCount);
            reply.insert(QStringLiteral("globalAbsenceProven"), false);
        }
        if ((operation == QStringLiteral("stop-for-update") ||
             operation == QStringLiteral("quit")) &&
            lifecycleResultExitCode(reply) == 0) {
            const qint64 pid = qint64(reply.value(QStringLiteral("pid")).toDouble());
            const QString start = reply.value(QStringLiteral("startIdentity")).toString();
            QElapsedTimer elapsed;
            elapsed.start();
            while (processStartIdentity(pid) == start && elapsed.elapsed() < timeoutMs) {
                QThread::msleep(20);
            }
            if (processStartIdentity(pid) == start) {
                reply.insert(QStringLiteral("result"), QStringLiteral("timeout"));
                reply.insert(QStringLiteral("message"),
                             QStringLiteral("Stop accepted, but the process did not exit."));
            } else {
                reply.insert(QStringLiteral("exited"), true);
            }
        }
    }
    QTextStream(stdout) << QJsonDocument(reply).toJson(QJsonDocument::Compact) << '\n';
    return lifecycleResultExitCode(reply);
}

} // namespace Licasa
