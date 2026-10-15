#pragma once

#include <QMessageLogContext>
#include <QMutex>
#include <QString>
#include <QtGlobal>

namespace Licasa {

QString defaultRuntimeErrorLogPath();

class RuntimeErrorLog final {
  public:
    explicit RuntimeErrorLog(QString path = {});
    ~RuntimeErrorLog();

    RuntimeErrorLog(const RuntimeErrorLog&) = delete;
    RuntimeErrorLog& operator=(const RuntimeErrorLog&) = delete;

    QString path() const;

  private:
    static void handleMessage(QtMsgType type, const QMessageLogContext& context,
                              const QString& message);
    void append(QtMsgType type, const QString& message);

    QString path_;
    QMutex mutex_;
    QtMessageHandler previousHandler_ = nullptr;
    bool installed_ = false;
};

} // namespace Licasa
