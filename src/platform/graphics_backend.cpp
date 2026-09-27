#include "platform/graphics_backend.h"

#include <QByteArray>
#include <QColorSpace>
#include <QDebug>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QSurfaceFormat>

namespace Licasa {
namespace {

QSGRendererInterface::GraphicsApi parseGraphicsApiEnvironmentValue(QByteArray value)
{
    value = value.trimmed().toLower();

    if (value.isEmpty()) {
        return QSGRendererInterface::Unknown;
    }
    if (value == QByteArrayLiteral("opengl")) {
        return QSGRendererInterface::OpenGL;
    }
    if (value == QByteArrayLiteral("vulkan")) {
        return QSGRendererInterface::Vulkan;
    }
    if (value == QByteArrayLiteral("metal")) {
        return QSGRendererInterface::Metal;
    }
    if (value == QByteArrayLiteral("d3d11")) {
        return QSGRendererInterface::Direct3D11;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (value == QByteArrayLiteral("d3d12")) {
        return QSGRendererInterface::Direct3D12;
    }
#endif
    return QSGRendererInterface::Unknown;
}

QString graphicsApiToString(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Unknown:
        return QStringLiteral("Unknown");
    case QSGRendererInterface::Software:
        return QStringLiteral("Software");
    case QSGRendererInterface::OpenVG:
        return QStringLiteral("OpenVG");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("OpenGL");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("Direct3D11");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("Vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("Metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("Null");
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("Direct3D12");
#endif
    }

    return QStringLiteral("Unknown");
}

QString requestedBackendForLogging()
{
    const QString quickBackend = QString::fromLatin1(qgetenv("QT_QUICK_BACKEND")).trimmed();
    const QString rhiBackend = QString::fromLatin1(qgetenv("QSG_RHI_BACKEND")).trimmed();
    if (quickBackend.isEmpty() && rhiBackend.isEmpty()) {
        return QStringLiteral("<auto>");
    }
    if (quickBackend.isEmpty()) {
        return QStringLiteral("QSG_RHI_BACKEND=%1").arg(rhiBackend);
    }
    if (rhiBackend.isEmpty()) {
        return QStringLiteral("QT_QUICK_BACKEND=%1").arg(quickBackend);
    }
    return QStringLiteral("QT_QUICK_BACKEND=%1, QSG_RHI_BACKEND=%2").arg(quickBackend, rhiBackend);
}

QString sceneGraphBackendForLogging()
{
    const QString backend = QQuickWindow::sceneGraphBackend().trimmed();
    return backend.isEmpty() ? QStringLiteral("<default>") : backend;
}

} // namespace

void configureSurfaceFormatForRequestedOpenGL()
{
    if (parseGraphicsApiEnvironmentValue(qgetenv("QSG_RHI_BACKEND")) !=
        QSGRendererInterface::OpenGL) {
        return;
    }

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setSwapInterval(1);
    format.setSamples(0);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(8);
    format.setColorSpace(QColorSpace::SRgb);
    QSurfaceFormat::setDefaultFormat(format);
}

void logWindowGraphicsBackend(QQuickWindow* window, const QString& stage)
{
    if (!window || !window->rendererInterface()) {
        return;
    }

    const auto api = window->rendererInterface()->graphicsApi();
    const QString message =
        QStringLiteral("[Licasa] Qt Quick backend (%1): graphicsApi=%2, "
                       "sceneGraphBackend=%3, requestedByEnv=%4, rhiBased=%5")
            .arg(stage, graphicsApiToString(api), sceneGraphBackendForLogging(),
                 requestedBackendForLogging(),
                 QSGRendererInterface::isApiRhiBased(api) ? QStringLiteral("true")
                                                          : QStringLiteral("false"));

    qInfo().noquote() << message;
}

} // namespace Licasa
