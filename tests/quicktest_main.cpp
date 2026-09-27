#include "app/application_settings.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_animation.h"
#include "imaging/image_resource_policy.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest/quicktest.h>
#include <memory>

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
        engine->addImageProvider("animation", animations_->createFrameProvider());
        engine->rootContext()->setContextProperty("testAnimationService", animations_.get());
    }

  private:
    std::unique_ptr<Licasa::ViewerPreferences> preferences_;
    std::unique_ptr<Licasa::ImageResourcePolicy> policy_;
    std::unique_ptr<Licasa::ImageAnimationService> animations_;
};

QUICK_TEST_MAIN_WITH_SETUP(licasa, LicasaQuickSetup)

#include "quicktest_main.moc"
