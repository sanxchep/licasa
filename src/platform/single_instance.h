#pragma once

#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QStringList>
#include <QUrl>

namespace Licasa {

class WindowManager;
class ApplicationLifecycle;

struct LaunchOptions {
    QUrl initialUrl;
    bool background = false;
};

LaunchOptions parseLaunchOptions(const QStringList& arguments);
enum class ForwardLaunchResult { NoInstance, Forwarded, Blocked };
ForwardLaunchResult forwardLaunchToExistingInstance(const LaunchOptions& options);

class SingleInstanceServer final : public QObject {
    Q_OBJECT

  public:
    explicit SingleInstanceServer(WindowManager* windowManager, ApplicationLifecycle* lifecycle,
                                  QObject* parent = nullptr);

    bool start();
    void stop();

  private slots:
    void acceptPendingConnections();

  private:
    QJsonObject processPayload(const QByteArray& payload);

    WindowManager* windowManager_ = nullptr;
    ApplicationLifecycle* lifecycle_ = nullptr;
    QLocalServer server_;
};

} // namespace Licasa
