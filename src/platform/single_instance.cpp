#include "platform/single_instance.h"

#include "app/application_lifecycle.h"
#include "app/window_manager.h"
#include "platform/application_identity.h"
#include "platform/lifecycle_control.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>

#include <memory>

namespace Licasa {
namespace {

bool runningInstanceExists()
{
    const auto reply = lifecycleRequest(QStringLiteral("status"));
    return lifecycleResultExitCode(reply) == 0;
}

} // namespace

LaunchOptions parseLaunchOptions(const QStringList& arguments)
{
    LaunchOptions options;
    for (int index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--background")) {
            options.background = true;
        } else if (argument != QStringLiteral("--quit") && options.initialUrl.isEmpty()) {
            const QFileInfo fileInfo(argument);
            options.initialUrl = fileInfo.exists() && fileInfo.isFile()
                                     ? QUrl::fromLocalFile(fileInfo.absoluteFilePath())
                                     : QUrl::fromUserInput(argument);
        }
    }
    return options;
}

ForwardLaunchResult forwardLaunchToExistingInstance(const LaunchOptions& options)
{
    if (lifecycleEndpointName().isEmpty()) {
        QString reason;
        if (sameInstallationProcessExists(&reason, false)) {
            qCritical().noquote() << reason;
            return ForwardLaunchResult::Blocked;
        }
        return ForwardLaunchResult::NoInstance;
    }
    auto reply = lifecycleRequest(QStringLiteral("status"));
    if (reply.value(QStringLiteral("result")).toString() == QStringLiteral("absent")) {
        QString reason;
        if (sameInstallationProcessExists(&reason, false)) {
            qCritical().noquote() << reason;
            return ForwardLaunchResult::Blocked;
        }
        return ForwardLaunchResult::NoInstance;
    }
    if (lifecycleResultExitCode(reply) != 0) {
        qCritical().noquote() << "Cannot safely contact the existing Licasa installation:"
                              << reply.value(QStringLiteral("message")).toString();
        return ForwardLaunchResult::Blocked;
    }
    const QString residentGeneration = reply.value(QStringLiteral("generation")).toString();
    const qint64 residentPid = qint64(reply.value(QStringLiteral("pid")).toDouble());
    if (!canReuseExecutableGeneration(QCoreApplication::applicationPid(), residentPid) &&
        residentGeneration != applicationIdentity().generation) {
        // A newer (or intentionally older) binary may take over only an empty
        // cooperative predecessor. Never route user work into stale code.
        reply = lifecycleRequest(QStringLiteral("prepare-update"));
        if (lifecycleResultExitCode(reply) != 0) {
            qCritical()
                << "Another generation has active work. Finish it and Quit Licasa before retrying.";
            return ForwardLaunchResult::Blocked;
        }
        if (runLifecycleControl(QStringLiteral("stop-for-update")) != 0) {
            (void)lifecycleRequest(QStringLiteral("cancel-update"));
            return ForwardLaunchResult::Blocked;
        }
        return ForwardLaunchResult::NoInstance;
    }
    if (options.background && options.initialUrl.isEmpty()) {
        // Duplicate login/background probes cannot reset the hidden idle clock.
        return ForwardLaunchResult::Forwarded;
    }
    QJsonObject arguments{
        {QStringLiteral("generation"), residentGeneration},
        {QStringLiteral("desktopSessionId"),
         applicationIdentity(false).toJson().value(QStringLiteral("desktopSessionId"))}};
    const QString operation =
        options.initialUrl.isEmpty() ? QStringLiteral("show") : QStringLiteral("open");
    if (!options.initialUrl.isEmpty()) {
        arguments.insert(QStringLiteral("url"), options.initialUrl.toString(QUrl::FullyEncoded));
    }
    reply = lifecycleRequest(operation, arguments);
    if (lifecycleResultExitCode(reply) == 0) {
        return ForwardLaunchResult::Forwarded;
    }
    qCritical() << "The existing Licasa instance is preparing an update. Retry when it completes.";
    return ForwardLaunchResult::Blocked;
}

SingleInstanceServer::SingleInstanceServer(WindowManager* windowManager,
                                           ApplicationLifecycle* lifecycle, QObject* parent)
    : QObject(parent), windowManager_(windowManager), lifecycle_(lifecycle)
{
    connect(&server_, &QLocalServer::newConnection, this,
            &SingleInstanceServer::acceptPendingConnections);
}

bool SingleInstanceServer::start()
{
    const QString name = lifecycleEndpointName();
    if (name.isEmpty()) {
        qWarning() << "Private lifecycle IPC is unavailable; managed updates require full Quit.";
        return true;
    }
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (server_.listen(name)) {
        return true;
    }
    // Probe afresh before unlinking, and never replace a foreign file/symlink.
    if (runningInstanceExists() || !lifecycleSocketIsOwned(name)) {
        qCritical() << "Another listener or unsafe file owns the lifecycle endpoint.";
        return false;
    }
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(200) || probe.error() != QLocalSocket::ConnectionRefusedError) {
        qCritical() << "The lifecycle endpoint may still be live.";
        return false;
    }
    QLocalServer::removeServer(name);
    if (server_.listen(name)) {
        return true;
    }
    qCritical() << "Unable to start the lifecycle server:" << server_.errorString();
    return false;
}

void SingleInstanceServer::stop() { server_.close(); }

void SingleInstanceServer::acceptPendingConnections()
{
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        if (!lifecyclePeerIdentity(*socket)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        struct ReadState {
            QByteArray buffer;
            bool finished = false;
        };
        const auto state = std::make_shared<ReadState>();
        const auto consume = [this, socket, state] {
            if (state->finished) {
                return;
            }
            if (socket->bytesAvailable() > maximumLifecycleBytes - state->buffer.size()) {
                state->finished = true;
                socket->abort();
                return;
            }
            state->buffer += socket->readAll();
            const qsizetype newline = state->buffer.indexOf('\n');
            if (newline < 0) {
                return;
            }
            state->finished = true;
            const QJsonObject reply = processPayload(state->buffer.left(newline));
            socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, socket, consume);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        // Bound silent/partial clients as well as command bytes.
        QTimer::singleShot(2000, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        consume();
    }
}

QJsonObject SingleInstanceServer::processPayload(const QByteArray& payload)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return lifecycleError(QStringLiteral("unsupported"),
                              QStringLiteral("A JSON lifecycle command is required."));
    }
    const QJsonObject request = document.object();
    if (request.value(QStringLiteral("protocol")).toInt() != 1) {
        return lifecycleError(QStringLiteral("unsupported"));
    }
    if (request.value(QStringLiteral("installationId")).toString() !=
        applicationIdentity().installationId) {
        return lifecycleError(QStringLiteral("wrong-installation"));
    }
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("open") || operation == QStringLiteral("show")) {
        if (request.value(QStringLiteral("generation")).toString() !=
            applicationIdentity().generation) {
            return lifecycleError(QStringLiteral("wrong-installation"),
                                  QStringLiteral("Build generation differs."));
        }
        if (request.value(QStringLiteral("desktopSessionId")) !=
            applicationIdentity(false).toJson().value(QStringLiteral("desktopSessionId"))) {
            return lifecycleError(
                QStringLiteral("busy"),
                QStringLiteral("This installation is active in another desktop session."));
        }
        if (!lifecycle_ || !lifecycle_->launchAllowed()) {
            return lifecycleError(QStringLiteral("busy"));
        }
        if (operation == QStringLiteral("show")) {
            if (windowManager_) {
                windowManager_->showAnyWindow();
            }
        } else {
            const QUrl url =
                QUrl::fromEncoded(request.value(QStringLiteral("url")).toString().toUtf8());
            if (!url.isValid() || url.isEmpty() || !windowManager_ ||
                !windowManager_->openInNewWindow(url)) {
                return lifecycleError(QStringLiteral("broken"),
                                      QStringLiteral("Cannot open the requested image."));
            }
        }
        return lifecycleError(QStringLiteral("accepted"));
    }
    QJsonObject reply = applicationIdentity().toJson();
    const QJsonObject result =
        lifecycle_ ? lifecycle_->command(operation)
                   : QJsonObject{{QStringLiteral("result"), QStringLiteral("broken")}};
    for (auto it = result.begin(); it != result.end(); ++it) {
        reply.insert(it.key(), it.value());
    }
    return reply;
}

} // namespace Licasa
