#include "app/application_settings.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_animation.h"
#include "imaging/image_resource_policy.h"
#include <QGuiApplication>
#include <QImage>
#include <QQuickImageProvider>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

namespace {
QUrl gif()
{
    return QUrl::fromLocalFile(QStringLiteral(
        LICASA_SOURCE_DIR "/tests/test-assets/animated/gif/standard-transparent-320x240.gif"));
}
QUrl jxl(const char* name)
{
    return QUrl::fromLocalFile(QStringLiteral(LICASA_SOURCE_DIR "/tests/test-assets/modern/jxl/") +
                               QString::fromLatin1(name));
}
struct Viewer {
    Licasa::ViewerPreferences preferences;
    Licasa::ImageResourcePolicy policy{preferences};
    std::unique_ptr<Licasa::AsyncImageProvider> pool{new Licasa::AsyncImageProvider(policy)};
    std::unique_ptr<Licasa::ImageAnimationService> service{
        new Licasa::ImageAnimationService(policy, *pool)};
    std::unique_ptr<QQuickImageProvider> frames{service->createFrameProvider()};
    std::unique_ptr<QObject> owner{new QObject};
    Licasa::ImageAnimation* animation =
        qobject_cast<Licasa::ImageAnimation*>(service->createController(owner.get()));
    ~Viewer()
    {
        owner.reset();
        pool.reset(); // service remains alive until all worker callbacks finish
    }
    QImage frame() const
    {
        QSize size;
        return frames->requestImage(animation->frameSource().path().mid(1), &size, {});
    }
};
} // namespace

class ImageAnimationTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("LicasaTests");
        QCoreApplication::setApplicationName("ImageAnimationTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }
    void sharesTheProcessingGateWithoutBlockingUi()
    {
        Viewer viewer;
        QSignalSpy ready(viewer.animation, &Licasa::ImageAnimation::frameChanged);
        int heartbeats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
        heartbeat.start(10);
        viewer.policy.processingMutex().lock();
        viewer.animation->setSource(gif());
        viewer.animation->setActive(true);
        QTest::qWait(120);
        const bool stillWaiting = viewer.animation->frameSource().isEmpty();
        const bool responsive = heartbeats >= 3;
        viewer.policy.processingMutex().unlock();
        QVERIFY(stillWaiting);
        QVERIFY(responsive);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.animation->frameSource().isEmpty(), 5000);
        QVERIFY(viewer.animation->error().isEmpty());
        const int first = viewer.animation->currentFrame();
        QTRY_VERIFY_WITH_TIMEOUT(viewer.animation->currentFrame() != first, 5000);
        QVERIFY(!viewer.frame().isNull());
    }
    void hiddenAndDestroyedViewersDiscardQueuedWork()
    {
        Viewer viewer;
        viewer.policy.processingMutex().lock();
        viewer.animation->setSource(gif());
        viewer.animation->setActive(true);
        viewer.animation->setActive(false);
        viewer.policy.processingMutex().unlock();
        QTest::qWait(150);
        QVERIFY(viewer.animation->frameSource().isEmpty());
        QVERIFY(viewer.animation->error().isEmpty());
        viewer.animation->setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.animation->frameSource().isEmpty(), 5000);
        const QUrl old = viewer.animation->frameSource();
        viewer.owner.reset();
        QTest::qWait(150);
        QSize ignored;
        QVERIFY(viewer.frames->requestImage(old.path().mid(1), &ignored, {}).isNull());
    }
    void pausedPlaybackSurvivesAnExpiredWorker()
    {
        Viewer viewer;
        viewer.animation->setSource(gif());
        viewer.animation->setPlaying(false);
        viewer.animation->setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.animation->frameSource().isEmpty(), 5000);
        const int first = viewer.animation->currentFrame();
        QTest::qWait(1300); // AsyncImageProvider workers expire after one second.
        QCOMPARE(viewer.animation->currentFrame(), first);
        viewer.animation->setPlaying(true);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.animation->currentFrame() != first, 5000);
        QVERIFY(viewer.animation->error().isEmpty());
    }
    void onlyNewestSourceIsDelivered()
    {
        Viewer viewer;
        viewer.policy.processingMutex().lock();
        viewer.animation->setSource(jxl("large-48mp.jxl"));
        viewer.animation->setActive(true);
        viewer.animation->setSource(gif());
        viewer.policy.processingMutex().unlock();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.animation->frameSource().isEmpty(), 5000);
        QCOMPARE(viewer.frame().size(), QSize(320, 240));
        QVERIFY(viewer.animation->error().isEmpty());
    }
    void jxlPlaybackLoopsAndSeeks()
    {
        Viewer viewer;
        viewer.animation->setSource(jxl("animated.jxl"));
        viewer.animation->setPlaying(false);
        viewer.animation->setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 0, 5000);
        QCOMPARE(viewer.frame().pixelColor(64, 48).red(), 31);
        viewer.animation->seekFrame(2);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 2, 5000);
        QCOMPARE(viewer.frame().pixelColor(64, 48).red(), 89);
        viewer.animation->seekFrame(0);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 0, 5000);
        QSignalSpy frames(viewer.animation, &Licasa::ImageAnimation::frameChanged);
        viewer.animation->setPlaying(true);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.animation->playing(), 5000);
        QCOMPARE(viewer.animation->currentFrame(), 2);
        QCOMPARE(frames.count(), 5); // frames 1,2 then 0,1,2 for the second play
        QVERIFY(viewer.animation->error().isEmpty());
    }
    void changedLimitAndMalformedAnimationFailCleanly()
    {
        Viewer viewer;
        viewer.animation->setSource(jxl("animation-100-4k.jxl"));
        viewer.animation->setPlaying(false);
        viewer.animation->setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.animation->currentFrame(), 0, 10000);
        viewer.preferences.setMaximumImageMegapixels(25);
        viewer.animation->setPlaying(true);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.animation->currentFrame() > 0, 10000);
        viewer.animation->setSource(jxl("broken/truncated.jxl"));
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.animation->error().isEmpty(), 5000);
        QVERIFY(viewer.animation->frameSource().isEmpty());
    }

  private:
    QTemporaryDir settings_;
};

int main(int argc, char** argv)
{
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    QGuiApplication application(argc, argv);
    Licasa::initializeImagePlugins();
    ImageAnimationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_image_animation.moc"
