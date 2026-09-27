#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QUrl>
#include <QVariantMap>

#include <memory>

class QQmlApplicationEngine;
class QQuickWindow;

namespace Licasa {

class ImageResourcePolicy;
class AsyncImageProvider;
class AnimationExportService;
class MotionPhotoExportService;
class PreferredCoverFrameCoordinator;
struct WindowMediaState;
class ImageSaveService;
class MotionPhotoResourcePolicy;

class WindowSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl initialImageUrl READ initialImageUrl CONSTANT)
    Q_PROPERTY(bool startHidden READ startHidden CONSTANT)
    Q_PROPERTY(bool persistentShell READ persistentShell CONSTANT)

  public:
    WindowSession(QUrl initialImageUrl, bool startHidden, bool persistentShell,
                  QObject* parent = nullptr);

    QUrl initialImageUrl() const;
    bool startHidden() const;
    bool persistentShell() const;
    void requestOpen(const QUrl& url);

  signals:
    void openRequested(const QUrl& url);

  private:
    QUrl initialImageUrl_;
    bool startHidden_ = false;
    bool persistentShell_ = false;
};

class WindowManager final : public QObject {
    Q_OBJECT

  public:
    WindowManager(QQmlApplicationEngine* engine, QVariantMap sharedWindowProperties,
                  QObject* parent = nullptr);
    ~WindowManager() override;

    void setMotionPhotoResourcePolicy(MotionPhotoResourcePolicy* resourcePolicy) noexcept;
    void setImageResourcePolicy(ImageResourcePolicy* resourcePolicy) noexcept;
    void setImageProvider(AsyncImageProvider* provider) noexcept;
    void setImageSaveService(ImageSaveService* saveService) noexcept;

    Q_INVOKABLE bool createWindow(const QUrl& initialUrl = QUrl(), bool startHidden = false,
                                  bool persistentShell = false);
    Q_INVOKABLE bool openInNewWindow(const QUrl& url);
    Q_INVOKABLE QObject* ensureMotionPhotoExportService(QObject* windowObject);
    Q_INVOKABLE QObject* ensureAnimationExportService(QObject* windowObject);
    Q_INVOKABLE QObject* ensurePreferredCoverFrameCoordinator(QObject* windowObject);
    Q_INVOKABLE void releasePictureResources();
    Q_INVOKABLE void markParallelPreviewPresented(const QString& pairId);
    Q_INVOKABLE void destroyWindow(QObject* object);
    Q_INVOKABLE void destroyAllWindows();

    void showAnyWindow();

  private:
    void registerWindow(QQuickWindow* window);
    void pruneDeadWindows();

    QQmlApplicationEngine* engine_ = nullptr;
    QQmlComponent component_;
    QVariantMap sharedWindowProperties_;
    MotionPhotoResourcePolicy* motionPhotoResourcePolicy_ = nullptr;
    ImageResourcePolicy* imageResourcePolicy_ = nullptr;
    AsyncImageProvider* imageProvider_ = nullptr;
    ImageSaveService* imageSaveService_ = nullptr;
    QList<QPointer<QQuickWindow>> windows_;
    QHash<QQuickWindow*, std::shared_ptr<WindowMediaState>> mediaStates_;
};

} // namespace Licasa
