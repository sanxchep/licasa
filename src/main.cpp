/*!
 * \file main.cpp
 * \brief Application composition root for Licasa.
 * \author Sanxchep
 */

#include "app/app_constants.h"
#include "app/application_lifecycle.h"
#include "app/application_settings.h"
#include "app/language_manager.h"
#include "app/runtime_error_log.h"
#include "app/window_manager.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/format_support.h"
#include "imaging/image_animation.h"
#include "imaging/image_probe.h"
#include "imaging/image_resource_policy.h"
#include "platform/application_identity.h"
#include "platform/graphics_backend.h"
#include "platform/lifecycle_control.h"
#include "platform/lifecycle_launch_gate.h"
#include "platform/native_window_ops.h"
#include "platform/single_instance.h"

#include <QCoreApplication>
#include <QDate>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <memory>

int main(int argc, char* argv[])
{
    // Package control must work without a display, renderer, QML or codecs.
    QStringList rawArguments;
    for (int index = 0; index < argc; ++index) {
        rawArguments.append(QString::fromLocal8Bit(argv[index]));
    }
    const int controlIndex = rawArguments.indexOf(QStringLiteral("--lifecycle"));
    if (controlIndex >= 0 || rawArguments.contains(QStringLiteral("--quit"))) {
        QCoreApplication control(argc, argv);
        control.setApplicationName(QString::fromLatin1(Licasa::Constants::applicationName));
        control.setOrganizationName(QString::fromLatin1(Licasa::Constants::organizationName));
        const QString operation =
            controlIndex >= 0 ? rawArguments.value(controlIndex + 1) : QStringLiteral("quit");
        return Licasa::runLifecycleControl(operation);
    }
    Licasa::LifecycleLaunchGate launchGate;
    QString gateError;
    if (!launchGate.acquire(&gateError)) {
        qCritical().noquote() << gateError;
        return 1;
    }
    constexpr int softwareRetryExitCode = 42;
    Licasa::RuntimeErrorLog runtimeErrorLog;
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    Licasa::configureSurfaceFormatForRequestedOpenGL();

    QGuiApplication application(argc, argv);
    Licasa::configureSurfaceFormatForPlatform(QGuiApplication::platformName());
    application.setApplicationName(QString::fromLatin1(Licasa::Constants::applicationName));
    application.setApplicationDisplayName(QString::fromLatin1(Licasa::Constants::applicationName));
    application.setOrganizationName(QString::fromLatin1(Licasa::Constants::organizationName));
    application.setDesktopFileName(QString::fromLatin1(Licasa::Constants::desktopFileName));
    application.setWindowIcon(QIcon(QStringLiteral(":/assets/licasa.png")));

    const bool installedAsSnap = qEnvironmentVariableIsSet("SNAP_NAME");
    const QString snapVersion = installedAsSnap ? qEnvironmentVariable("SNAP_VERSION") : QString{};
    const QString installedVersion =
        snapVersion.isEmpty() ? QStringLiteral(LICASA_VERSION) : snapVersion;
    application.setApplicationVersion(installedVersion);
    const QDate releaseDate = QDate::fromString(QStringLiteral(LICASA_RELEASE_DATE), Qt::ISODate);
    const QVariantMap appInfo{{QStringLiteral("version"), installedVersion},
                              {QStringLiteral("installedAsSnap"), installedAsSnap},
                              {QStringLiteral("releaseDate"),
                               QLocale().toString(releaseDate, QStringLiteral("d MMMM yyyy"))}};

    const Licasa::LaunchOptions options = Licasa::parseLaunchOptions(application.arguments());
    const QString endpoint = Licasa::lifecycleEndpointName();
#if defined(Q_OS_LINUX)
    std::unique_ptr<Licasa::RuntimeStartupLock> startupLock;
#else
    std::unique_ptr<QLockFile> startupLock;
#endif
    if (!endpoint.isEmpty()) {
#if defined(Q_OS_LINUX)
        startupLock = std::make_unique<Licasa::RuntimeStartupLock>();
        QString startupError;
        if (!startupLock->acquire(endpoint + QStringLiteral(".launch"), &startupError)) {
            qCritical().noquote() << startupError;
            return 1;
        }
#else
        startupLock = std::make_unique<QLockFile>(endpoint + QStringLiteral(".launch"));
        if (!startupLock->tryLock(2000)) {
            qCritical() << "Another Licasa launch is in progress. Retry shortly.";
            return 1;
        }
#endif
    }
    const auto forwarding = Licasa::forwardLaunchToExistingInstance(options);
    if (forwarding != Licasa::ForwardLaunchResult::NoInstance) {
        return forwarding == Licasa::ForwardLaunchResult::Forwarded ? 0 : 1;
    }
    Licasa::initializeImagePlugins();

    Licasa::BackgroundModeManager backgroundModeManager;
    bool autostartSynchronized = false;
    if (options.background && options.initialUrl.isEmpty()) {
        backgroundModeManager.syncAutostart();
        autostartSynchronized = true;
        if (!backgroundModeManager.enabled()) {
            return 0;
        }
    }
    Licasa::ViewerPreferences viewerPreferences;
    Licasa::LanguageManager languageManager;
    languageManager.setReleaseDate(releaseDate);
    Licasa::ImageResourcePolicy imageResourcePolicy(viewerPreferences);
    Licasa::ImageProbe imageProbe(imageResourcePolicy);
    Licasa::FormatSupport formatSupport;
    Licasa::ImageSaveService imageSaveService(imageResourcePolicy);
    NativeWindowOps nativeWindowOps;
    auto* decodeProvider = new Licasa::AsyncImageProvider(imageResourcePolicy);
    Licasa::ImageAnimationService imageAnimationService(imageResourcePolicy, *decodeProvider);
    QQmlApplicationEngine engine;
    QObject::connect(&languageManager, &Licasa::LanguageManager::languageChanged, &engine, [&] {
        formatSupport.retranslate();
        engine.retranslate();
    });
    engine.addImageProvider(QStringLiteral("animation"),
                            imageAnimationService.createFrameProvider());

    const QVariantMap sharedWindowProperties{
        {QStringLiteral("languageManager"),
         QVariant::fromValue(static_cast<QObject*>(&languageManager))},
        {QStringLiteral("appInfo"), appInfo},
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
    bool graphicsFailureHandled = false;
    QObject::connect(&windowManager, &Licasa::WindowManager::initialGraphicsFailed, &application,
                     [&](const QString& message) {
                         if (graphicsFailureHandled) {
                             return;
                         }
                         graphicsFailureHandled = true;
                         const bool mayRetry = qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND") &&
                                               qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND") &&
                                               !qEnvironmentVariableIsSet("LICASA_SOFTWARE_RETRY");
                         if (mayRetry) {
                             qWarning().noquote()
                                 << "[Licasa] Graphics initialization failed; retrying once "
                                    "with the Qt software renderer:"
                                 << message;
                         } else {
                             qCritical().noquote()
                                 << "[Licasa] Graphics initialization failed with no "
                                    "automatic retry available:"
                                 << message;
                         }
                         QTimer::singleShot(0, &application, [mayRetry]() {
                             QCoreApplication::exit(mayRetry ? softwareRetryExitCode : 1);
                         });
                     });
    windowManager.setMotionPhotoResourcePolicy(&imageResourcePolicy);
    windowManager.setImageResourcePolicy(&imageResourcePolicy);
    windowManager.setImageProvider(decodeProvider);
    windowManager.setImageSaveService(&imageSaveService);
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &windowManager,
                     &Licasa::WindowManager::destroyAllWindows);

    engine.addImageProvider(QString::fromLatin1(Licasa::Constants::imageProviderName),
                            decodeProvider);

    Licasa::ApplicationLifecycle lifecycle(&backgroundModeManager, &windowManager);
    Licasa::SingleInstanceServer singleInstanceServer(&windowManager, &lifecycle);
    if (!singleInstanceServer.start()) {
        return -1;
    }

    const bool startHidden =
        options.background && backgroundModeManager.enabled() && options.initialUrl.isEmpty();
    if (!windowManager.createWindow(options.initialUrl, startHidden, true)) {
        return -1;
    }

    startupLock.reset();
    launchGate.release();
    // Reading consent is sufficient to open a viewer. Its durable migration
    // and desktop-entry maintenance need not delay the first image. Flush on
    // early normal exit as well, so a quick Quit still preserves the opt-in.
    const auto syncAutostart = [&] {
        if (!autostartSynchronized) {
            backgroundModeManager.syncAutostart();
            autostartSynchronized = true;
        }
    };
    // Prime the immutable executable receipt after initial painting, off the
    // GUI thread. Cold launch needs only the cheap installation identity;
    // an earlier status request can still compute the exact receipt on demand.
    QTimer::singleShot(1000, &application, [] {
        QThreadPool::globalInstance()->start([] { (void)Licasa::applicationIdentity(); });
    });
    QTimer::singleShot(1000, &application, syncAutostart);
    const int exitCode = application.exec();
    syncAutostart();
    decodeProvider->waitForPendingWork();
    if (exitCode == softwareRetryExitCode) {
        singleInstanceServer.stop();
        windowManager.destroyAllWindows();
        QProcess retry;
        retry.setProgram(Licasa::applicationLaunchPath());
        retry.setArguments(application.arguments().mid(1));
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
        environment.insert(QStringLiteral("LICASA_SOFTWARE_RETRY"), QStringLiteral("1"));
        retry.setProcessEnvironment(environment);
        if (!retry.startDetached()) {
            qCritical() << "[Licasa] Could not start the software renderer retry.";
            return 1;
        }
        return 0;
    }
    return exitCode;
}
