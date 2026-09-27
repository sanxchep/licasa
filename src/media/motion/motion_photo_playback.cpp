#include "media/motion/motion_photo_playback.h"

#include "imaging/image_decode_contract.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Licasa {
namespace {

constexpr quint32 fourCc(char a, char b, char c, char d) noexcept
{
    return (quint32(quint8(a)) << 24) | (quint32(quint8(b)) << 16) | (quint32(quint8(c)) << 8) |
           quint32(quint8(d));
}

struct IsoBox {
    quint64 start = 0;
    quint64 payload = 0;
    quint64 end = 0;
    quint32 type = 0;
};

constexpr quint32 kMaximumIsoMetadataWorkItems = 8192;

enum class IsoScanStatus {
    Complete,
    Incomplete,
    WorkLimitExceeded,
};

struct IsoScanBudget {
    quint32 remaining = kMaximumIsoMetadataWorkItems;

    bool consume(quint32 amount = 1) noexcept
    {
        if (amount > remaining) {
            return false;
        }
        remaining -= amount;
        return true;
    }
};

bool readExactAt(QIODevice& device, quint64 offset, char* data, qsizetype size)
{
    if (size < 0 || offset > quint64(std::numeric_limits<qint64>::max()) ||
        quint64(size) > quint64(std::numeric_limits<qint64>::max()) - offset) {
        return false;
    }
    if (!device.seek(qint64(offset))) {
        return false;
    }
    return device.read(data, size) == size;
}

std::optional<IsoBox> readIsoBox(QIODevice& device, quint64 start, quint64 parentEnd)
{
    if (start > parentEnd || parentEnd - start < 8) {
        return std::nullopt;
    }
    char header[16]{};
    if (!readExactAt(device, start, header, 8)) {
        return std::nullopt;
    }
    const auto* bytes = reinterpret_cast<const uchar*>(header);
    const quint32 shortSize = qFromBigEndian<quint32>(bytes);
    const quint32 type = qFromBigEndian<quint32>(bytes + 4);
    quint64 headerSize = 8;
    quint64 boxSize = shortSize;
    if (shortSize == 1) {
        if (parentEnd - start < 16 || !readExactAt(device, start + 8, header + 8, 8)) {
            return std::nullopt;
        }
        boxSize = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(header + 8));
        headerSize = 16;
    } else if (shortSize == 0) {
        boxSize = parentEnd - start;
    }
    if (boxSize < headerSize || boxSize > parentEnd - start) {
        return std::nullopt;
    }
    return IsoBox{start, start + headerSize, start + boxSize, type};
}

bool isVisualSampleEntry(quint32 type) noexcept
{
    return type == fourCc('a', 'v', 'c', '1') || type == fourCc('a', 'v', 'c', '3') ||
           type == fourCc('h', 'v', 'c', '1') || type == fourCc('h', 'e', 'v', '1') ||
           type == fourCc('m', 'p', '4', 'v') || type == fourCc('a', 'v', '0', '1') ||
           type == fourCc('v', 'p', '0', '8') || type == fourCc('v', 'p', '0', '9') ||
           type == fourCc('j', 'p', 'e', 'g');
}

IsoScanStatus scanSampleDescriptions(QIODevice& device, const IsoBox& stsd,
                                     std::vector<QSize>& sizes, IsoScanBudget& budget)
{
    if (stsd.end - stsd.payload < 8) {
        return IsoScanStatus::Incomplete;
    }
    char fullBox[8]{};
    if (!readExactAt(device, stsd.payload, fullBox, 8)) {
        return IsoScanStatus::Incomplete;
    }
    const quint32 entryCount = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(fullBox + 4));
    // A real video track has only a handful of sample descriptions. Keep a
    // strict per-table and aggregate work bound. Reserving the complete table
    // before scanning prevents many individually valid stsd boxes from evading
    // the global limit.
    if (entryCount > 4096 || !budget.consume(entryCount)) {
        return IsoScanStatus::WorkLimitExceeded;
    }

    quint64 cursor = stsd.payload + 8;
    for (quint32 index = 0; index < entryCount; ++index) {
        const auto entry = readIsoBox(device, cursor, stsd.end);
        if (!entry) {
            return IsoScanStatus::Incomplete;
        }
        if (isVisualSampleEntry(entry->type)) {
            // ISO/IEC 14496-12 VisualSampleEntry: width/height are 16-bit
            // big-endian fields at byte offsets 32/34 from the entry start.
            if (entry->end - entry->start < 36) {
                return IsoScanStatus::Incomplete;
            }
            char dimensions[4]{};
            if (!readExactAt(device, entry->start + 32, dimensions, 4)) {
                return IsoScanStatus::Incomplete;
            }
            const auto* bytes = reinterpret_cast<const uchar*>(dimensions);
            const int width = int(qFromBigEndian<quint16>(bytes));
            const int height = int(qFromBigEndian<quint16>(bytes + 2));
            if (width > 0 && height > 0) {
                sizes.emplace_back(width, height);
            }
        }
        cursor = entry->end;
    }
    return IsoScanStatus::Complete;
}

std::optional<int> trackPresentationRotation(QIODevice& device, const IsoBox& tkhd)
{
    // ISO/IEC 14496-12 TrackHeaderBox carries a 3x3 fixed-point display
    // matrix. Qt 6.4's FFmpeg backend does not propagate this track transform
    // to QVideoFrame::rotationAngle(), so retain the four orthogonal rotations
    // needed by phone Motion Photos. Translation terms are intentionally
    // irrelevant to a decoded frame raster.
    char versionAndFlags[4]{};
    if (!readExactAt(device, tkhd.payload, versionAndFlags, 4)) {
        return std::nullopt;
    }
    const quint8 version = quint8(versionAndFlags[0]);
    if (version > 1) {
        return std::nullopt;
    }
    const quint64 matrixOffset = version == 1 ? 52 : 40;
    if (tkhd.end - tkhd.payload < matrixOffset + 36) {
        return std::nullopt;
    }

    char matrix[36]{};
    if (!readExactAt(device, tkhd.payload + matrixOffset, matrix, 36)) {
        return std::nullopt;
    }
    const auto fixed = [&matrix](qsizetype offset) {
        return static_cast<qint32>(
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(matrix + offset)));
    };
    const qint32 a = fixed(0);
    const qint32 b = fixed(4);
    const qint32 c = fixed(12);
    const qint32 d = fixed(16);
    constexpr qint32 one = 1 << 16;
    if (a == one && b == 0 && c == 0 && d == one) {
        return 0;
    }
    if (a == 0 && b == one && c == -one && d == 0) {
        return 90;
    }
    if (a == -one && b == 0 && c == 0 && d == -one) {
        return 180;
    }
    if (a == 0 && b == -one && c == one && d == 0) {
        return -90;
    }
    return std::nullopt;
}

IsoScanStatus scanIsoBoxes(QIODevice& device, quint64 begin, quint64 end, int depth,
                           std::vector<QSize>& sizes, std::vector<int>& rotations,
                           IsoScanBudget& budget)
{
    if (depth > 8) {
        return IsoScanStatus::WorkLimitExceeded;
    }
    if (begin > end) {
        return IsoScanStatus::Incomplete;
    }
    quint64 cursor = begin;
    while (cursor < end) {
        if (!budget.consume()) {
            return IsoScanStatus::WorkLimitExceeded;
        }
        const auto box = readIsoBox(device, cursor, end);
        if (!box) {
            return IsoScanStatus::Incomplete;
        }
        if (box->type == fourCc('s', 't', 's', 'd')) {
            const auto status = scanSampleDescriptions(device, *box, sizes, budget);
            if (status != IsoScanStatus::Complete) {
                return status;
            }
        } else if (box->type == fourCc('t', 'k', 'h', 'd')) {
            const auto rotation = trackPresentationRotation(device, *box);
            if (rotation && *rotation != 0) {
                rotations.push_back(*rotation);
            }
        } else if (box->type == fourCc('m', 'o', 'o', 'v') ||
                   box->type == fourCc('t', 'r', 'a', 'k') ||
                   box->type == fourCc('m', 'd', 'i', 'a') ||
                   box->type == fourCc('m', 'i', 'n', 'f') ||
                   box->type == fourCc('s', 't', 'b', 'l')) {
            const auto status =
                scanIsoBoxes(device, box->payload, box->end, depth + 1, sizes, rotations, budget);
            if (status != IsoScanStatus::Complete) {
                return status;
            }
        }
        if (box->end <= cursor) {
            return IsoScanStatus::Incomplete;
        }
        cursor = box->end;
    }
    return cursor == end ? IsoScanStatus::Complete : IsoScanStatus::Incomplete;
}

struct DeclaredVideoDimensions {
    std::optional<QSize> size;
    std::optional<int> presentationRotationDegrees;
    IsoScanStatus status = IsoScanStatus::Incomplete;
};

DeclaredVideoDimensions declaredVideoDimensions(QIODevice& device)
{
    DeclaredVideoDimensions result;
    if (!device.isOpen() || device.size() <= 0) {
        return result;
    }
    std::vector<QSize> sizes;
    std::vector<int> rotations;
    IsoScanBudget budget;
    result.status = scanIsoBoxes(device, 0, quint64(device.size()), 0, sizes, rotations, budget);
    // QMediaPlayer receives the device from the beginning regardless of where
    // the bounded metadata probe stopped. A size found on a checked stsd path
    // remains useful even if a later unrelated box is malformed.
    if (!device.seek(0)) {
        if (result.status == IsoScanStatus::Complete) {
            result.status = IsoScanStatus::Incomplete;
        }
        return result;
    }
    if (sizes.empty()) {
        return result;
    }
    result.size =
        *std::max_element(sizes.cbegin(), sizes.cend(), [](const QSize& left, const QSize& right) {
            return quint64(left.width()) * quint64(left.height()) <
                   quint64(right.width()) * quint64(right.height());
        });
    if (!rotations.empty() &&
        std::all_of(rotations.cbegin(), rotations.cend(),
                    [&rotations](int rotation) { return rotation == rotations.front(); })) {
        result.presentationRotationDegrees = rotations.front();
    }
    return result;
}

std::optional<MotionPhotoFrameRaster> rasterFromVideoFrame(const QVideoFrame& frame,
                                                           quint64 admittedPixels,
                                                           int fallbackRotationDegrees = 0)
{
    if (!frame.isValid() || !ImageDecodeContract::allows(frame.size(), admittedPixels)) {
        return std::nullopt;
    }

    // QVideoFrame::toImage() intentionally does not apply presentation
    // transforms. Apply them before the frame crosses the private backend ABI so
    // the public-side presentation surface receives exactly one owned RGBA view.
    QImage image = frame.toImage();
    if (image.isNull() || !ImageDecodeContract::allows(image.size(), admittedPixels)) {
        return std::nullopt;
    }

    const int frameRotationDegrees = static_cast<int>(frame.rotationAngle());
    const int rotationDegrees =
        frameRotationDegrees != 0 ? frameRotationDegrees : fallbackRotationDegrees;
    if (rotationDegrees != 0) {
        image = image.transformed(QTransform().rotate(rotationDegrees), Qt::SmoothTransformation);
    }
    if (frame.mirrored()) {
        image = image.mirrored(true, false);
    }
    if (image.isNull() || !ImageDecodeContract::allows(image.size(), admittedPixels)) {
        return std::nullopt;
    }

    const QByteArray iccProfile =
        image.colorSpace().isValid() ? image.colorSpace().iccProfile() : QByteArray{};
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull() || !image.constBits() || image.bytesPerLine() <= 0) {
        return std::nullopt;
    }

    const qsizetype byteCount = image.sizeInBytes();
    if (byteCount <= 0) {
        return std::nullopt;
    }

    MotionPhotoFrameRaster raster;
    raster.rgba8888 = QByteArray(reinterpret_cast<const char*>(image.constBits()), byteCount);
    raster.iccProfile = iccProfile;
    raster.size = image.size();
    raster.bytesPerLine = image.bytesPerLine();
    raster.timestampUs = frame.startTime();
    if (!raster.isValid()) {
        return std::nullopt;
    }
    return raster;
}

} // namespace

struct MotionPhotoPlayback::Session {
    struct PreviewSeekState {
        std::mutex mutex;
        qint64 targetUs = 0;
        quint64 request = 0;
        bool active = false;
        bool completing = false;
    };

    struct PendingFrame {
        QSize size;
        qint64 timestampUs = -1;
        std::optional<MotionPhotoFrameRaster> raster;
        quint64 previewRequest = 0;
        bool completesPreview = false;
        bool conversionFailed = false;
    };

    struct FrameHandoff {
        std::mutex mutex;
        std::optional<PendingFrame> latest;
        bool callbackQueued = false;
    };

    // Explicit clear() orders destruction; the device is always last.
    std::unique_ptr<ByteRangeDevice> device;
    std::unique_ptr<QVideoSink> sink;
    std::unique_ptr<QMediaPlayer> player;
    std::optional<PhotoAssetInfo> asset;
    quint64 pixels = 0;
    int presentationRotationDegrees = 0;
    bool admitted = false;
    bool frameSeen = false;
    // A visible Pause intent may race a queued EndOfMedia notification from
    // Qt Multimedia. Preserve the admitted session until the user explicitly
    // resumes/seeks/stops instead of letting that stale EOS tear it down.
    bool pauseRequested = false;
    bool previewedEnd = false;
    bool lockHeld = false;
    std::shared_ptr<PreviewSeekState> previewSeek = std::make_shared<PreviewSeekState>();
    std::shared_ptr<FrameHandoff> frameHandoff = std::make_shared<FrameHandoff>();
};

MotionPhotoPlayback::MotionPhotoPlayback(MotionPhotoResourcePolicy& policy, QObject* parent)
    : QObject(parent), policy_(policy)
{
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        ++generation_;
        clear();
    });
}

MotionPhotoPlayback::~MotionPhotoPlayback()
{
    Q_ASSERT(thread() == QThread::currentThread());
    clear();
}

bool MotionPhotoPlayback::active() const { return bool(session_); }
bool MotionPhotoPlayback::playing() const
{
    return session_ && session_->player && !session_->pauseRequested &&
           session_->player->playbackState() == QMediaPlayer::PlayingState;
}
bool MotionPhotoPlayback::seekable() const
{
    return session_ && session_->admitted && session_->player->isSeekable();
}
qint64 MotionPhotoPlayback::position() const
{
    return session_ && session_->player ? session_->player->position() : 0;
}

qint64 MotionPhotoPlayback::duration() const
{
    return session_ && session_->player ? session_->player->duration() : 0;
}

std::optional<MotionPhotoFrameRaster> MotionPhotoPlayback::captureCurrentFrame() const
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!session_ || !session_->admitted || !session_->sink) {
        return std::nullopt;
    }
    return rasterFromVideoFrame(session_->sink->videoFrame(), session_->pixels,
                                session_->presentationRotationDegrees);
}

void MotionPhotoPlayback::setFrameReadyCallback(FrameReadyCallback callback)
{
    Q_ASSERT(thread() == QThread::currentThread());
    frameReadyCallback_ = std::move(callback);
}

void MotionPhotoPlayback::clear()
{
    seekQueued_ = false;
    if (!session_) {
        return;
    }
    // No callback may enter the controller while Qt synchronously drains its
    // decoder threads. Invalidate all queued callbacks with generation_ as well.
    if (session_->sink) {
        disconnect(session_->sink.get(), nullptr, this, nullptr);
    }
    if (session_->player) {
        disconnect(session_->player.get(), nullptr, this, nullptr);
        session_->player->stop();
        session_->player->setSource(QUrl());
        session_->player.reset();
    }
    session_->sink.reset();
    session_->device.reset();
    if (session_->lockHeld) {
        policy_.processingMutex().unlock();
    }
    session_.reset();
}

void MotionPhotoPlayback::fail(const QString& message)
{
    ++generation_;
    clear();
    error_ = message;
    emit changed();
}

void MotionPhotoPlayback::play(const PhotoAssetInfo& asset)
{
    Q_ASSERT(thread() == QThread::currentThread());
    const quint64 generation = ++generation_;
    QTimer::singleShot(0, this, [this, asset, generation] {
        if (generation == generation_) {
            start(asset, generation);
        }
    });
}

void MotionPhotoPlayback::start(const PhotoAssetInfo& asset, quint64 generation)
{
    clear();
    error_.clear();
    if (asset.kind != PhotoAssetKind::MotionPhoto || !asset.motion ||
        !asset.sourceUrl.isLocalFile()) {
        fail(QStringLiteral("Playback requires a validated local Motion Photo asset."));
        return;
    }
    const bool embedded = asset.motion->hasEmbeddedVideo();
    const bool external = asset.motion->hasExternalVideo();
    if (embedded == external) {
        fail(QStringLiteral("Motion Photo playback requires exactly one validated video source."));
        return;
    }
    QString videoPath;
    quint64 videoOffset = 0;
    quint64 videoLength = 0;
    std::optional<ExternalFileIdentity> expectedIdentity;
    QUrl formatHint;
    if (embedded) {
        if (asset.motion->mimeType != QStringLiteral("video/mp4")) {
            fail(QStringLiteral("Embedded Motion Photo playback requires a validated MP4 range."));
            return;
        }
        videoPath = asset.sourceUrl.toLocalFile();
        videoOffset = asset.motion->embedded.offset;
        videoLength = asset.motion->embedded.length;
        formatHint = QUrl(QStringLiteral("file:///licasa-bounded-motion-photo.mp4"));
    } else {
        if (asset.motion->mimeType != QStringLiteral("video/quicktime") ||
            !asset.motion->externalVideoIdentity ||
            asset.motion->externalVideoIdentity->size == 0) {
            fail(QStringLiteral(
                "External Live Photo playback requires a pinned validated MOV identity."));
            return;
        }
        videoPath = asset.motion->externalVideoUrl.toLocalFile();
        videoLength = asset.motion->externalVideoIdentity->size;
        expectedIdentity = asset.motion->externalVideoIdentity;
        // Never give the media backend the real paired pathname. The synthetic
        // URL supplies only a .mov format hint; the pinned QIODevice is the
        // sole byte source.
        formatHint = QUrl(QStringLiteral("file:///licasa-pinned-live-photo.mov"));
    }
    // Only FFmpeg passed the bounded-device gate on Qt 6.4.2. Respect explicit
    // overrides by rejecting an unqualified backend instead of silently changing it.
    const QByteArray backend = qgetenv("QT_MEDIA_BACKEND");
    if (!backend.isEmpty() && backend != "ffmpeg") {
        fail(QStringLiteral("Bounded Motion Photo playback requires the Qt FFmpeg backend."));
        return;
    }
    session_ = std::make_unique<Session>();
    session_->asset = asset;
    if (expectedIdentity) {
        session_->device = std::make_unique<ByteRangeDevice>(videoPath, videoOffset, videoLength,
                                                             *expectedIdentity);
    } else {
        session_->device = std::make_unique<ByteRangeDevice>(videoPath, videoOffset, videoLength);
    }
    if (!session_->device->open(QIODevice::ReadOnly)) {
        fail(session_->device->errorString());
        return;
    }
    // Never wait for a still decode on the GUI thread. One playback session
    // owns the existing heavy-operation gate until stop/error/end/destruction.
    if (!policy_.processingMutex().tryLock()) {
        fail(QStringLiteral(
            "Image processing is busy; request motion playback again when it finishes."));
        return;
    }
    session_->lockHeld = true;
    session_->pixels = quint64(policy_.prepareForProcessing()) * 1000000;
    // Inspect only checked ISO-BMFF box headers/sample-table metadata first.
    // This happens before QMediaPlayer exists, so an obviously over-budget
    // declared video cannot activate FFmpeg or allocate decoder surfaces.
    const auto declaredDimensions = declaredVideoDimensions(*session_->device);
    if (declaredDimensions.status == IsoScanStatus::WorkLimitExceeded) {
        fail(QStringLiteral("Motion Photo metadata exceeds the playback complexity limit."));
        return;
    }
    if (declaredDimensions.size &&
        !ImageDecodeContract::allows(*declaredDimensions.size, session_->pixels)) {
        fail(QStringLiteral("Video dimensions exceed the image resource policy before playback."));
        return;
    }
    session_->presentationRotationDegrees =
        declaredDimensions.presentationRotationDegrees.value_or(0);
    if (backend.isEmpty()) {
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    }
    session_->player = std::make_unique<QMediaPlayer>();
    session_->sink = std::make_unique<QVideoSink>();
    connectPlayerSignals(generation);
    connectFrameSink(generation);

    auto* player = session_->player.get();
    // The URL is only a format hint. The opened, bounded/pinned device is the
    // input. External Live Photos therefore cannot fall back to reopening the
    // path that was validated by the background pairing probe.
    player->setSourceDevice(session_->device.get(), formatHint);
    if (player->sourceDevice() != session_->device.get()) {
        fail(QStringLiteral("The media backend did not retain the bounded source device."));
        return;
    }
    QTimer::singleShot(10000, this, [this, generation] {
        if (generation == generation_ && session_ && !session_->frameSeen) {
            fail(QStringLiteral("Timed out waiting for the first video frame."));
        }
    });
    // FFmpeg 6.4.2 can finish metadata synchronously inside setSourceDevice.
    admitAndPlay();
}

void MotionPhotoPlayback::connectPlayerSignals(quint64 generation)
{
    auto* player = session_->player.get();
    // Every multimedia callback crosses an event-loop boundary before it can
    // seek, stop or delete a decoder. The frame callback drops pixel data below.
    connect(
        player, &QMediaPlayer::errorOccurred, this,
        [this, generation](QMediaPlayer::Error, const QString& message) {
            if (generation == generation_) {
                fail(message);
            }
        },
        Qt::QueuedConnection);
    connect(
        player, &QMediaPlayer::mediaStatusChanged, this,
        [this, generation](QMediaPlayer::MediaStatus status) {
            if (generation != generation_) {
                return;
            }
            if (status == QMediaPlayer::EndOfMedia) {
                // Natural EOF is transport state, not session teardown. Keep
                // the admitted player, bounded source device and last owned
                // presentation frame alive so the visible controls can remain
                // at the end position and the user can seek or replay. Explicit
                // stop/lifecycle transitions still release the decoder and the
                // heavy-processing gate. This also removes the tiny window in
                // which a short clip could end between the visible-frame check
                // and a Pause click, causing the controls to disappear.
                if (!session_ || !session_->player) {
                    return;
                }
                emit changed();
                return;
            }
            if (status == QMediaPlayer::InvalidMedia) {
                fail(session_->player->errorString());
                return;
            }
            admitAndPlay();
        },
        Qt::QueuedConnection);
    connect(
        player, &QMediaPlayer::metaDataChanged, this,
        [this, generation] {
            if (generation == generation_) {
                admitAndPlay();
            }
        },
        Qt::QueuedConnection);
    connect(
        player, &QMediaPlayer::playbackStateChanged, this,
        [this, generation](QMediaPlayer::PlaybackState state) {
            if (generation != generation_) {
                return;
            }
            // PlayingState alone does not prove visible replay. Keep the replay
            // guard until a fresh decoded frame reaches the presentation path.
            Q_UNUSED(state);
            emit changed();
        },
        Qt::QueuedConnection);
    connect(
        player, &QMediaPlayer::positionChanged, this,
        [this, generation] {
            if (generation == generation_) {
                emit changed();
            }
        },
        Qt::QueuedConnection);
    connect(
        player, &QMediaPlayer::durationChanged, this,
        [this, generation] {
            if (generation == generation_) {
                emit changed();
            }
        },
        Qt::QueuedConnection);
    connect(
        player, &QMediaPlayer::seekableChanged, this,
        [this, generation] {
            if (generation == generation_) {
                emit changed();
            }
        },
        Qt::QueuedConnection);
}

void MotionPhotoPlayback::connectFrameSink(quint64 generation)
{
    // Qt delivers sink frames on a backend thread on some runtimes. Keep one
    // replaceable owned frame waiting for the GUI loop: a newer frame replaces
    // the pending one instead of being dropped. This matters for paused seeks,
    // where Qt can deliver a catch-up keyframe and then the requested frame
    // before the GUI callback runs. QVideoFrame itself never crosses the
    // thread/module boundary.
    auto invalidFrame = std::make_shared<std::atomic_bool>(false);
    const auto previewSeek = session_->previewSeek;
    const auto frameHandoff = session_->frameHandoff;
    const quint64 admittedPixels = session_->pixels;
    const int presentationRotationDegrees = session_->presentationRotationDegrees;
    const bool copyPresentationFrames = bool(frameReadyCallback_);
    connect(
        session_->sink.get(), &QVideoSink::videoFrameChanged, this,
        [this, generation, previewSeek, frameHandoff, invalidFrame, admittedPixels,
         presentationRotationDegrees, copyPresentationFrames](const QVideoFrame& frame) {
            if (!frame.isValid()) {
                return;
            }
            if (!ImageDecodeContract::allows(frame.size(), admittedPixels)) {
                invalidFrame->store(true);
            }

            const QSize size = frame.size();
            const qint64 timestamp = frame.startTime();
            const qint64 endTimestamp = frame.endTime();
            quint64 previewRequest = 0;
            bool completesPreview = false;
            {
                std::lock_guard<std::mutex> lock(previewSeek->mutex);
                if (previewSeek->completing) {
                    return;
                }
                if (previewSeek->active) {
                    const bool reachesTarget =
                        timestamp >= previewSeek->targetUs ||
                        (endTimestamp >= 0 && endTimestamp >= previewSeek->targetUs);
                    if (!reachesTarget) {
                        return;
                    }
                    previewSeek->active = false;
                    previewSeek->completing = true;
                    previewRequest = previewSeek->request;
                    completesPreview = true;
                }
            }

            const std::optional<MotionPhotoFrameRaster> raster =
                copyPresentationFrames
                    ? rasterFromVideoFrame(frame, admittedPixels, presentationRotationDegrees)
                    : std::nullopt;
            const bool conversionFailed = copyPresentationFrames && !raster.has_value();
            const QSize presentationSize = raster ? raster->size : size;

            bool queueCallback = false;
            {
                std::lock_guard<std::mutex> lock(frameHandoff->mutex);
                frameHandoff->latest = Session::PendingFrame{
                    presentationSize, timestamp,        raster,
                    previewRequest,   completesPreview, conversionFailed,
                };
                if (!frameHandoff->callbackQueued) {
                    frameHandoff->callbackQueued = true;
                    queueCallback = true;
                }
            }
            if (!queueCallback) {
                return;
            }

            QMetaObject::invokeMethod(
                this,
                [this, generation, previewSeek, frameHandoff, invalidFrame,
                 copyPresentationFrames]() mutable {
                    std::optional<Session::PendingFrame> pending;
                    {
                        std::lock_guard<std::mutex> lock(frameHandoff->mutex);
                        pending = std::move(frameHandoff->latest);
                        frameHandoff->latest.reset();
                        frameHandoff->callbackQueued = false;
                    }
                    if (generation != generation_ || !session_ || !pending) {
                        return;
                    }
                    if (invalidFrame->load()) {
                        fail(QStringLiteral(
                            "Decoded video frame exceeds the image resource policy."));
                        return;
                    }
                    if (pending->conversionFailed || (copyPresentationFrames && !pending->raster)) {
                        fail(QStringLiteral(
                            "Decoded video frame could not be converted for presentation."));
                        return;
                    }

                    bool pauseAfterPreview = false;
                    if (pending->completesPreview) {
                        std::lock_guard<std::mutex> lock(previewSeek->mutex);
                        if (previewSeek->request == pending->previewRequest &&
                            previewSeek->completing) {
                            previewSeek->completing = false;
                            pauseAfterPreview = true;
                        }
                    }

                    session_->frameSeen = true;
                    emit frameDecoded(pending->size, pending->timestampUs);
                    if (pending->raster && frameReadyCallback_) {
                        frameReadyCallback_(std::move(*pending->raster));
                    }

                    if (pauseAfterPreview && session_ && session_->player) {
                        session_->pauseRequested = true;
                        session_->player->pause();
                        emit changed();
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
}

void MotionPhotoPlayback::admitAndPlay()
{
    if (!session_ || session_->admitted) {
        return;
    }
    auto* player = session_->player.get();
    const QSize size = player->metaData().value(QMediaMetaData::Resolution).toSize();
    if (!size.isValid() && player->mediaStatus() == QMediaPlayer::LoadingMedia) {
        return;
    }
    if (!player->hasVideo() || !ImageDecodeContract::allows(size, session_->pixels)) {
        fail(QStringLiteral(
            "Video dimensions are unavailable or exceed the image resource policy."));
        return;
    }
    session_->admitted = true;
    session_->pauseRequested = false;
    // Qt 6.4.2 creates its video renderer/decoder when a sink is attached.
    // Keep that attachment after metadata admission, not before setSourceDevice.
    player->setVideoSink(session_->sink.get());
    // No audio output is allocated in this first, silent C++ increment.
    // Qt 6.4.2 FFmpeg dereferences its absent audio renderer when changing
    // tracks without an audio output. Leave the default tracks untouched.
    player->play();
    emit changed();
}

void MotionPhotoPlayback::pause()
{
    Q_ASSERT(thread() == QThread::currentThread());
    // User transport intent already arrives on the owning GUI thread, while
    // every QMediaPlayer callback above is queued before it can re-enter this
    // controller. Pause synchronously so a short Motion Photo cannot reach an
    // already-queued EndOfMedia transition before the requested PausedState.
    // This also makes the internal pause contract match what the visible
    // transport control reports to the user when pause() returns.
    if (session_ && session_->admitted) {
        {
            std::lock_guard<std::mutex> lock(session_->previewSeek->mutex);
            ++session_->previewSeek->request;
            session_->previewSeek->active = false;
            session_->previewSeek->completing = false;
        }
        session_->pauseRequested = true;
        session_->player->pause();
    }
}

void MotionPhotoPlayback::resume()
{
    Q_ASSERT(thread() == QThread::currentThread());
    const quint64 generation = generation_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != generation_ || !session_ || !session_->admitted) {
            return;
        }

        auto* player = session_->player.get();
        {
            std::lock_guard<std::mutex> lock(session_->previewSeek->mutex);
            ++session_->previewSeek->request;
            session_->previewSeek->active = false;
            session_->previewSeek->completing = false;
        }
        session_->pauseRequested = false;
        const qint64 mediaDuration = player->duration();
        const bool replayFromEnd = session_->previewedEnd ||
                                   player->mediaStatus() == QMediaPlayer::EndOfMedia ||
                                   (mediaDuration > 0 && player->position() >= mediaDuration);
        if (!replayFromEnd) {
            player->play();
            return;
        }

        // Qt 6.4's bounded-device FFmpeg path can keep presenting the stale
        // final frame after a rewind. Recreate the bounded player/session from
        // the already-validated asset; this is the same clean lifecycle used
        // for reopening and guarantees replay begins with a fresh decoder.
        const std::optional<PhotoAssetInfo> asset = session_->asset;
        if (asset) {
            play(*asset);
        }
    });
}

void MotionPhotoPlayback::seek(qint64 milliseconds)
{
    Q_ASSERT(thread() == QThread::currentThread());
    pendingSeek_ = std::max(qint64(0), milliseconds);
    if (seekQueued_) {
        return;
    }
    seekQueued_ = true;
    const quint64 generation = generation_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != generation_) {
            return;
        }
        seekQueued_ = false;
        if (!seekable()) {
            return;
        }

        auto* player = session_->player.get();
        const bool resumeAfterSeek = player->playbackState() == QMediaPlayer::PlayingState;
        const qint64 target = std::min(pendingSeek_, duration());

        // Qt 6.4's FFmpeg backend does not reliably publish a replacement
        // QVideoSink frame for setPosition() while paused. For a paused scrub,
        // run the decoder only until it reaches the requested frame, suppress
        // catch-up/keyframes from presentation, then synchronously return to
        // PausedState after publishing the target frame. Playing seeks retain
        // the normal pause/reposition/resume transition.
        if (!resumeAfterSeek) {
            session_->previewedEnd = duration() > 0 && target >= duration();
            const qint64 previewTarget =
                session_->previewedEnd ? std::max<qint64>(0, duration() - 100) : target;
            {
                std::lock_guard<std::mutex> lock(session_->previewSeek->mutex);
                ++session_->previewSeek->request;
                session_->previewSeek->targetUs = previewTarget * 1000;
                session_->previewSeek->active = true;
                session_->previewSeek->completing = false;
            }
            session_->pauseRequested = true;
            player->pause();
            player->setPosition(previewTarget);
            player->play();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(session_->previewSeek->mutex);
            ++session_->previewSeek->request;
            session_->previewSeek->active = false;
            session_->previewSeek->completing = false;
        }
        session_->previewedEnd = false;
        player->pause();
        player->setPosition(target);
        player->play();
    });
}

void MotionPhotoPlayback::stop()
{
    Q_ASSERT(thread() == QThread::currentThread());
    const quint64 generation = ++generation_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != generation_) {
            return;
        }
        clear();
        error_.clear();
        emit changed();
    });
}
} // namespace Licasa
