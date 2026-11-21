#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
#include "imaging/hdr/output_negotiation.h"
#include "app/application_settings.h"
#include "imaging/hdr/render_target_budget.h"
#endif
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

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QDynamicPropertyChangeEvent>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
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

QUrl WindowManager::defaultImageUrl() const
{
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    for (const auto* relativePath : {
             "../share/licasa/images/licasa-4096x4096.png",
             "generated/default-image/licasa-4096x4096.png",
         }) {
        const QFileInfo image(applicationDirectory.filePath(QString::fromLatin1(relativePath)));
        if (image.isFile()) {
            return QUrl::fromLocalFile(image.absoluteFilePath());
        }
    }
    return {};
}

bool WindowManager::eventFilter(QObject* watched, QEvent* event)
{
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    if (event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "licasaHdrRecoveryState")
        emit lifecycleStateChanged();
#endif
    if (event->type() == QEvent::KeyRelease) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (!key->isAutoRepeat() && key->modifiers() == Qt::NoModifier &&
            (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            emit navigationKeyReleased(watched, key->key() == Qt::Key_Right ? 1 : -1);
        }
    }
    return QObject::eventFilter(watched, event);
}

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
    if (saveService) {
        connect(saveService, &ImageSaveService::busyChanged, this,
                &WindowManager::lifecycleStateChanged);
    }
}

int WindowManager::visibleWindowCount() const
{
    return int(
        std::count_if(windows_.cbegin(), windows_.cend(), [](const QPointer<QQuickWindow>& window) {
            return window && window->isVisible();
        }));
}

QString WindowManager::userWorkBusyReason() const
{
    if (imageSaveService_ && imageSaveService_->busy()) {
        return QStringLiteral("saving");
    }
    for (const auto& state : mediaStates_) {
        if (state && ((state->motionService && state->motionService->busy()) ||
                      (state->animationService && state->animationService->busy()))) {
            return QStringLiteral("exporting");
        }
    }
    for (const QPointer<QQuickWindow>& window : windows_) {
        if (!window) {
            continue;
        }
        const QString reason = window->property("lifecycleBusyReason").toString();
        if (!reason.isEmpty() && reason != QStringLiteral("opening-image")) {
            return reason;
        }
    }
    return {};
}

QString WindowManager::lifecycleBusyReason() const
{
    const QString reason = userWorkBusyReason();
    if (!reason.isEmpty()) {
        return reason;
    }
    for (const QPointer<QQuickWindow>& window : windows_) {
        if (window &&
            window->property("lifecycleBusyReason").toString() == QStringLiteral("opening-image")) {
            return QStringLiteral("opening-image");
        }
    }
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    for (const QPointer<QQuickWindow>& window : windows_) {
        if (window && hdrTargetKeepsWindowAlive(window)) return QStringLiteral("graphics-recovery");
    }
#endif
    return visibleWindowCount() > 0 ? QStringLiteral("visible-window") : QString{};
}

bool WindowManager::launchesAllowed() const { return launchesAllowed_; }

void WindowManager::setLaunchesAllowed(bool allowed)
{
    if (launchesAllowed_ == allowed) {
        return;
    }
    launchesAllowed_ = allowed;
    emit lifecycleStateChanged();
}

void WindowManager::quiesceForLifecycle()
{
    for (const QPointer<QQuickWindow>& window : windows_) {
        if (window) {
            QMetaObject::invokeMethod(window, "stopOptionalWorkForLifecycle", Qt::DirectConnection);
        }
    }
    if (imageSaveService_) {
        imageSaveService_->cancelSizeEstimate();
    }
    releasePictureResources();
}

bool WindowManager::createWindow(const QUrl& initialUrl, bool startHidden, bool persistentShell)
{
    if (!launchesAllowed_) {
        return false;
    }
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

    const QUrl imageUrl = initialUrl.isEmpty() && !startHidden ? defaultImageUrl() : initialUrl;
    if (initialUrl.isEmpty() && !startHidden && imageUrl.isEmpty()) {
        qCritical() << "The bundled default Licasa image is missing.";
        return false;
    }
    auto session = std::make_unique<WindowSession>(imageUrl, startHidden, persistentShell);
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
        // Every open invalidates the probe. Without this, returning to the same
        // Motion Photo keeps the spent binding and its stored cover never applies.
        if (mediaState->coverCoordinator) {
            mediaState->coverCoordinator->clearMotionSource();
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

#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    // Negotiate before Component.onCompleted can create/show the native surface.
    QObject* object = component_.beginCreate(engine_->rootContext());
    if (object) {
        component_.setInitialProperties(object, initialProperties);
        if (auto* candidate = qobject_cast<QQuickWindow*>(object))
            requestHdrOutput(candidate, qobject_cast<ViewerPreferences*>(sharedWindowProperties_.value(QStringLiteral("viewerPreferences")).value<QObject*>()));
        component_.completeCreate();
    }
#else
    QObject* object =
        component_.createWithInitialProperties(initialProperties, engine_->rootContext());
#endif
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
    connect(service, &MotionPhotoExportService::busyChanged, this,
            &WindowManager::lifecycleStateChanged);
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
    connect(service, &AnimationExportService::busyChanged, this,
            &WindowManager::lifecycleStateChanged);
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
    if (!launchesAllowed_ || !url.isValid()) {
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
        const bool uneditedDefault =
            residentUrl == defaultImageUrl() && !window->property("editorHasAdjustments").toBool();
        if (!window->isVisible() || residentUrl.isEmpty() || uneditedDefault) {
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

void WindowManager::releasePictureResourcesForNavigation(const QUrl& nextImage)
{
    if (imageProvider_) {
        imageProvider_->releasePictureResources(true, nextImage);
    }
}

bool WindowManager::hasDecodedFullBase(const QUrl& url) const
{
    return imageProvider_ && imageProvider_->hasDecodedFullBase(url);
}

void WindowManager::prepareNearbyPreviews(const QList<QUrl>& urls, const QSize& requestedSize,
                                          int maximumImageMemoryMiB)
{
    if (launchesAllowed_ && imageProvider_) {
        imageProvider_->prepareNearbyPreviews(urls, requestedSize, maximumImageMemoryMiB);
    }
}

void WindowManager::markParallelPreviewPresented(const QString& pairId)
{
    if (imageProvider_) {
        imageProvider_->markParallelPreviewPresented(pairId);
    }
}

QVariantMap WindowManager::sampleDisplayedPixel(QObject* windowObject, qreal x, qreal y) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window || !window->isVisible() || !std::isfinite(x) || !std::isfinite(y) || x < 0 ||
        y < 0 || x >= window->width() || y >= window->height()) {
        return {};
    }

#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    if (!hdrTargetReadbackReady(window)) return {};
#endif
    // Capture the displayed raster with edits and color management applied.
    // The hover label can cover the next cell, so exclude it from the grab.
    // grabWindow() synchronizes the scene graph before rendering this frame.
    QPointer<QQuickItem> hoverLabel =
        window->findChild<QQuickItem*>(QStringLiteral("pixelCoordinateOverlay"));
    if (hoverLabel) {
        hoverLabel->setProperty("sampleCaptureHidden", true);
    }
    const QImage frame = window->grabWindow();
    if (hoverLabel) {
        hoverLabel->setProperty("sampleCaptureHidden", false);
    }
    if (frame.isNull()) {
        return {};
    }
    const int pixelX = std::clamp(int(x * frame.width() / window->width()), 0, frame.width() - 1);
    const int pixelY =
        std::clamp(int(y * frame.height() / window->height()), 0, frame.height() - 1);
    const QColor color = frame.pixelColor(pixelX, pixelY);
    return {{QStringLiteral("red"), color.red()},
            {QStringLiteral("green"), color.green()},
            {QStringLiteral("blue"), color.blue()},
            {QStringLiteral("alpha"), color.alpha()}};
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

void WindowManager::quitApplication()
{
    const QString reason = userWorkBusyReason();
    if (!reason.isEmpty()) {
        emit quitBlocked(reason);
        return;
    }
    if (isSignalConnected(QMetaMethod::fromSignal(&WindowManager::quitRequested))) {
        emit quitRequested();
    } else {
        QCoreApplication::exit(0);
    }
}

void WindowManager::showAnyWindow()
{
    if (!launchesAllowed_) {
        return;
    }
    pruneDeadWindows();

    for (const QPointer<QQuickWindow>& window : windows_) {
        if (!window) {
            continue;
        }

        if (window->property("residentImageUrl").toUrl().isEmpty()) {
            auto* session =
                window->findChild<WindowSession*>(QString(), Qt::FindDirectChildrenOnly);
            if (session) {
                session->requestOpen(defaultImageUrl());
                return;
            }
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
    window->installEventFilter(this);

    // A hidden persistent shell should keep only application state. Decoded
    // photos, textures, and render targets are rebuilt on demand when shown.
    window->setPersistentGraphics(false);
    window->setPersistentSceneGraph(false);

    connect(window, &QWindow::visibleChanged, window, [this, window](bool visible) {
        emit lifecycleStateChanged();
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
    connect(window, SIGNAL(lifecycleBusyReasonChanged()), this, SIGNAL(lifecycleStateChanged()));

    connect(
        window, &QQuickWindow::sceneGraphInitialized, window,
        [window]() { logWindowGraphicsBackend(window, QStringLiteral("sceneGraphInitialized")); },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::sceneGraphInitialized, this,
        [this]() { graphicsInitialized_ = true; }, Qt::QueuedConnection);
    connect(
        window, &QQuickWindow::sceneGraphInvalidated, window,
        []() { qInfo().noquote() << "[Licasa] Qt Quick scene graph invalidated"; },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::sceneGraphError, this,
        [this](QQuickWindow::SceneGraphError error, const QString& message) {
            qWarning() << "[Licasa] Qt Quick scene graph error:" << error << message;
            if (!graphicsInitialized_) {
                emit initialGraphicsFailed(message);
            }
        },
        Qt::QueuedConnection);

    windows_.append(QPointer<QQuickWindow>(window));
    emit lifecycleStateChanged();
    connect(window, &QObject::destroyed, this, [this, window]() {
        mediaStates_.remove(window);
        pruneDeadWindows();
        emit lifecycleStateChanged();
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
