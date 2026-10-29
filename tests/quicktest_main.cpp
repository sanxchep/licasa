#include "app/application_settings.h"
#include "editor_test_support.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_animation.h"
#include "imaging/image_resource_policy.h"
#include <QColor>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QUrlQuery>
#include <QtQuickTest/quicktest.h>
#include <atomic>
#include <memory>

struct PreviewRequestState {
    struct Request {
        int id;
        QString source;
        QSize requested;
        QPointer<QQuickImageResponse> response;
        bool cancelled = false;
        bool finished = false;
    };
    QMutex mutex;
    QList<Request> requests;
    int nextId = 1;
};

class ControlledPreviewResponse final : public QQuickImageResponse {
  public:
    ControlledPreviewResponse(std::shared_ptr<PreviewRequestState> state, QString source,
                              QSize requested)
        : state_(std::move(state)), source_(std::move(source))
    {
        QMutexLocker lock(&state_->mutex);
        id_ = state_->nextId++;
        state_->requests.append({id_, source_, requested, this});
    }
    QQuickTextureFactory* textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(image_);
    }
    QString errorString() const override { return error_; }
    void cancel() override
    {
        {
            QMutexLocker lock(&state_->mutex);
            for (auto& request : state_->requests) {
                if (request.id == id_) {
                    request.cancelled = true;
                }
            }
        }
        QMetaObject::invokeMethod(this, [this] { publish(true); }, Qt::QueuedConnection);
    }
    void publish(bool fail)
    {
        if (published_.exchange(true)) {
            return;
        }
        if (fail) {
            error_ = QStringLiteral("Controlled image response failed");
        } else {
            const bool full =
                QUrlQuery(QUrl(QStringLiteral("image://previewTest/") + source_))
                    .queryItemValue(QStringLiteral("licasa_stage")) == QStringLiteral("full");
            image_ = QImage(full ? QSize(640, 480) : QSize(160, 120), QImage::Format_RGB32);
            image_.fill(full ? QColor(Qt::blue) : QColor(Qt::green));
        }
        {
            QMutexLocker lock(&state_->mutex);
            for (auto& request : state_->requests) {
                if (request.id == id_) {
                    request.finished = true;
                }
            }
        }
        emit finished();
    }

  private:
    std::shared_ptr<PreviewRequestState> state_;
    QString source_;
    int id_ = 0;
    QImage image_;
    QString error_;
    std::atomic_bool published_ = false;
};

class PreviewRequestController final : public QObject {
    Q_OBJECT
  public:
    const std::shared_ptr<PreviewRequestState> state = std::make_shared<PreviewRequestState>();

    Q_INVOKABLE QVariantList requests() const
    {
        QMutexLocker lock(&state->mutex);
        QVariantList values;
        for (const auto& request : state->requests) {
            values.append(QVariantMap{{"id", request.id},
                                      {"source", request.source},
                                      {"width", request.requested.width()},
                                      {"height", request.requested.height()},
                                      {"cancelled", request.cancelled},
                                      {"finished", request.finished}});
        }
        return values;
    }
    Q_INVOKABLE void reset()
    {
        QMutexLocker lock(&state->mutex);
        state->requests.clear();
    }
    Q_INVOKABLE bool release(int id, bool fail)
    {
        QPointer<QQuickImageResponse> response;
        // Finish/cancel records are written before the response can be deleted.
        // Keep that lock through queued delivery so a late test release cannot
        // race deletion of a response which was already cancelled.
        QMutexLocker lock(&state->mutex);
        for (const auto& request : state->requests) {
            if (request.id == id && !request.cancelled && !request.finished) {
                response = request.response;
            }
        }
        if (!response) {
            return false;
        }
        return QMetaObject::invokeMethod(
            response.data(),
            [response, fail] {
                if (response) {
                    static_cast<ControlledPreviewResponse*>(response.data())->publish(fail);
                }
            },
            Qt::QueuedConnection);
    }
};

class ControlledPreviewProvider final : public QQuickAsyncImageProvider {
  public:
    explicit ControlledPreviewProvider(std::shared_ptr<PreviewRequestState> state)
        : state_(std::move(state))
    {}
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requested) override
    {
        return new ControlledPreviewResponse(state_, id, requested);
    }

  private:
    std::shared_ptr<PreviewRequestState> state_;
};

class LicasaQuickSetup final : public QObject {
    Q_OBJECT
  public:
    LicasaQuickSetup() { Licasa::ImageResourcePolicy::initializeDecoderEnvironment(); }
  public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        Licasa::initializeImagePlugins();
        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        policy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);
        auto* provider = new Licasa::AsyncImageProvider(*policy_);
        animations_ = std::make_unique<Licasa::ImageAnimationService>(*policy_, *provider);
        engine->addImageProvider("testStill", provider);
        engine->addImageProvider("licasa", new Licasa::AsyncImageProvider(*policy_));
        saveService_ = std::make_unique<Licasa::ImageSaveService>(*policy_);
        engine->addImageProvider("animation", animations_->createFrameProvider());
        engine->addImageProvider("previewTest",
                                 new ControlledPreviewProvider(previewRequests_.state));
        engine->rootContext()->setContextProperty("testAnimationService", animations_.get());
        engine->rootContext()->setContextProperty("testPreviewRequests", &previewRequests_);
        engine->rootContext()->setContextProperty("testEditor", &editorSupport_);
        engine->rootContext()->setContextProperty("testImageSaveService", saveService_.get());
    }

  private:
    std::unique_ptr<Licasa::ViewerPreferences> preferences_;
    std::unique_ptr<Licasa::ImageResourcePolicy> policy_;
    std::unique_ptr<Licasa::ImageAnimationService> animations_;
    PreviewRequestController previewRequests_;
    EditorTestSupport editorSupport_;
    std::unique_ptr<Licasa::ImageSaveService> saveService_;
};

QUICK_TEST_MAIN_WITH_SETUP(licasa, LicasaQuickSetup)

#include "quicktest_main.moc"
