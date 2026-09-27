#pragma once

#include <QLocalServer>
#include <QObject>
#include <QStringList>
#include <QUrl>

namespace Licasa {

class WindowManager;

struct LaunchOptions {
    QUrl initialUrl;
    bool background = false;
    bool quit = false;
};

LaunchOptions parseLaunchOptions(const QStringList& arguments);
bool forwardLaunchToExistingInstance(const LaunchOptions& options);

class SingleInstanceServer final : public QObject {
    Q_OBJECT

  public:
    explicit SingleInstanceServer(WindowManager* windowManager, QObject* parent = nullptr);

    bool start();

  private slots:
    void acceptPendingConnections();

  private:
    void processPayload(const QByteArray& payload);

    WindowManager* windowManager_ = nullptr;
    QLocalServer server_;
};

} // namespace Licasa
