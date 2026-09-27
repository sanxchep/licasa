/*!
 * \file main.cpp
 * \brief Application composition root for Licasa.
 * \author Sanxchep
 */

#include "app/app_constants.h"
#include "app/application_settings.h"
#include "app/window_manager.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/format_support.h"
#include "imaging/image_animation.h"
#include "imaging/image_probe.h"
#include "imaging/image_resource_policy.h"
#include "platform/graphics_backend.h"
#include "platform/native_window_ops.h"
#include "platform/single_instance.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QVariantMap>

int main(int argc, char* argv[])
{
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    Licasa::configureSurfaceFormatForRequestedOpenGL();

    QGuiApplication application(argc, argv);
    Licasa::initializeImagePlugins();
    application.setApplicationName(QString::fromLatin1(Licasa::Constants::applicationName));
    application.setApplicationDisplayName(QString::fromLatin1(Licasa::Constants::applicationName));
    application.setOrganizationName(QString::fromLatin1(Licasa::Constants::organizationName));
    application.setDesktopFileName(QString::fromLatin1(Licasa::Constants::desktopFileName));
    application.setWindowIcon(QIcon(QStringLiteral(":/assets/licasa.png")));

    const Licasa::LaunchOptions options = Licasa::parseLaunchOptions(application.arguments());
    if (Licasa::forwardLaunchToExistingInstance(options)) {
        return 0;
    }

    Licasa::BackgroundModeManager backgroundModeManager;
    Licasa::ViewerPreferences viewerPreferences;
    Licasa::ImageResourcePolicy imageResourcePolicy(viewerPreferences);
    Licasa::ImageProbe imageProbe(imageResourcePolicy);
    Licasa::FormatSupport formatSupport;
    Licasa::ImageSaveService imageSaveService(imageResourcePolicy);
    NativeWindowOps nativeWindowOps;
    auto* decodeProvider = new Licasa::AsyncImageProvider(imageResourcePolicy);
    Licasa::ImageAnimationService imageAnimationService(imageResourcePolicy, *decodeProvider);
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("animation"),
                            imageAnimationService.createFrameProvider());

    const QVariantMap sharedWindowProperties{
        {QStringLiteral("imageProbe"), QVariant::fromValue(static_cast<QObject*>(&imageProbe))},
        {QStringLiteral("formatSupport"),
         QVariant::fromValue(static_cast<QObject*>(&formatSupport))},
        {QStringLiteral("imageSaveService"),
         QVariant::fromValue(static_cast<QObject*>(&imageSaveService))},
        {QStringLiteral("imageAnimationService"),
         QVariant::fromValue(static_cast<QObject*>(&imageAnimationService))},
        {QStringLiteral("nativeWindowOps"),
         QVariant::fromValue(static_cast<QObject*>(&nativeWindowOps))},
        {QStringLiteral("backgroundModeManager"),
         QVariant::fromValue(static_cast<QObject*>(&backgroundModeManager))},
        {QStringLiteral("viewerPreferences"),
         QVariant::fromValue(static_cast<QObject*>(&viewerPreferences))},
    };

    Licasa::WindowManager windowManager(&engine, sharedWindowProperties);
    windowManager.setMotionPhotoResourcePolicy(&imageResourcePolicy);
    windowManager.setImageResourcePolicy(&imageResourcePolicy);
    windowManager.setImageProvider(decodeProvider);
    windowManager.setImageSaveService(&imageSaveService);
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &windowManager,
                     &Licasa::WindowManager::destroyAllWindows);

    engine.addImageProvider(QString::fromLatin1(Licasa::Constants::imageProviderName),
                            decodeProvider);

    Licasa::SingleInstanceServer singleInstanceServer(&windowManager);
    if (!singleInstanceServer.start()) {
        return -1;
    }

    const bool startHidden = options.background && options.initialUrl.isEmpty();
    if (!windowManager.createWindow(options.initialUrl, startHidden, true)) {
        return -1;
    }

    const int exitCode = application.exec();
    decodeProvider->waitForPendingWork();
    return exitCode;
}
