#include "media/motion/motion_photo_session.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMutex>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QtTest>

#include <algorithm>

using namespace Licasa;

namespace {

PhotoAssetInfo
embeddedMotionAsset(const QString& path = QStringLiteral("/tmp/licasa-session-test.jpg"))
{
    PhotoAssetInfo asset;
    asset.kind = PhotoAssetKind::MotionPhoto;
    asset.sourceUrl = QUrl::fromLocalFile(path);
    asset.motion = MotionComponent{
        CheckedByteRange{0, 9},
        QStringLiteral("video/mp4"),
        qint64(123456),
    };
    return asset;
}

#ifdef LICASA_MOTION_BACKEND_ENABLED
QStringList mappedMediaLibraries()
{
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly)) {
        return {};
    }

    QSet<QString> paths;
    for (const QByteArray& line : maps.readAll().split('\n')) {
        if (!line.contains('/')) {
            continue;
        }
        const QByteArray path = line.mid(line.indexOf('/'));
        if (path.contains("liblicasa_motion_photo_backend") || path.contains("libQt6Multimedia") ||
            path.contains("libffmpegmediaplugin") || path.contains("libavcodec.so") ||
            path.contains("libavformat.so") || path.contains("libavutil.so") ||
            path.contains("libswresample.so") || path.contains("libswscale.so")) {
            paths.insert(QString::fromLocal8Bit(path));
        }
    }
    QStringList result(paths.cbegin(), paths.cend());
    result.sort();
    return result;
}

bool containsMapping(const QStringList& mappings, const QString& needle)
{
    return std::any_of(mappings.cbegin(), mappings.cend(),
                       [&](const QString& path) { return path.contains(needle); });
}

#endif

class FakeMotionPolicy final : public MotionPhotoResourcePolicy {
  public:
    QMutex& processingMutex() noexcept override { return mutex_; }
    int prepareForProcessing() override { return 100; }

  private:
    QMutex mutex_;
};

} // namespace

class MotionPhotoSessionTest final : public QObject {
    Q_OBJECT

  private slots:
    void defaultAndStillStateAreInactive()
    {
        MotionPhotoSession session;
        QVERIFY(!session.available());
        QVERIFY(!session.active());
        QVERIFY(!session.playing());
        QCOMPARE(session.positionMs(), qint64(0));
        QCOMPARE(session.durationMs(), qint64(0));
        QVERIFY(!session.seekable());
        QVERIFY(session.errorString().isEmpty());

        PhotoAssetInfo still;
        still.kind = PhotoAssetKind::Still;
        still.sourceUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/still.jpg"));
        session.setAsset(still);
        QVERIFY(!session.available());
        session.stop();
        QVERIFY(!session.active());
    }

    void validatedMotionAuthorityIsCachedWithoutPlaybackActivation()
    {
        MotionPhotoSession session;
        QSignalSpy available(&session, &MotionPhotoSession::availableChanged);
        session.setAsset(embeddedMotionAsset());
        QCOMPARE(available.count(), 1);
        QVERIFY(session.available());
        QVERIFY(!session.active());
        QVERIFY(!session.playing());
        QVERIFY(!session.seekable());
        QCOMPARE(session.positionMs(), qint64(0));
        QCOMPARE(session.durationMs(), qint64(0));

        // Merely caching validated authority, seeking while idle, pausing or
        // stopping must never demand-load the multimedia backend.
        session.seek(999);
        session.pause();
        session.stop();
        QVERIFY(!session.active());
        QVERIFY(!session.playing());
        QCOMPARE(session.positionMs(), qint64(0));
    }

    void replacementAndClearInvalidatePreviousAuthority()
    {
        MotionPhotoSession session;
        session.setAsset(embeddedMotionAsset());
        QVERIFY(session.available());

        PhotoAssetInfo malformed = embeddedMotionAsset();
        malformed.motion->externalVideoUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/pair.mov"));
        session.setAsset(malformed);
        QVERIFY(!session.available());
        QVERIFY(!session.active());

        session.setAsset(embeddedMotionAsset());
        QVERIFY(session.available());
        session.clearAsset();
        QVERIFY(!session.available());
        QVERIFY(!session.active());
        QVERIFY(!session.playing());
    }

    void qmlMetaObjectExposesStateAndIntentButNoMediaAuthority()
    {
        MotionPhotoSession session;
        const QMetaObject* meta = session.metaObject();

        for (const char* property : {"available", "active", "playing", "positionMs", "durationMs",
                                     "seekable", "errorString"}) {
            QVERIFY2(meta->indexOfProperty(property) >= 0, property);
        }

        for (const char* property :
             {"videoOffset", "videoLength", "byteRange", "embeddedRange", "PhotoAssetInfo",
              "sourceUrl", "externalVideoUrl", "backend", "backendLibrary", "rgba8888", "pixels",
              "videoFrame", "videoSink", "presentationFrame"}) {
            QCOMPARE(meta->indexOfProperty(property), -1);
        }

        QVERIFY(meta->indexOfMethod("play()") >= 0);
        QVERIFY(meta->indexOfMethod("pause()") >= 0);
        QVERIFY(meta->indexOfMethod("togglePlayback()") >= 0);
        QVERIFY(meta->indexOfMethod("seek(qint64)") >= 0 ||
                meta->indexOfMethod("seek(qlonglong)") >= 0);
        QVERIFY(meta->indexOfMethod("stop()") >= 0);

        QCOMPARE(meta->indexOfMethod("setAsset(Licasa::PhotoAssetInfo)"), -1);
        QCOMPARE(meta->indexOfMethod("clearAsset()"), -1);
    }

    void backendLoadsOnlyOnFirstPlaybackIntent()
    {
#ifndef LICASA_MOTION_BACKEND_ENABLED
        QSKIP("Motion Photo backend is disabled in this build");
#else
        const QStringList before = mappedMediaLibraries();
        QVERIFY2(!containsMapping(before, QStringLiteral("liblicasa_motion_photo_backend")),
                 qPrintable(before.join('\n')));
        QVERIFY2(!containsMapping(before, QStringLiteral("libQt6Multimedia")),
                 qPrintable(before.join('\n')));
        QVERIFY2(!containsMapping(before, QStringLiteral("libavcodec.so")),
                 qPrintable(before.join('\n')));

        FakeMotionPolicy policy;
        MotionPhotoSession session(&policy);
        QTemporaryFile source;
        QVERIFY(source.open());
        QCOMPARE(source.write("bad media", 9), qint64(9));
        QVERIFY(source.flush());

        session.setAsset(embeddedMotionAsset(source.fileName()));
        QVERIFY(session.available());

        const QStringList idle = mappedMediaLibraries();
        qInfo().noquote() << "STAGE2A IDLE MAPPINGS:"
                          << (idle.isEmpty() ? QStringLiteral("<none>") : idle.join('\n'));
        QVERIFY2(!containsMapping(idle, QStringLiteral("liblicasa_motion_photo_backend")),
                 qPrintable(idle.join('\n')));
        QVERIFY2(!containsMapping(idle, QStringLiteral("libQt6Multimedia")),
                 qPrintable(idle.join('\n')));

        session.play();

        QTRY_VERIFY_WITH_TIMEOUT(containsMapping(mappedMediaLibraries(),
                                                 QStringLiteral("liblicasa_motion_photo_backend")),
                                 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            containsMapping(mappedMediaLibraries(), QStringLiteral("libQt6Multimedia")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.errorString().isEmpty(), 12000);
        const QStringList afterPlay = mappedMediaLibraries();
        qInfo().noquote() << "STAGE2A AFTER PLAY MAPPINGS:" << afterPlay.join('\n');

        // The malformed fixture proves the backend -> session state callback is
        // live without needing to copy any media authority back into QML.
        QVERIFY(!session.active());
        QVERIFY(!session.playing());

        session.clearAsset();

        // QLibrary::unload()/dlclose() is not a portable guarantee that the
        // object disappears from /proc/self/maps: Qt, the platform loader, or
        // a dependency may retain a mapping after the backend instance is
        // destroyed. The security/lifecycle contract is authority and state,
        // not post-activation address-space reclamation.
        QVERIFY(!session.available());
        QVERIFY(!session.active());
        QVERIFY(!session.playing());
        QCOMPARE(session.positionMs(), qint64(0));
        QCOMPARE(session.durationMs(), qint64(0));
        QVERIFY(!session.seekable());
        QVERIFY(session.errorString().isEmpty());

        // With no validated asset cached, stale playback intent must be inert.
        session.play();
        QCoreApplication::processEvents();
        QVERIFY(!session.available());
        QVERIFY(!session.active());
        QVERIFY(!session.playing());

        qInfo().noquote() << "STAGE2A AFTER CLEAR MAPPINGS (informational):"
                          << mappedMediaLibraries().join('\n');
#endif
    }
};

QTEST_MAIN(MotionPhotoSessionTest)

#include "tst_motion_photo_session.moc"
