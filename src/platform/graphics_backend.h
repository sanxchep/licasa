#pragma once

class QQuickWindow;
class QString;

namespace Licasa {

void configureSurfaceFormatForRequestedOpenGL();
void logWindowGraphicsBackend(QQuickWindow* window, const QString& stage);

} // namespace Licasa
