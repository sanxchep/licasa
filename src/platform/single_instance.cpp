#include "platform/single_instance.h"

#include "app/window_manager.h"
#include "platform/single_instance_endpoint.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QLocalSocket>

#include <memory>

namespace Licasa {
namespace {

constexpr qsizetype kMaximumCommandBytes = 16 * 1024;

QString serverName()
{
    // Resolve once after QCoreApplication identity has been established. Every
    // probe, client, stale cleanup and listener must use this exact endpoint.
    static const QString name = singleInstanceEndpointName();
    return name;
}

QByteArray makeOpenMessage(const QUrl& url) { return "OPEN " + url.toEncoded() + '\n'; }

QByteArray makeShowMessage() { return QByteArrayLiteral("SHOW\n"); }

QByteArray makeQuitMessage() { return QByteArrayLiteral("QUIT\n"); }

bool sendMessageToRunningInstance(const QByteArray& message, int timeoutMs = 120)
{
    if (message.isEmpty() || message.size() > kMaximumCommandBytes) {
        return false;
    }

    const QString name = serverName();
    if (name.isEmpty()) {
        return false;
    }

    QLocalSocket socket;
    socket.connectToServer(name);

    if (!socket.waitForConnected(timeoutMs) || socket.write(message) == -1 ||
        !socket.waitForBytesWritten(timeoutMs)) {
        return false;
    }

    socket.flush();
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(timeoutMs);
    }
    return true;
}

bool runningInstanceExists()
{
    const QString name = serverName();
    if (name.isEmpty()) {
        return false;
    }

    QLocalSocket socket;
    socket.connectToServer(name);
    const bool connected = socket.waitForConnected(80);
    if (connected) {
        socket.disconnectFromServer();
        if (socket.state() != QLocalSocket::UnconnectedState) {
            socket.waitForDisconnected(80);
        }
    }
    return connected;
}

} // namespace

LaunchOptions parseLaunchOptions(const QStringList& arguments)
{
    LaunchOptions options;

    for (int index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--background")) {
            options.background = true;
        } else if (argument == QStringLiteral("--quit")) {
            options.quit = true;
        } else if (options.initialUrl.isEmpty()) {
            const QFileInfo fileInfo(argument);
            options.initialUrl = fileInfo.exists() && fileInfo.isFile()
                                     ? QUrl::fromLocalFile(fileInfo.absoluteFilePath())
                                     : QUrl::fromUserInput(argument);
        }
    }

    return options;
}

bool forwardLaunchToExistingInstance(const LaunchOptions& options)
{
    if (options.quit) {
        sendMessageToRunningInstance(makeQuitMessage());
        return true;
    }
    if (!options.initialUrl.isEmpty()) {
        return sendMessageToRunningInstance(makeOpenMessage(options.initialUrl));
    }
    if (options.background) {
        return runningInstanceExists();
    }
    return sendMessageToRunningInstance(makeShowMessage());
}

SingleInstanceServer::SingleInstanceServer(WindowManager* windowManager, QObject* parent)
    : QObject(parent), windowManager_(windowManager)
{
    connect(&server_, &QLocalServer::newConnection, this,
            &SingleInstanceServer::acceptPendingConnections);
}

bool SingleInstanceServer::start()
{
    const QString name = serverName();
    if (name.isEmpty()) {
        qWarning() << "Single-instance command sharing is disabled for this launch.";
        return true;
    }

    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (server_.listen(name)) {
        return true;
    }

    // Do not unlink a live listener merely because an earlier forwarding
    // attempt timed out. Only remove the endpoint after it fails a fresh probe.
    if (runningInstanceExists()) {
        qCritical() << "Another Licasa instance owns the command server.";
        return false;
    }

    QLocalServer::removeServer(name);
    if (server_.listen(name)) {
        return true;
    }

    qCritical() << "Unable to start the single-instance command server:" << server_.errorString();
    return false;
}

void SingleInstanceServer::acceptPendingConnections()
{
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        struct ReadState {
            QByteArray buffer;
            bool finished = false;
        };
        const auto state = std::make_shared<ReadState>();
        const auto processAvailablePayload = [this, socket, state](bool endOfStream) {
            if (state->finished) {
                return;
            }

            const qint64 remaining = kMaximumCommandBytes - state->buffer.size();
            if (socket->bytesAvailable() > remaining) {
                state->finished = true;
                qWarning() << "Rejected an oversized single-instance command.";
                socket->abort();
                return;
            }

            state->buffer.append(socket->readAll());
            const qsizetype newline = state->buffer.indexOf('\n');
            if (newline < 0 && !endOfStream) {
                return;
            }

            const qsizetype payloadLength = newline >= 0 ? newline : state->buffer.size();
            const QByteArray payload = state->buffer.left(payloadLength).trimmed();
            state->finished = true;
            if (payload.isEmpty()) {
                return;
            }
            processPayload(payload);
        };

        connect(socket, &QLocalSocket::readyRead, socket,
                [processAvailablePayload]() { processAvailablePayload(false); });
        connect(socket, &QLocalSocket::disconnected, socket, [socket, processAvailablePayload]() {
            processAvailablePayload(true);
            socket->deleteLater();
        });
        connect(socket, &QLocalSocket::errorOccurred, socket,
                [socket](QLocalSocket::LocalSocketError) { socket->deleteLater(); });

        // Data can already be buffered by the time nextPendingConnection()
        // returns. Consume it immediately instead of relying on a later signal.
        processAvailablePayload(false);
    }
}

void SingleInstanceServer::processPayload(const QByteArray& payload)
{
    if (payload.startsWith("OPEN ")) {
        const QUrl url = QUrl::fromEncoded(payload.mid(5).trimmed());
        if (url.isValid() && windowManager_) {
            windowManager_->openInNewWindow(url);
        }
        return;
    }

    if (payload == QByteArrayLiteral("SHOW")) {
        if (windowManager_) {
            windowManager_->showAnyWindow();
        }
        return;
    }

    if (payload == QByteArrayLiteral("QUIT")) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), []() { QCoreApplication::exit(0); },
            Qt::QueuedConnection);
    }
}

} // namespace Licasa
