#pragma once

#include <QJsonObject>
#include <QString>

class QLocalSocket;

namespace Licasa {

inline constexpr qsizetype maximumLifecycleBytes = 16 * 1024;

bool lifecyclePeerIdentity(QLocalSocket& socket, qint64* pid = nullptr);
bool lifecycleSocketIsOwned(const QString& path);
QJsonObject lifecycleRequest(const QString& operation, const QJsonObject& arguments = {},
                             int timeoutMs = 1000, bool* writeAttempted = nullptr);
int lifecycleResultExitCode(const QJsonObject& reply);
int runLifecycleControl(const QString& operation, int timeoutMs = 10000);
QJsonObject lifecycleError(const QString& result, const QString& message = {});

} // namespace Licasa
