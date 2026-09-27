#include "app/application_settings.h"
#include "imaging/async_image_provider.h"
#include "imaging/image_resource_policy.h"

#include <QSemaphore>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

class AsyncImageProviderTest final : public QObject {
    Q_OBJECT

  private slots:
    void destructionDrainsQueuedWorkOnTheWorker()
    {
        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        Licasa::ViewerPreferences preferences;
        Licasa::ImageResourcePolicy policy(preferences);
        auto provider = std::make_unique<Licasa::AsyncImageProvider>(policy);
        QSemaphore entered, release;
        std::atomic_bool cleanupRan{false};
        std::atomic_bool cleanupRanOffMainThread{false};
        QThread* mainThread = QThread::currentThread();
        provider->enqueueWork([&] {
            entered.release();
            release.acquire();
        });
        entered.acquire();
        provider->enqueueWork([&] {
            cleanupRanOffMainThread = QThread::currentThread() != mainThread;
            cleanupRan = true;
        });
        // Keep cleanup queued when destruction begins, then let the active job
        // finish. Destruction must execute cleanup, not drop its closure.
        std::thread unblock([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release.release();
        });
        provider.reset();
        unblock.join();
        QVERIFY(cleanupRan.load());
        QVERIFY(cleanupRanOffMainThread.load());
    }
};

QTEST_GUILESS_MAIN(AsyncImageProviderTest)
#include "tst_async_image_provider.moc"
