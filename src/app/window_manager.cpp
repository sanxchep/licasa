#include "app/window_manager.h"
#include "app/window_media_state.h"

#include "app/app_constants.h"
#include "export/animation_export.h"
#include "export/image_save_service.h"
#include "export/motion_photo_export.h"
#include "export/motion_photo_frame_save_coordinator.h"
#include "imaging/async_image_provider.h"
#include "imaging/image_animation.h"
#include "imaging/image_resource_policy.h"
#include "media/motion/motion_photo_session.h"
#include "media/photo_asset_probe.h"
#include "media/preferred_cover_frame_coordinator.h"
#include "platform/graphics_backend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace Licasa {

WindowSession::WindowSession(QUrl initialImageUrl, bool startHidden, bool persistentShell,
                             QObject* parent)
    : QObject(parent), initialImageUrl_(std::move(initialImageUrl)), startHidden_(startHidden),
      persistentShell_(persistentShell)
{}

QUrl WindowSession::initialImageUrl() const { return initialImageUrl_; }

bool WindowSession::startHidden() const { return startHidden_; }

bool WindowSession::persistentShell() const { return persistentShell_; }

void WindowSession::requestOpen(const QUrl& url) { emit openRequested(url); }

WindowManager::WindowManager(QQmlApplicationEngine* engine, QVariantMap sharedWindowProperties,
                             QObject* parent)
    : QObject(parent), engine_(engine),
      component_(engine, QUrl(QString::fromLatin1(Constants::mainQmlUrl)), this),
      sharedWindowProperties_(std::move(sharedWindowProperties))
{}

WindowManager::~WindowManager() { destroyAllWindows(); }

void WindowManager::setMotionPhotoResourcePolicy(MotionPhotoResourcePolicy* resourcePolicy) noexcept
{
    motionPhotoResourcePolicy_ = resourcePolicy;
}

void WindowManager::setImageResourcePolicy(ImageResourcePolicy* resourcePolicy) noexcept
{
    imageResourcePolicy_ = resourcePolicy;
}

void WindowManager::setImageProvider(AsyncImageProvider* provider) noexcept
{
    imageProvider_ = provider;
}

void WindowManager::setImageSaveService(ImageSaveService* saveService) noexcept
{
    imageSaveService_ = saveService;
}

bool WindowManager::createWindow(const QUrl& initialUrl, bool startHidden, bool persistentShell)
{
    if (!engine_) {
        return false;
    }
    if (!component_.isReady()) {
        qCritical() << "Main.qml component is not ready:" << component_.errors();
        return false;
    }
    if (!imageResourcePolicy_) {
        qCritical() << "WindowManager requires ImageResourcePolicy before creating windows.";
        return false;
    }
    if (!imageSaveService_) {
        qCritical() << "WindowManager requires ImageSaveService before creating windows.";
        return false;
    }

    auto session = std::make_unique<WindowSession>(initialUrl, startHidden, persistentShell);
    // Phone-media metadata is intentionally per-window and lazy. A window can
    // cancel its own bounded metadata probe during rapid navigation without
    // interfering with another image window.
    auto photoAssetProbe = std::make_unique<PhotoAssetProbe>();
    auto motionPhotoSession = std::make_unique<MotionPhotoSession>(motionPhotoResourcePolicy_);
    auto mediaState = std::make_shared<WindowMediaState>();
    mediaState->motionSession = motionPhotoSession.get();
    auto motionPhotoFrameSaveCoordinator =
        std::make_unique<MotionPhotoFrameSaveCoordinator>(*motionPhotoSession, *imageSaveService_);
    QPointer<MotionPhotoSession> motionSessionGuard(motionPhotoSession.get());
    photoAssetProbe->setResultHandler(
        [motionSessionGuard, mediaState](const QUrl&, const PhotoAssetInfo& asset) {
            mediaState->motionAsset = asset;
            if (motionSessionGuard) {
                motionSessionGuard->setAsset(asset);
            }
            if (mediaState->motionService) {
                mediaState->motionService->setAsset(asset);
            }
        });
    photoAssetProbe->setInvalidationHandler([motionSessionGuard, mediaState]() {
        mediaState->motionAsset.reset();
        if (motionSessionGuard) {
            motionSessionGuard->clearAsset();
        }
        if (mediaState->motionService) {
            mediaState->motionService->clearAsset();
        }
    });

    QVariantMap initialProperties = sharedWindowProperties_;
    initialProperties.insert(QStringLiteral("windowSession"),
                             QVariant::fromValue(static_cast<QObject*>(session.get())));
    initialProperties.insert(QStringLiteral("windowManager"),
                             QVariant::fromValue(static_cast<QObject*>(this)));
    initialProperties.insert(QStringLiteral("photoAssetProbe"),
                             QVariant::fromValue(static_cast<QObject*>(photoAssetProbe.get())));
    initialProperties.insert(QStringLiteral("motionPhotoSession"),
                             QVariant::fromValue(static_cast<QObject*>(motionPhotoSession.get())));
    initialProperties.insert(QStringLiteral("motionPhotoExportService"),
                             QVariant::fromValue(static_cast<QObject*>(nullptr)));
    initialProperties.insert(QStringLiteral("animationExportService"),
                             QVariant::fromValue(static_cast<QObject*>(nullptr)));
    initialProperties.insert(
        QStringLiteral("motionPhotoFrameSaveCoordinator"),
        QVariant::fromValue(static_cast<QObject*>(motionPhotoFrameSaveCoordinator.get())));

    QObject* object =
        component_.createWithInitialProperties(initialProperties, engine_->rootContext());
    if (!object) {
        qCritical() << "Failed to create Main.qml window:" << component_.errors();
        return false;
    }

    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qCritical() << "Main.qml did not create a QQuickWindow.";
        delete object;
        return false;
    }

    session->setParent(window);
    session.release();
    photoAssetProbe->setParent(window);
    photoAssetProbe.release();
    motionPhotoSession->setParent(window);
    motionPhotoSession.release();
    motionPhotoFrameSaveCoordinator->setParent(window);
    motionPhotoFrameSaveCoordinator.release();
    mediaStates_.insert(window, mediaState);
    registerWindow(window);
    return true;
}

QObject* WindowManager::ensureMotionPhotoExportService(QObject* windowObject)
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) {
        return nullptr;
    }

    const auto it = mediaStates_.find(window);
    if (it == mediaStates_.end()) {
        return nullptr;
    }

    const std::shared_ptr<WindowMediaState>& state = it.value();
    if (!state) {
        return nullptr;
    }
    if (state->motionService) {
        return state->motionService;
    }

    auto* service = new MotionPhotoExportService(window);
    if (state->motionAsset) {
        service->setAsset(*state->motionAsset);
    }
    if (!service->available()) {
        service->deleteLater();
        return nullptr;
    }

    state->motionService = service;
    window->setProperty("motionPhotoExportService",
                        QVariant::fromValue(static_cast<QObject*>(service)));
    return service;
}

QObject* WindowManager::ensureAnimationExportService(QObject* windowObject)
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window || !imageResourcePolicy_) {
        return nullptr;
    }

    const auto it = mediaStates_.find(window);
    if (it == mediaStates_.end()) {
        return nullptr;
    }

    const std::shared_ptr<WindowMediaState>& state = it.value();
    if (!state) {
        return nullptr;
    }
    if (state->animationService) {
        return state->animationService;
    }

    auto* service = new AnimationExportService(*imageResourcePolicy_, window);
    state->animationService = service;
    window->setProperty("animationExportService",
                        QVariant::fromValue(static_cast<QObject*>(service)));
    return service;
}

QObject* WindowManager::ensurePreferredCoverFrameCoordinator(QObject* windowObject)
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) {
        return nullptr;
    }

    const auto it = mediaStates_.find(window);
    if (it == mediaStates_.end()) {
        return nullptr;
    }

    const std::shared_ptr<WindowMediaState>& state = it.value();
    if (!state) {
        return nullptr;
    }

    if (!state->coverCoordinator) {
        state->coverCoordinator = new PreferredCoverFrameCoordinator(window);
    }
    PreferredCoverFrameCoordinator* coordinator = state->coverCoordinator;

    state->disconnectCoverSignals();

    QObject* animationObject = window->property("animationController").value<QObject*>();
    auto* animation = qobject_cast<ImageAnimation*>(animationObject);
    const bool animationMode = window->property("currentImageAnimated").toBool() && animation;

    if (animationMode) {
        state->bindAnimationCover(window, animation);
        return coordinator;
    }

    MotionPhotoSession* motionSession = state->motionSession;
    if (motionSession && motionSession->available() && state->motionAsset &&
        state->motionAsset->kind == PhotoAssetKind::MotionPhoto) {
        state->bindMotionCover();
        return coordinator;
    }

    coordinator->clearSource();
    return nullptr;
}

bool WindowManager::openInNewWindow(const QUrl& url)
{
    if (!url.isValid()) {
        return false;
    }

    pruneDeadWindows();
    for (const QPointer<QQuickWindow>& window : windows_) {
        if (!window) {
            continue;
        }

        auto* session = window->findChild<WindowSession*>(QString(), Qt::FindDirectChildrenOnly);
        if (!session || !session->persistentShell()) {
            continue;
        }

        const QUrl residentUrl = window->property("residentImageUrl").toUrl();
        if (!window->isVisible() || residentUrl.isEmpty()) {
            session->requestOpen(url);
            return true;
        }
    }

    return createWindow(url, false, false);
}

void WindowManager::destroyWindow(QObject* object)
{
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        return;
    }

    releasePictureResources();
    window->hide();
    window->deleteLater();
}

void WindowManager::releasePictureResources()
{
    if (imageProvider_) {
        imageProvider_->releasePictureResources();
    }
}

void WindowManager::markParallelPreviewPresented(const QString& pairId)
{
    if (imageProvider_) {
        imageProvider_->markParallelPreviewPresented(pairId);
    }
}

void WindowManager::destroyAllWindows()
{
    pruneDeadWindows();

    const auto windows = std::exchange(windows_, {});
    for (const QPointer<QQuickWindow>& window : windows) {
        if (!window) {
            continue;
        }

        window->hide();
        delete window.data();
    }
}

void WindowManager::quitApplication() { QCoreApplication::exit(0); }

void WindowManager::showAnyWindow()
{
    pruneDeadWindows();

    for (const QPointer<QQuickWindow>& window : windows_) {
        if (!window) {
            continue;
        }

        window->show();
        window->raise();
        window->requestActivate();
        return;
    }

    createWindow(QUrl(), false, true);
}

void WindowManager::registerWindow(QQuickWindow* window)
{
    pruneDeadWindows();

    // A hidden persistent shell should keep only application state. Decoded
    // photos, textures, and render targets are rebuilt on demand when shown.
    window->setPersistentGraphics(false);
    window->setPersistentSceneGraph(false);

    connect(window, &QWindow::visibleChanged, window, [this, window](bool visible) {
        if (visible) {
            return;
        }
        QTimer::singleShot(0, window, [this, window] {
            if (!window->isVisible() && window->property("residentImageUrl").toUrl().isEmpty()) {
                releasePictureResources();
            }
            const auto* session =
                window->findChild<WindowSession*>(QString(), Qt::FindDirectChildrenOnly);
            if (window->isVisible() || !session || !session->persistentShell() ||
                !window->property("residentImageUrl").toUrl().isEmpty() ||
                !window->isSceneGraphInitialized() ||
                window->rendererInterface()->graphicsApi() != QSGRendererInterface::Software) {
                return;
            }
            // Qt's software loop does not implement releaseResources() and
            // leaves deleted textures pending until its next scene sync. A
            // hidden, empty frame flushes those references through the public
            // API without showing a blank frame or destroying the warm shell.
            // The context-bound callback is cancelled if the window dies.
            (void)window->grabWindow();
        });
    });

    connect(
        window, &QQuickWindow::sceneGraphInitialized, window,
        [window]() { logWindowGraphicsBackend(window, QStringLiteral("sceneGraphInitialized")); },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::sceneGraphInvalidated, window,
        []() { qInfo().noquote() << "[Licasa] Qt Quick scene graph invalidated"; },
        Qt::DirectConnection);
    connect(window, &QQuickWindow::sceneGraphError, window,
            [](QQuickWindow::SceneGraphError error, const QString& message) {
                qWarning() << "[Licasa] Qt Quick scene graph error:" << error << message;
            });

    windows_.append(QPointer<QQuickWindow>(window));
    connect(window, &QObject::destroyed, this, [this, window]() {
        mediaStates_.remove(window);
        pruneDeadWindows();
    });
}

void WindowManager::pruneDeadWindows()
{
    windows_.erase(
        std::remove_if(windows_.begin(), windows_.end(),
                       [](const QPointer<QQuickWindow>& window) { return window.isNull(); }),
        windows_.end());
}

} // namespace Licasa
