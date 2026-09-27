#pragma once

#include <QJsonObject>
#include <QtGlobal>

#include <memory>

class QQuickWindow;
class QUrl;

namespace LicasaDiagnostics {

class DiagnosticCheck {
  public:
    virtual ~DiagnosticCheck() = default;
    virtual bool attach(QQuickWindow* window) = 0;
};

qint64 monotonicNs();
QJsonObject processMetrics();
void printJson(const QJsonObject& value);

std::unique_ptr<DiagnosticCheck> makeRawBehaviorCheck(bool previewOnly, bool editOnly);
std::unique_ptr<DiagnosticCheck> makeMotionPhotoControlsCheck();
std::unique_ptr<DiagnosticCheck> makeViewerMeasurement(qint64 started, const QUrl& imageUrl,
                                                       int cycles);

} // namespace LicasaDiagnostics
