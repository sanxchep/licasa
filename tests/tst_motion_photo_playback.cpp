#include "app/application_settings.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_resource_policy.h"
#include "media/motion/motion_photo_playback.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#ifdef __GLIBC__
#include <malloc.h>
#endif

using namespace Licasa;

#if defined(LICASA_SAMSUNG_REAL_FIXTURE_1) && defined(LICASA_SAMSUNG_REAL_FIXTURE_2) &&            \
    defined(LICASA_SAMSUNG_REAL_FIXTURE_3)
struct SamsungRealPlaybackFixture {
    const char* path = nullptr;
    qint64 fileBytes = 0;
    quint64 videoOffset = 0;
    quint64 videoLength = 0;
    qint64 minimumDurationMs = 0;
    qint64 maximumDurationMs = 0;
};

static std::array<SamsungRealPlaybackFixture, 3> samsungRealPlaybackFixtures()
{
    return {{{LICASA_SAMSUNG_REAL_FIXTURE_1, 8013982, 3366251, 4647675, 2700, 3200},
             {LICASA_SAMSUNG_REAL_FIXTURE_2, 7221645, 2685814, 4535775, 2700, 3200},
             {LICASA_SAMSUNG_REAL_FIXTURE_3, 7311312, 2584924, 4726320, 2700, 3300}}};
}
#endif

#if defined(LICASA_XIAOMI_REAL_FIXTURE_MODERN) && defined(LICASA_XIAOMI_REAL_FIXTURE_LEGACY) &&    \
    defined(LICASA_XIAOMI_REAL_FIXTURE_GAINMAP)
struct XiaomiRealPlaybackFixture {
    const char* path = nullptr;
    qint64 fileBytes = 0;
    quint64 videoOffset = 0;
    quint64 videoLength = 0;
    qint64 presentationUs = 0;
    QSize frameSize;
    qint64 minimumDurationMs = 0;
    qint64 maximumDurationMs = 0;
};

static std::array<XiaomiRealPlaybackFixture, 3> xiaomiRealPlaybackFixtures()
{
    return {{{LICASA_XIAOMI_REAL_FIXTURE_MODERN, 3129678, 698144, 2431534, 1232129,
              QSize(1080, 1440), 2500, 3000},
             {LICASA_XIAOMI_REAL_FIXTURE_LEGACY, 4332013, 2961546, 1370467, 1232308,
              QSize(1080, 1440), 1500, 1900},
             {LICASA_XIAOMI_REAL_FIXTURE_GAINMAP, 8291290, 5123094, 3168196, 1530610,
              QSize(1296, 1728), 2800, 3300}}};
}
#endif

static PhotoAssetInfo fixture()
{
#ifdef LICASA_HEIC_MOTION_FIXTURE
    return probePhotoAsset(QUrl::fromLocalFile(QStringLiteral(LICASA_HEIC_MOTION_FIXTURE)));
#else
    return {};
#endif
}

static constexpr quint32 isoFourCc(char a, char b, char c, char d) noexcept
{
    return (quint32(quint8(a)) << 24) | (quint32(quint8(b)) << 16) | (quint32(quint8(c)) << 8) |
           quint32(quint8(d));
}

static void appendIsoBe32(QByteArray* bytes, quint32 value)
{
    char raw[4];
    qToBigEndian<quint32>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 4);
}

static QByteArray isoBox(quint32 type, const QByteArray& payload = {})
{
    Q_ASSERT(quint64(payload.size()) + 8 <= std::numeric_limits<quint32>::max());
    QByteArray result;
    result.reserve(payload.size() + 8);
    appendIsoBe32(&result, quint32(payload.size() + 8));
    appendIsoBe32(&result, type);
    result += payload;
    return result;
}

static QByteArray sampleDescriptionBox(quint32 declaredEntries, quint32 actualEntries,
                                       bool visualEntryFirst = false)
{
    QByteArray payload(4, '\0'); // version and flags
    payload.reserve(8 + int(actualEntries) * 8 + (visualEntryFirst ? 28 : 0));
    appendIsoBe32(&payload, declaredEntries);
    if (visualEntryFirst && actualEntries > 0) {
        QByteArray visualPayload(28, '\0');
        QByteArray visual = isoBox(isoFourCc('a', 'v', 'c', '1'), visualPayload);
        qToBigEndian<quint16>(1920, reinterpret_cast<uchar*>(visual.data() + 32));
        qToBigEndian<quint16>(1080, reinterpret_cast<uchar*>(visual.data() + 34));
        payload += visual;
        --actualEntries;
    }
    const QByteArray opaqueEntry = isoBox(isoFourCc('t', 'e', 'x', 't'));
    for (quint32 index = 0; index < actualEntries; ++index) {
        payload += opaqueEntry;
    }
    return isoBox(isoFourCc('s', 't', 's', 'd'), payload);
}

static QByteArray adversarialIsoMetadata(const QString& shape)
{
    QByteArray result = isoBox(isoFourCc('f', 't', 'y', 'p'), QByteArray("isom\0\0\0\0", 8));
    const QByteArray freeBox = isoBox(isoFourCc('f', 'r', 'e', 'e'));
    if (shape == QStringLiteral("many-boxes")) {
        for (int index = 0; index < 8192; ++index) {
            result += freeBox;
        }
    } else if (shape == QStringLiteral("aggregate-stsd")) {
        for (int index = 0; index < 3; ++index) {
            result += sampleDescriptionBox(3000, 3000);
        }
    } else if (shape == QStringLiteral("dimension-before-many-boxes")) {
        result += sampleDescriptionBox(1, 1, true);
        for (int index = 0; index < 8192; ++index) {
            result += freeBox;
        }
    } else if (shape == QStringLiteral("oversized-stsd")) {
        result += sampleDescriptionBox(4097, 0);
    } else if (shape == QStringLiteral("excessive-depth")) {
        QByteArray nested;
        for (int depth = 0; depth < 9; ++depth) {
            nested = isoBox(isoFourCc('m', 'o', 'o', 'v'), nested);
        }
        result += nested;
    }
    return result;
}

#if defined(LICASA_APPLE_EXTERNAL_MOV_FIXTURE) && defined(LICASA_APPLE_EXTERNAL_MOV_IDENTIFIER)
static void appendBe16(QByteArray* bytes, quint16 value)
{
    char raw[2];
    qToBigEndian<quint16>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 2);
}

static void appendBe32(QByteArray* bytes, quint32 value)
{
    char raw[4];
    qToBigEndian<quint32>(value, reinterpret_cast<uchar*>(raw));
    bytes->append(raw, 4);
}

static QByteArray applePairJpeg(const QByteArray& identifier)
{
    QByteArray maker("Apple iOS\0", 10);
    maker += QByteArray::fromHex("00014d4d");
    appendBe16(&maker, 1);
    appendBe16(&maker, 0x0011);
    appendBe16(&maker, 2);
    appendBe32(&maker, quint32(identifier.size() + 1));
    appendBe32(&maker, 32);
    appendBe32(&maker, 0);
    maker += identifier;
    maker += '\0';

    QByteArray tiff("MM\0*", 4);
    appendBe32(&tiff, 8);
    appendBe16(&tiff, 1);
    appendBe16(&tiff, 0x8769);
    appendBe16(&tiff, 4);
    appendBe32(&tiff, 1);
    appendBe32(&tiff, 26);
    appendBe32(&tiff, 0);
    appendBe16(&tiff, 1);
    appendBe16(&tiff, 0x927c);
    appendBe16(&tiff, 7);
    appendBe32(&tiff, quint32(maker.size()));
    appendBe32(&tiff, 44);
    appendBe32(&tiff, 0);
    tiff += maker;

    QByteArray payload("Exif\0\0", 6);
    payload += tiff;
    QByteArray jpeg = QByteArray::fromHex("ffd8ffe1");
    appendBe16(&jpeg, quint16(payload.size() + 2));
    jpeg += payload;
    jpeg += QByteArray::fromHex("ffd9");
    return jpeg;
}

static PhotoAssetInfo externalFixtureAsset(QTemporaryDir& directory,
                                           QString* moviePathOut = nullptr)
{
    const QString fixturePath = QStringLiteral(LICASA_APPLE_EXTERNAL_MOV_FIXTURE);
    const QByteArray identifier(LICASA_APPLE_EXTERNAL_MOV_IDENTIFIER);
    if (!directory.isValid() || !QFileInfo::exists(fixturePath)) {
        return {};
    }
    const QString stillPath = directory.filePath(QStringLiteral("IMG_9000.JPG"));
    QFile still(stillPath);
    const QByteArray jpeg = applePairJpeg(identifier);
    if (!still.open(QIODevice::WriteOnly) || still.write(jpeg) != jpeg.size()) {
        return {};
    }
    still.close();
    const QString moviePath = directory.filePath(QStringLiteral("IMG_9000.MOV"));
    if (!QFile::copy(fixturePath, moviePath)) {
        return {};
    }
    if (moviePathOut) {
        *moviePathOut = moviePath;
    }
    return probePhotoAsset(QUrl::fromLocalFile(stillPath));
}
#endif

static QStringList mappedMediaLibraries()
{
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly)) {
        return {};
    }
    QSet<QString> paths;
    for (const auto& line : maps.readAll().split('\n')) {
        if (!line.contains('/')) {
            continue;
        }
        const QByteArray path = line.mid(line.indexOf('/'));
        if (path.contains("libQt6Multimedia") || path.contains("libffmpegmediaplugin") ||
            path.contains("libavcodec.so") || path.contains("libavformat.so") ||
            path.contains("libavutil.so") || path.contains("libswresample.so") ||
            path.contains("libswscale.so")) {
            paths.insert(QString::fromLocal8Bit(path));
        }
    }
    QStringList result;
    result.reserve(paths.size());
    for (const auto& path : paths) {
        result.append(path);
    }
    result.sort();
    return result;
}

static bool qualifiedPrivateFfmpegIsMapped()
{
    const QString expected =
        QFileInfo(QStringLiteral(LICASA_EXPECTED_FFMPEG_PLUGIN)).canonicalFilePath();
    if (expected.isEmpty()) {
        return false;
    }
    for (const auto& path : mappedMediaLibraries()) {
        if (QFileInfo(path).canonicalFilePath() == expected) {
            return true;
        }
    }
    return false;
}

static bool ffmpegBackendIsMapped()
{
    const QStringList mappings = mappedMediaLibraries();
    return std::any_of(mappings.cbegin(), mappings.cend(), [](const QString& path) {
        return path.contains(QStringLiteral("libffmpegmediaplugin")) ||
               path.contains(QStringLiteral("libavcodec.so")) ||
               path.contains(QStringLiteral("libavformat.so")) ||
               path.contains(QStringLiteral("libavutil.so"));
    });
}

static QByteArray overBudgetVideoFromFixture(const PhotoAssetInfo& asset)
{
    if (!asset.motion || !asset.sourceUrl.isLocalFile()) {
        return {};
    }
    QFile source(asset.sourceUrl.toLocalFile());
    if (!source.open(QIODevice::ReadOnly) ||
        asset.motion->embedded.offset > quint64(std::numeric_limits<qint64>::max()) ||
        asset.motion->embedded.length > 16ULL * 1024ULL * 1024ULL ||
        !source.seek(qint64(asset.motion->embedded.offset))) {
        return {};
    }
    QByteArray video = source.read(qint64(asset.motion->embedded.length));
    if (quint64(video.size()) != asset.motion->embedded.length) {
        return {};
    }
    for (const QByteArray& type : {QByteArrayLiteral("hvc1"), QByteArrayLiteral("hev1"),
                                   QByteArrayLiteral("avc1"), QByteArrayLiteral("mp4v")}) {
        qsizetype cursor = 0;
        while ((cursor = video.indexOf(type, cursor)) >= 4) {
            const qsizetype boxStart = cursor - 4;
            const quint32 boxSize = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar*>(video.constData() + boxStart));
            if (boxSize >= 86 && quint64(boxStart) + boxSize <= quint64(video.size()) &&
                cursor + 32 <= video.size()) {
                qToBigEndian<quint16>(8192, reinterpret_cast<uchar*>(video.data() + cursor + 28));
                qToBigEndian<quint16>(4096, reinterpret_cast<uchar*>(video.data() + cursor + 30));
                return video;
            }
            cursor += type.size();
        }
    }
    return {};
}

static QJsonObject processMemory()
{
    QJsonObject result;
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly)) {
        for (const auto& line : status.readAll().split('\n')) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 2) {
                continue;
            }
            if (fields[0] == "VmRSS:") {
                result["rss_kib"] = fields[1].toDouble();
            }
            if (fields[0] == "VmHWM:") {
                result["peak_rss_kib"] = fields[1].toDouble();
            }
        }
    }
#ifdef __GLIBC__
    const auto allocation = mallinfo2();
    result["live_allocation_kib"] = double(allocation.uordblks + allocation.hblkhd) / 1024;
#endif
    result["mapped_media_libraries"] = QJsonArray::fromStringList(mappedMediaLibraries());
    result["qualified_private_ffmpeg_mapped"] = qualifiedPrivateFfmpegIsMapped();
    return result;
}

class MotionPhotoPlaybackTest final : public QObject {
    Q_OBJECT
  private slots:
    void idleAndCancelledRequestStayLazy()
    {
        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QVERIFY(!playback.active());
        playback.play({});
        playback.stop();
        QCoreApplication::processEvents();
        QVERIFY(!playback.active());
        QVERIFY(playback.error().isEmpty());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
        const QStringList mappings = mappedMediaLibraries();
        QVERIFY(std::none_of(mappings.cbegin(), mappings.cend(), [](const QString& path) {
            return path.contains(QStringLiteral("libffmpegmediaplugin")) ||
                   path.contains(QStringLiteral("libavcodec.so")) ||
                   path.contains(QStringLiteral("libavformat.so")) ||
                   path.contains(QStringLiteral("libavutil.so"));
        }));
    }

    void invalidAssetsAndRanges()
    {
        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy changes(&playback, &MotionPhotoPlayback::changed);
        PhotoAssetInfo asset;
        playback.play(asset);
        QTRY_COMPARE(changes.count(), 1);
        QVERIFY(!playback.error().isEmpty());
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write("bad media", 9), qint64(9));
        QVERIFY(file.flush());
        asset.kind = PhotoAssetKind::MotionPhoto;
        asset.sourceUrl = QUrl::fromLocalFile(file.fileName());
        asset.motion = MotionComponent{
            {0, std::numeric_limits<quint64>::max()}, QStringLiteral("video/mp4"), {}};
        playback.play(asset);
        QTRY_COMPARE(changes.count(), 2);
        QVERIFY(playback.error().contains(QStringLiteral("limits")));
        QVERIFY(!playback.active());
        asset.motion->embedded = {8, 2};
        playback.play(asset);
        QTRY_COMPARE(changes.count(), 3);
        QVERIFY(playback.error().contains(QStringLiteral("outside")));
        asset.sourceUrl = QUrl(QStringLiteral("https://example.invalid/photo.jpg"));
        playback.play(asset);
        QTRY_COMPARE(changes.count(), 4);
        QVERIFY(!playback.active());
    }

    void busyGateAndMalformedMedia()
    {
        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write("not an mp4", 10), qint64(10));
        QVERIFY(file.flush());
        PhotoAssetInfo asset;
        asset.kind = PhotoAssetKind::MotionPhoto;
        asset.sourceUrl = QUrl::fromLocalFile(file.fileName());
        asset.motion = MotionComponent{{0, 10}, QStringLiteral("video/mp4"), {}};
        policy.processingMutex().lock();
        playback.play(asset);
        QTRY_VERIFY(!playback.error().isEmpty());
        QVERIFY(playback.error().contains(QStringLiteral("busy")));
        policy.processingMutex().unlock();
        QSignalSpy changes(&playback, &MotionPhotoPlayback::changed);
        playback.play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(changes.count() > 0 && !playback.active(), 12000);
        QVERIFY(!playback.error().isEmpty());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
    }

    void playbackSeekStopAndReopen()
    {
#ifndef LICASA_HEIC_MOTION_FIXTURE
        QSKIP("Configure the external S23 fixture for runtime qualification");
#else
        const auto asset = fixture();
        QVERIFY(asset.motion.has_value());
        QCOMPARE(asset.motion->embedded.offset, quint64(1783479));
        QCOMPARE(asset.motion->embedded.length, quint64(3555843));
        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        for (int cycle = 0; cycle < 3; ++cycle) {
            frames.clear();
            playback.play(asset);
            QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0 || !playback.error().isEmpty(), 12000);
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY2(qualifiedPrivateFfmpegIsMapped(),
                     "Playback did not load the qualified private Qt FFmpeg plugin");
            QCOMPARE(frames.first()[0].toSize(), QSize(1440, 1080));
            QCOMPARE(playback.duration(), qint64(2799));
            QVERIFY(playback.seekable());
            QVERIFY(!policy.processingMutex().tryLock());
            preferences.setMaximumImageMegapixels(25);
            QCOMPARE(policy.maximumImageMegapixels(), 25);
            playback.pause();
            QTRY_VERIFY(!playback.playing());
            // A burst coalesces to the last position; old commands must not
            // affect the next source. Timestamp proves actual post-seek output.
            playback.seek(200);
            playback.seek(700);
            playback.seek(1399);
            frames.clear();
            playback.resume();
            QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() && frames.last()[1].toLongLong() >= 1399000,
                                     12000);
            playback.stop();
            QTRY_VERIFY(!playback.active());
            QVERIFY(policy.processingMutex().tryLock());
            policy.processingMutex().unlock();
            QVERIFY(playback.error().isEmpty());
            preferences.setMaximumImageMegapixels(100);
        }
#endif
    }

    void pauseIntentTakesEffectBeforeReturning()
    {
#ifndef LICASA_HEIC_MOTION_FIXTURE
        QSKIP("Configure the external S23 fixture for pause-intent qualification");
#else
        const auto asset = fixture();
        QVERIFY(asset.motion.has_value());

        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        playback.play(asset);

        QTRY_VERIFY_WITH_TIMEOUT(playback.active() && playback.playing() && playback.duration() > 0,
                                 12000);
        QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));

        // pause() is invoked by the visible transport on the owning GUI
        // thread. It must establish PausedState before returning; otherwise a
        // short clip can race EndOfMedia while the queued pause is still
        // waiting in the event loop.
        playback.pause();
        QVERIFY(playback.active());
        QVERIFY(!playback.playing());

        // A paused short clip must survive an EndOfMedia notification that was
        // already queued by Qt Multimedia. Seeking exactly to EOF stresses the
        // same ordering used by the production lifecycle diagnostic. The
        // session stays admitted so the user can scrub or replay instead of
        // losing the transport surface underneath the click.
        const qint64 mediaDuration = playback.duration();
        QVERIFY(mediaDuration > 0);
        frames.clear();
        playback.seek(mediaDuration);
        QTRY_VERIFY_WITH_TIMEOUT(playback.active() && playback.duration() == mediaDuration &&
                                     playback.position() >=
                                         std::max<qint64>(0, mediaDuration - 100),
                                 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 12000);
        QTest::qWait(100);
        QVERIFY(playback.active());
        QVERIFY(!playback.playing());

        // Play from EOF is defined as replay from zero. This also ensures the
        // preserved paused-EOS state is not a dead end.
        frames.clear();
        playback.resume();
        QTRY_VERIFY_WITH_TIMEOUT(playback.playing(), 2000);
        QVERIFY(playback.position() < mediaDuration);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(frames.cbegin(), frames.cend(),
                                             [](const QList<QVariant>& frame) {
                                                 return frame.size() > 1 &&
                                                        frame[1].toLongLong() < 1000000;
                                             }),
                                 12000);

        // Natural EOF must preserve the admitted session and last-frame state.
        // Otherwise a short Motion Photo can finish between the UI observing
        // its first visible frame and the user's Pause click, making the tray
        // disappear even though playback itself succeeded. Let the replay run
        // to completion, then give the queued EndOfMedia callback time to run.
        QTRY_VERIFY_WITH_TIMEOUT(!playback.playing(), mediaDuration + 3000);
        QTest::qWait(100);
        QVERIFY(playback.active());
        QCOMPARE(playback.duration(), mediaDuration);
        QVERIFY(playback.position() >= std::max<qint64>(0, mediaDuration - 100));
        QVERIFY(playback.seekable());

        // Natural EOF must remain scrubbable as well as visible. This is the
        // capability that keeps TemporalControls.canScrub true after a short
        // clip finishes, instead of leaving a disabled/non-clickable timeline.
        const qint64 scrubTarget = std::max<qint64>(1, mediaDuration / 2);
        frames.clear();
        playback.seek(scrubTarget);
        QTRY_VERIFY_WITH_TIMEOUT(playback.position() >= std::max<qint64>(0, scrubTarget - 150) &&
                                     playback.position() <=
                                         std::min(mediaDuration, scrubTarget + 250),
                                 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 12000);
        QVERIFY2(frames.last()[1].toLongLong() >= std::max<qint64>(0, scrubTarget * 1000 - 100000),
                 "Paused scrub did not decode a replacement presentation frame");
        QVERIFY(playback.active());
        QVERIFY(!playback.playing());
        QVERIFY(playback.seekable());

        // Return to EOF and prove the preserved state remains replayable.
        playback.seek(mediaDuration);
        QTRY_VERIFY_WITH_TIMEOUT(playback.position() >= std::max<qint64>(0, mediaDuration - 100),
                                 2000);
        playback.resume();
        QTRY_VERIFY_WITH_TIMEOUT(playback.playing(), 2000);
        QVERIFY(playback.position() < mediaDuration);

        playback.stop();
        QTRY_VERIFY(!playback.active());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
#endif
    }

    void overBudgetMetadataDoesNotDecode()
    {
#ifndef LICASA_HEIC_MOTION_FIXTURE
        QSKIP("Configure the external S23 fixture for runtime qualification");
#else
        const bool requireLazyPreflight =
            qEnvironmentVariableIsSet("LICASA_EXPECT_PREPLAYBACK_LAZY");
        if (requireLazyPreflight) {
            QVERIFY2(!ffmpegBackendIsMapped(),
                     "FFmpeg was already mapped before the oversized preflight test started");
        }
        const QByteArray video = overBudgetVideoFromFixture(fixture());
        QVERIFY2(!video.isEmpty(), "Could not create the metadata-only over-budget MP4 variant");
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write(video), qint64(video.size()));
        QVERIFY(file.flush());
        ViewerPreferences preferences;
        preferences.setMaximumImageMegapixels(25);
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        PhotoAssetInfo asset;
        asset.kind = PhotoAssetKind::MotionPhoto;
        asset.sourceUrl = QUrl::fromLocalFile(file.fileName());
        asset.motion = MotionComponent{{0, quint64(video.size())}, QStringLiteral("video/mp4"), {}};
        playback.play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(!playback.error().isEmpty(), 12000);
        QVERIFY(playback.error().contains(QStringLiteral("dimensions")));
        QVERIFY(!playback.active());
        QVERIFY(frames.isEmpty());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
        if (requireLazyPreflight) {
            QVERIFY2(!ffmpegBackendIsMapped(),
                     "Oversized container metadata activated the FFmpeg backend before rejection");
        }
        preferences.setMaximumImageMegapixels(100);
#endif
    }

    void metadataComplexityLimitIsFailClosed_data()
    {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<bool>("external");
        const QStringList shapes{
            QStringLiteral("many-boxes"),
            QStringLiteral("aggregate-stsd"),
            QStringLiteral("dimension-before-many-boxes"),
            QStringLiteral("oversized-stsd"),
            QStringLiteral("excessive-depth"),
        };
        for (const QString& shape : shapes) {
            const QByteArray embeddedName = (shape + QStringLiteral("-embedded")).toLatin1();
            QTest::newRow(embeddedName.constData()) << shape << false;
            const QByteArray externalName = (shape + QStringLiteral("-external")).toLatin1();
            QTest::newRow(externalName.constData()) << shape << true;
        }
    }

    void metadataComplexityLimitIsFailClosed()
    {
        QFETCH(QString, shape);
        QFETCH(bool, external);
        const bool requireLazyPreflight =
            qEnvironmentVariableIsSet("LICASA_EXPECT_PREPLAYBACK_LAZY");
        if (requireLazyPreflight) {
            QVERIFY2(!ffmpegBackendIsMapped(),
                     "FFmpeg was already mapped before the metadata complexity preflight");
        }

        const QByteArray media = adversarialIsoMetadata(shape);
        QVERIFY(media.size() > 16);
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write(media), qint64(media.size()));
        QVERIFY(file.flush());

        PhotoAssetInfo asset;
        asset.kind = PhotoAssetKind::MotionPhoto;
        asset.sourceUrl = QUrl::fromLocalFile(file.fileName());
        MotionComponent motion;
        if (external) {
            motion.mimeType = QStringLiteral("video/quicktime");
            motion.externalVideoUrl = asset.sourceUrl;
            motion.externalVideoIdentity = externalFileIdentity(file);
            QVERIFY(motion.externalVideoIdentity.has_value());
        } else {
            motion.mimeType = QStringLiteral("video/mp4");
            motion.embedded = {0, quint64(media.size())};
        }
        asset.motion = motion;

        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        playback.play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(!playback.error().isEmpty(), 2000);
        QVERIFY(playback.error().contains(QStringLiteral("metadata"), Qt::CaseInsensitive));
        QVERIFY(playback.error().contains(QStringLiteral("complexity"), Qt::CaseInsensitive));
        QVERIFY(playback.error().contains(QStringLiteral("limit"), Qt::CaseInsensitive));
        QVERIFY(!playback.active());
        QVERIFY(frames.isEmpty());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
        if (requireLazyPreflight) {
            QVERIFY2(!ffmpegBackendIsMapped(),
                     "Complex metadata activated FFmpeg before rejection");
        }
    }

    void externalAppleMovPlaybackIsPinnedAndSeekable()
    {
#if !defined(LICASA_APPLE_EXTERNAL_MOV_FIXTURE) || !defined(LICASA_APPLE_EXTERNAL_MOV_IDENTIFIER)
        QSKIP("Configure the generated Apple external MOV fixture for runtime qualification");
#else
        QTemporaryDir directory;
        QString moviePath;
        const auto asset = externalFixtureAsset(directory, &moviePath);
        QVERIFY2(!moviePath.isEmpty(), "Could not prepare the generated external MOV fixture");
        QVERIFY(asset.kind == PhotoAssetKind::MotionPhoto);
        QVERIFY(asset.motion.has_value());
        QVERIFY(!asset.motion->hasEmbeddedVideo());
        QVERIFY(asset.motion->hasExternalVideo());
        QVERIFY(asset.motion->externalVideoIdentity.has_value());
        QCOMPARE(asset.motion->externalVideoIdentity->size, quint64(QFileInfo(moviePath).size()));
        QCOMPARE(asset.motion->mimeType, QStringLiteral("video/quicktime"));

        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        for (int cycle = 0; cycle < 3; ++cycle) {
            frames.clear();
            playback.play(asset);
            QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !playback.error().isEmpty(), 12000);
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY2(qualifiedPrivateFfmpegIsMapped(),
                     "External MOV playback did not load the qualified private FFmpeg plugin");
            QCOMPARE(frames.first()[0].toSize(), QSize(1440, 1080));
            QVERIFY(playback.duration() >= 2700 && playback.duration() <= 3000);
            QVERIFY(playback.seekable());
            QVERIFY(!policy.processingMutex().tryLock());

            frames.clear();
            playback.seek(1399);
            QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() && frames.last()[1].toLongLong() >= 1399000,
                                     12000);
            playback.stop();
            QTRY_VERIFY(!playback.active());
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY(policy.processingMutex().tryLock());
            policy.processingMutex().unlock();
        }
#endif
    }

    void realLegacyPixelMvimgProbeAndPlayback()
    {
#ifndef LICASA_LEGACY_PIXEL_REAL_FIXTURE
        QSKIP("Configure the pinned original legacy Pixel MVIMG fixture for final qualification");
#else
        const QString path = QStringLiteral(LICASA_LEGACY_PIXEL_REAL_FIXTURE);
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("MVIMG_20191231_235723.jpg"));
        QCOMPARE(QFileInfo(path).size(), qint64(7117287));

        const PhotoAssetInfo asset = probePhotoAsset(QUrl::fromLocalFile(path));
        QVERIFY2(asset.metadataError.isEmpty(), qPrintable(asset.metadataError));
        QVERIFY(asset.kind == PhotoAssetKind::MotionPhoto);
        QVERIFY(asset.motion.has_value());
        QVERIFY(asset.motion->hasEmbeddedVideo());
        QVERIFY(!asset.motion->hasExternalVideo());
        QCOMPARE(asset.motion->mimeType, QStringLiteral("video/mp4"));
        QCOMPARE(asset.motion->embedded.offset, quint64(5318017));
        QCOMPARE(asset.motion->embedded.length, quint64(1799270));
        QVERIFY(asset.motion->presentationTimestampUs.has_value());
        QCOMPARE(*asset.motion->presentationTimestampUs, qint64(0));

        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        playback.play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !playback.error().isEmpty(), 15000);
        QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
        QVERIFY2(qualifiedPrivateFfmpegIsMapped(),
                 "Real legacy Pixel playback did not load the qualified private FFmpeg plugin");
        QCOMPARE(frames.first()[0].toSize(), QSize(1024, 768));
        QVERIFY(playback.duration() >= 1000 && playback.duration() <= 1300);
        QVERIFY(playback.seekable());
        QVERIFY(!policy.processingMutex().tryLock());

        const qint64 targetMs = std::max<qint64>(1, playback.duration() / 2);
        frames.clear();
        playback.seek(targetMs);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !playback.error().isEmpty(), 15000);
        QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
        QVERIFY2(frames.last()[1].toLongLong() >= targetMs * 1000,
                 "Real legacy Pixel seek did not produce a frame at/after the requested position");
        playback.stop();
        QTRY_VERIFY(!playback.active());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
#endif
    }

    void realRotatedPixelTrackMatrixIsPresentedUpright()
    {
#ifndef LICASA_ROTATED_PIXEL_REAL_FIXTURE
        QSKIP("Configure the rotated Pixel Motion Photo regression fixture");
#else
        const QString path = QStringLiteral(LICASA_ROTATED_PIXEL_REAL_FIXTURE);
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QCOMPARE(QFileInfo(path).size(), qint64(4776912));

        const PhotoAssetInfo asset = probePhotoAsset(QUrl::fromLocalFile(path));
        QVERIFY2(asset.metadataError.isEmpty(), qPrintable(asset.metadataError));
        QVERIFY(asset.kind == PhotoAssetKind::MotionPhoto);
        QVERIFY(asset.motion.has_value());
        QVERIFY(asset.motion->hasEmbeddedVideo());
        QCOMPARE(asset.motion->embedded.offset, quint64(1754545));
        QCOMPARE(asset.motion->embedded.length, quint64(3022367));
        QVERIFY(asset.motion->presentationTimestampUs.has_value());
        QCOMPARE(*asset.motion->presentationTimestampUs, qint64(1166704));

        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        std::optional<MotionPhotoFrameRaster> presented;
        playback.setFrameReadyCallback(
            [&presented](MotionPhotoFrameRaster frame) { presented = std::move(frame); });
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        playback.play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(presented.has_value() || !playback.error().isEmpty(), 15000);
        QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
        QVERIFY(!frames.isEmpty());
        QCOMPARE(presented->size, QSize(1080, 1920));
        QCOMPARE(frames.first()[0].toSize(), QSize(1080, 1920));
        QVERIFY(playback.seekable());

        playback.stop();
        QTRY_VERIFY(!playback.active());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
#endif
    }

    void realSamsungSeftFixturesProbeAndPlayback()
    {
#if !defined(LICASA_SAMSUNG_REAL_FIXTURE_1) || !defined(LICASA_SAMSUNG_REAL_FIXTURE_2) ||          \
    !defined(LICASA_SAMSUNG_REAL_FIXTURE_3)
        QSKIP("Configure all three pinned original Samsung SEFT fixtures for final qualification");
#else
        for (const auto& expected : samsungRealPlaybackFixtures()) {
            const QString path = QString::fromUtf8(expected.path);
            const QFileInfo fileInfo(path);
            QVERIFY2(fileInfo.exists(), qPrintable(path));
            QCOMPARE(fileInfo.size(), expected.fileBytes);

            const PhotoAssetInfo asset = probePhotoAsset(QUrl::fromLocalFile(path));
            QVERIFY2(asset.metadataError.isEmpty(), qPrintable(asset.metadataError));
            QVERIFY(asset.kind == PhotoAssetKind::MotionPhoto);
            QVERIFY(asset.motion.has_value());
            QVERIFY(asset.motion->hasEmbeddedVideo());
            QVERIFY(!asset.motion->hasExternalVideo());
            QCOMPARE(asset.motion->mimeType, QStringLiteral("video/mp4"));
            QCOMPARE(asset.motion->embedded.offset, expected.videoOffset);
            QCOMPARE(asset.motion->embedded.length, expected.videoLength);

            ViewerPreferences preferences;
            ImageResourcePolicy policy(preferences);
            MotionPhotoPlayback playback(policy);
            QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
            playback.play(asset);
            QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !playback.error().isEmpty(), 15000);
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY2(qualifiedPrivateFfmpegIsMapped(),
                     "Real Samsung SEFT playback did not load the qualified private FFmpeg plugin");
            QCOMPARE(frames.first()[0].toSize(), QSize(1440, 1080));
            QVERIFY(playback.duration() >= expected.minimumDurationMs &&
                    playback.duration() <= expected.maximumDurationMs);
            QVERIFY(playback.seekable());
            QVERIFY(!policy.processingMutex().tryLock());

            const qint64 targetMs = std::max<qint64>(1, playback.duration() / 2);
            const qint64 targetTimestampUs = targetMs * 1000;
            frames.clear();
            playback.seek(targetMs);
            const auto hasPostSeekFrame = [&frames, targetTimestampUs]() {
                return std::any_of(frames.cbegin(), frames.cend(),
                                   [targetTimestampUs](const QList<QVariant>& frame) {
                                       return frame.size() > 1 &&
                                              frame[1].toLongLong() >= targetTimestampUs;
                                   });
            };
            QTRY_VERIFY_WITH_TIMEOUT(hasPostSeekFrame() || !playback.error().isEmpty(), 15000);
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY2(hasPostSeekFrame(),
                     "Real Samsung seek did not produce a frame at/after the requested position");
            playback.stop();
            QTRY_VERIFY(!playback.active());
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY(policy.processingMutex().tryLock());
            policy.processingMutex().unlock();
        }
#endif
    }

    void realXiaomiMotionFixturesProbeAndPlayback()
    {
#if !defined(LICASA_XIAOMI_REAL_FIXTURE_MODERN) || !defined(LICASA_XIAOMI_REAL_FIXTURE_LEGACY) ||  \
    !defined(LICASA_XIAOMI_REAL_FIXTURE_GAINMAP)
        QSKIP("Configure all three pinned original Xiaomi Motion Photo fixtures for final "
              "qualification");
#else
        for (const auto& expected : xiaomiRealPlaybackFixtures()) {
            const QString path = QString::fromUtf8(expected.path);
            const QFileInfo fileInfo(path);
            QVERIFY2(fileInfo.exists(), qPrintable(path));
            QCOMPARE(fileInfo.size(), expected.fileBytes);

            const PhotoAssetInfo asset = probePhotoAsset(QUrl::fromLocalFile(path));
            QVERIFY2(asset.metadataError.isEmpty(), qPrintable(asset.metadataError));
            QVERIFY(asset.kind == PhotoAssetKind::MotionPhoto);
            QVERIFY(asset.motion.has_value());
            QVERIFY(asset.motion->hasEmbeddedVideo());
            QVERIFY(!asset.motion->hasExternalVideo());
            QCOMPARE(asset.motion->mimeType, QStringLiteral("video/mp4"));
            QCOMPARE(asset.motion->embedded.offset, expected.videoOffset);
            QCOMPARE(asset.motion->embedded.length, expected.videoLength);
            QVERIFY(asset.motion->presentationTimestampUs.has_value());
            QCOMPARE(*asset.motion->presentationTimestampUs, expected.presentationUs);

            ViewerPreferences preferences;
            ImageResourcePolicy policy(preferences);
            MotionPhotoPlayback playback(policy);
            QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
            playback.play(asset);
            QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !playback.error().isEmpty(), 15000);
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY2(qualifiedPrivateFfmpegIsMapped(),
                     "Real Xiaomi playback did not load the qualified private FFmpeg plugin");
            QCOMPARE(frames.first()[0].toSize(), expected.frameSize);
            QVERIFY(playback.duration() >= expected.minimumDurationMs &&
                    playback.duration() <= expected.maximumDurationMs);
            QVERIFY(playback.seekable());
            QVERIFY(!policy.processingMutex().tryLock());

            const qint64 targetMs = std::max<qint64>(1, playback.duration() / 2);
            const qint64 targetTimestampUs = targetMs * 1000;
            frames.clear();
            playback.seek(targetMs);
            const auto hasPostSeekFrame = [&frames, targetTimestampUs]() {
                return std::any_of(frames.cbegin(), frames.cend(),
                                   [targetTimestampUs](const QList<QVariant>& frame) {
                                       return frame.size() > 1 &&
                                              frame[1].toLongLong() >= targetTimestampUs;
                                   });
            };
            QTRY_VERIFY_WITH_TIMEOUT(hasPostSeekFrame() || !playback.error().isEmpty(), 15000);
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY2(hasPostSeekFrame(),
                     "Real Xiaomi seek did not produce a frame at/after the requested position");
            playback.stop();
            QTRY_VERIFY(!playback.active());
            QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
            QVERIFY(policy.processingMutex().tryLock());
            policy.processingMutex().unlock();
        }
#endif
    }

    void realAppleHeicMovPairProbeAndPlayback()
    {
#if !defined(LICASA_APPLE_REAL_HEIC_FIXTURE) || !defined(LICASA_APPLE_REAL_MOV_FIXTURE)
        QSKIP("Configure the pinned real Apple HEIC+MOV fixture for final qualification");
#else
        const QString still = QStringLiteral(LICASA_APPLE_REAL_HEIC_FIXTURE);
        const QString movie = QStringLiteral(LICASA_APPLE_REAL_MOV_FIXTURE);
        QVERIFY2(QFileInfo::exists(still), qPrintable(still));
        QVERIFY2(QFileInfo::exists(movie), qPrintable(movie));

        const PhotoAssetInfo asset = probePhotoAsset(QUrl::fromLocalFile(still));
        QVERIFY2(asset.metadataError.isEmpty(), qPrintable(asset.metadataError));
        QVERIFY(asset.kind == PhotoAssetKind::MotionPhoto);
        QVERIFY(asset.motion.has_value());
        QVERIFY(!asset.motion->hasEmbeddedVideo());
        QVERIFY(asset.motion->hasExternalVideo());
        QCOMPARE(asset.motion->externalVideoUrl, QUrl::fromLocalFile(movie));
        QVERIFY(asset.motion->externalVideoIdentity.has_value());
        QCOMPARE(asset.motion->externalVideoIdentity->size, quint64(QFileInfo(movie).size()));
        QCOMPARE(asset.motion->mimeType, QStringLiteral("video/quicktime"));

        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        playback.play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !playback.error().isEmpty(), 15000);
        QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
        QVERIFY2(qualifiedPrivateFfmpegIsMapped(),
                 "Real Apple Live Photo playback did not load the qualified private FFmpeg plugin");
        QVERIFY(frames.first()[0].toSize().width() > 0);
        QVERIFY(frames.first()[0].toSize().height() > 0);
        QVERIFY(playback.duration() > 0);
        QVERIFY(playback.seekable());
        QVERIFY(!policy.processingMutex().tryLock());

        const qint64 targetMs = std::max<qint64>(1, playback.duration() / 2);
        frames.clear();
        playback.seek(targetMs);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !playback.error().isEmpty(), 15000);
        QVERIFY2(playback.error().isEmpty(), qPrintable(playback.error()));
        playback.stop();
        QTRY_VERIFY(!playback.active());
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
#endif
    }

    void externalReplacementIsRejectedBeforeBackendActivation()
    {
#if !defined(LICASA_APPLE_EXTERNAL_MOV_FIXTURE) || !defined(LICASA_APPLE_EXTERNAL_MOV_IDENTIFIER)
        QSKIP("Configure the generated Apple external MOV fixture for runtime qualification");
#else
        const bool requireLazy =
            qEnvironmentVariableIsSet("LICASA_EXPECT_EXTERNAL_PREPLAYBACK_LAZY");
        if (requireLazy) {
            QVERIFY2(!ffmpegBackendIsMapped(),
                     "FFmpeg was already mapped before the external replacement preflight");
        }

        QTemporaryDir directory;
        QString moviePath;
        const auto asset = externalFixtureAsset(directory, &moviePath);
        QVERIFY(asset.kind == PhotoAssetKind::MotionPhoto);
        QVERIFY(asset.motion.has_value());
        QVERIFY(asset.motion->externalVideoIdentity.has_value());

        // Replace the validated directory entry with byte-identical media. A
        // path-only design would accept this; the captured inode identity must
        // reject it before QMediaPlayer/FFmpeg exists.
        const QString replacement = directory.filePath(QStringLiteral("replacement.MOV"));
        QVERIFY(QFile::copy(QStringLiteral(LICASA_APPLE_EXTERNAL_MOV_FIXTURE), replacement));
        QVERIFY(QFile::remove(moviePath));
        QVERIFY(QFile::rename(replacement, moviePath));

        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QSignalSpy frames(&playback, &MotionPhotoPlayback::frameDecoded);
        QSignalSpy changes(&playback, &MotionPhotoPlayback::changed);
        playback.play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(!playback.error().isEmpty(), 2000);
        QVERIFY(playback.error().contains(QStringLiteral("changed after Live Photo pairing")));
        QVERIFY(!playback.active());
        QVERIFY(frames.isEmpty());
        QVERIFY(changes.count() > 0);
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
        if (requireLazy) {
            QVERIFY2(!ffmpegBackendIsMapped(),
                     "A replaced external MOV activated FFmpeg before identity rejection");
        }
#endif
    }

    void replacementAndDestruction()
    {
#ifndef LICASA_HEIC_MOTION_FIXTURE
        QSKIP("Configure the external S23 fixture for runtime qualification");
#else
        const auto asset = fixture();
        QVERIFY(asset.motion.has_value());
        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        auto playback = std::make_unique<MotionPhotoPlayback>(policy);
        QSignalSpy frames(playback.get(), &MotionPhotoPlayback::frameDecoded);
        playback->play(asset);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 12000);
        playback->seek(1400);
        playback->play(asset);
        frames.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 12000);
        QVERIFY(frames.first()[1].toLongLong() < 1000000);
        playback.reset();
        QVERIFY(policy.processingMutex().tryLock());
        policy.processingMutex().unlock();
        QCoreApplication::processEvents(); // queued callbacks of dead sessions
#endif
    }
};

int main(int argc, char** argv)
{
    ImageResourcePolicy::initializeDecoderEnvironment();
    QGuiApplication app(argc, argv);
    initializeImagePlugins();
    QTemporaryDir settings;
    if (!settings.isValid()) {
        return 2;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("LicasaTests"));
    QCoreApplication::setApplicationName(QStringLiteral("MotionPhotoPlayback"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    if (app.arguments().contains(QStringLiteral("--measure"))) {
        const auto asset = fixture();
        if (!asset.motion) {
            return 3;
        }
        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        QJsonObject report{{"before", processMemory()}};
        QJsonArray cycles;
        QJsonObject cycle;
        QElapsedTimer elapsed;
        bool first = true;
        bool stopping = false;
        auto begin = [&] {
            first = true;
            stopping = false;
            cycle = {};
            elapsed.start();
            playback.play(asset);
        };
        QObject::connect(&playback, &MotionPhotoPlayback::frameDecoded, &app,
                         [&](const QSize& size, qint64 timestamp) {
                             if (!qualifiedPrivateFfmpegIsMapped()) {
                                 app.exit(8);
                                 return;
                             }
                             if (first) {
                                 first = false;
                                 cycle["first_frame_ms"] = elapsed.nsecsElapsed() / 1e6;
                                 cycle["width"] = size.width();
                                 cycle["height"] = size.height();
                                 cycle["duration_ms"] = double(playback.duration());
                                 cycle["seekable"] = playback.seekable();
                                 cycle["frame_process"] = processMemory();
                                 playback.seek(1399);
                             } else if (!stopping && timestamp >= 1399000) {
                                 cycle["post_seek_timestamp_us"] = double(timestamp);
                                 stopping = true;
                                 playback.stop();
                             }
                         });
        QObject::connect(&playback, &MotionPhotoPlayback::changed, &app, [&] {
            if (!playback.error().isEmpty()) {
                app.exit(5);
                return;
            }
            if (!stopping || playback.active()) {
                return;
            }
            stopping = false;
            QTimer::singleShot(100, &app, [&] {
                cycle["after_close"] = processMemory();
                cycles.append(cycle);
                if (cycles.size() == 3) {
                    app.quit();
                } else {
                    begin();
                }
            });
        });
        QTimer::singleShot(12000, &app, [&] { app.exit(6); });
        begin();
        const int result = app.exec();
        if (result || cycles.size() != 3 || playback.active()) {
            return 7;
        }
        report["cycles"] = cycles;
        report["after"] = processMemory();
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--normal-exit"))) {
        const auto asset = fixture();
        if (!asset.motion) {
            return 3;
        }
        ViewerPreferences preferences;
        ImageResourcePolicy policy(preferences);
        MotionPhotoPlayback playback(policy);
        bool sought = false;
        bool completed = false;
        QObject::connect(&playback, &MotionPhotoPlayback::frameDecoded, &app,
                         [&](const QSize& size, qint64 timestamp) {
                             if (!qualifiedPrivateFfmpegIsMapped()) {
                                 app.exit(9);
                                 return;
                             }
                             if (size != QSize(1440, 1080)) {
                                 app.exit(4);
                                 return;
                             }
                             if (!sought) {
                                 sought = true;
                                 playback.seek(1399);
                             } else if (timestamp >= 1399000) {
                                 completed = true;
                                 app.quit();
                             }
                         });
        QObject::connect(&playback, &MotionPhotoPlayback::changed, &app, [&] {
            if (!playback.error().isEmpty()) {
                app.exit(5);
            }
        });
        QTimer::singleShot(12000, &app, [&] { app.exit(6); });
        playback.play(asset);
        const int result = app.exec();
        if (result || !completed || playback.active()) {
            return 7;
        }
        if (!policy.processingMutex().tryLock()) {
            return 8;
        }
        policy.processingMutex().unlock();
        qInfo("NORMAL QT EXIT AND TEARDOWN: PASS");
        return 0;
    }
    MotionPhotoPlaybackTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_motion_photo_playback.moc"
