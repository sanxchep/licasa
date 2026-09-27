#include "imaging/image_animation.h"
#include "imaging/async_image_provider.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"

#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace Licasa {
struct AnimationFrames {
    struct Entry {
        quint64 revision;
        QImage image;
    };
    QMutex mutex;
    QHash<quint64, Entry> images;
};

namespace {
class FrameProvider final : public QQuickImageProvider {
  public:
    explicit FrameProvider(std::shared_ptr<AnimationFrames> frames)
        : QQuickImageProvider(QQuickImageProvider::Image), frames_(std::move(frames))
    {}
    QImage requestImage(const QString& id, QSize* size, const QSize&) override
    {
        const auto parts = id.split('/');
        if (parts.size() != 2) {
            return {};
        }
        bool validId = false, validRevision = false;
        const auto key = parts[0].toULongLong(&validId);
        const auto revision = parts[1].toULongLong(&validRevision);
        if (!validId || !validRevision) {
            return {};
        }
        QMutexLocker lock(&frames_->mutex);
        const auto found = frames_->images.constFind(key);
        if (found == frames_->images.cend() || found->revision != revision) {
            return {};
        }
        if (size) {
            *size = found->image.size();
        }
        return found->image; // implicit sharing; no decode/copy on the UI thread
    }

  private:
    std::shared_ptr<AnimationFrames> frames_;
};
} // namespace

struct AnimationFrameResult {
    QImage image;
    QString error;
    int number = -1;
    int count = 0;
    int repeats = 0;
    int delay = 100;
    qint64 decodeMs = 0;
    bool end = false;
};

struct AnimationWork {
    explicit AnimationWork(QString path, int frame) : path(std::move(path)), nextFrame(frame) {}
    QString path;
    std::atomic_bool cancelled{false};
    quint64 pixels = 0;
    int nextFrame = 0;
    std::unique_ptr<QFile> file;
    std::unique_ptr<QImageReader> reader;

    AnimationFrameResult decode(ImageResourcePolicy& policy)
    {
        AnimationFrameResult result;
        if (cancelled.load()) {
            return result;
        }
        QMutexLocker processing(&policy.processingMutex());
        if (cancelled.load()) {
            return result;
        }
        const quint64 admitted = quint64(policy.prepareForProcessing()) * 1000000;
        QElapsedTimer elapsed;
        elapsed.start();
        if (!file) {
            if (!QFileInfo(path).isFile()) {
                result.error = QStringLiteral("The animated image is no longer available.");
                return result;
            }
            file = std::make_unique<QFile>(path);
        } else {
            // Pool threads can expire between slow/paused frames. A QFile has
            // no event processing here; detach it between jobs and adopt it on
            // the one worker that owns the next complete read operation.
            file->moveToThread(QThread::currentThread());
        }
        struct Detach {
            QFile* file;
            ~Detach() { file->moveToThread(nullptr); }
        } detach{file.get()};
        if (!file->isOpen() && !file->open(QIODevice::ReadOnly)) {
            result.error = file->errorString();
            return result;
        }
        if (!reader || pixels != admitted) {
            reader.reset();
            pixels = admitted;
            if (!file->seek(0)) {
                result.error = QStringLiteral("The animated image could not be read.");
                return result;
            }
            reader = std::make_unique<QImageReader>(file.get());
            ImageDecodeContract::configure(*reader, pixels, &cancelled);
            reader->setAutoTransform(true);
            if (!reader->canRead() || !reader->supportsAnimation()) {
                result.error = QStringLiteral("This file does not contain a readable animation.");
                return result;
            }
            if (!ImageDecodeContract::allows(reader->size(), pixels)) {
                result.error = QStringLiteral("The animation exceeds the selected image limit.");
                return result;
            }
            if (nextFrame > 0 && !reader->jumpToImage(nextFrame)) {
                for (int index = 0; index < nextFrame && !cancelled.load(); ++index) {
                    if (!ImageDecodeContract::allows(reader->size(), pixels) ||
                        !ImageDecodeContract::allows(reader->read().size(), pixels)) {
                        result.error = QStringLiteral("That animation frame is unavailable.");
                        return result;
                    }
                }
            }
        }
        if (cancelled.load()) {
            return result;
        }
        if (!reader->canRead() && nextFrame > 0) {
            result.end = true;
            return result;
        }
        // Check again for handlers whose current frame size differs from the
        // canvas. Allocation policy remains synchronized for the whole read.
        if (!ImageDecodeContract::allows(reader->size(), pixels)) {
            result.error = QStringLiteral("The animation exceeds the selected image limit.");
            return result;
        }
        result.image = reader->read();
        if (cancelled.load()) {
            result.image = {};
            return result;
        }
        if (result.image.isNull()) {
            result.error = file->property(ImageDecodeContract::errorProperty).toString();
            // Some streaming animation APIs (notably libheif sequences) only
            // reveal the frame count when end-of-sequence is reached. Require
            // that concrete count before treating a null read as normal EOS;
            // this must not hide a malformed-frame error from other handlers.
            if (result.error.isEmpty() && nextFrame > 0) {
                const int count = reader->imageCount();
                if (count > 0 && nextFrame >= count) {
                    result.end = true;
                    result.count = count;
                    return result;
                }
            }
            if (result.error.isEmpty()) {
                result.error = reader->errorString();
            }
            return result;
        }
        if (!ImageDecodeContract::allows(result.image.size(), pixels)) {
            result.image = {};
            result.error = QStringLiteral("The animation exceeds the selected image limit.");
            return result;
        }
        if (result.image.colorSpace().isValid() && result.image.colorSpace() != QColorSpace::SRgb) {
            result.image.convertToColorSpace(QColorSpace::SRgb);
        }
        if (nextFrame == std::numeric_limits<int>::max()) {
            result.image = {};
            result.error = QStringLiteral("The animation contains too many frames.");
            return result;
        }
        result.number = nextFrame++;
        result.count = reader->imageCount();
        result.repeats = reader->loopCount();
        result.delay = std::clamp(reader->nextImageDelay(), 10, 60000);
        result.decodeMs = elapsed.elapsed();
        return result;
    }
};

ImageAnimationService::ImageAnimationService(ImageResourcePolicy& policy,
                                             AsyncImageProvider& provider, QObject* parent)
    : QObject(parent), policy_(policy), provider_(provider),
      frames_(std::make_shared<AnimationFrames>())
{}

QObject* ImageAnimationService::createController(QObject* parent)
{
    if (!parent || parent->thread() != thread()) {
        return nullptr;
    }
    auto* controller = new ImageAnimation(*this, ++nextId_, parent);
    QQmlEngine::setObjectOwnership(controller, QQmlEngine::CppOwnership);
    return controller;
}

QQuickImageProvider* ImageAnimationService::createFrameProvider()
{
    return new FrameProvider(frames_);
}

ImageAnimation::ImageAnimation(ImageAnimationService& service, quint64 id, QObject* parent)
    : QObject(parent), service_(service), id_(id)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &ImageAnimation::requestFrame);
}

ImageAnimation::~ImageAnimation() { reset(); }

void ImageAnimation::reset()
{
    timer_.stop();
    retireWork();
    busy_ = false;
    frameSource_ = QUrl();
    currentFrame_ = -1;
    frameCount_ = 0;
    {
        QMutexLocker lock(&service_.frames_->mutex);
        service_.frames_->images.remove(id_);
    }
    emit frameChanged();
}

void ImageAnimation::setSource(const QUrl& source)
{
    if (source_ == source) {
        return;
    }
    reset();
    source_ = source;
    if (!playing_) {
        playing_ = true;
        emit playingChanged();
    }
    error_.clear();
    emit sourceChanged();
    emit errorChanged();
    start();
}

void ImageAnimation::setActive(bool active)
{
    if (active_ == active) {
        return;
    }
    active_ = active;
    if (active) {
        start();
    } else {
        reset();
    }
    emit activeChanged();
}

void ImageAnimation::setPlaying(bool playing)
{
    if (playing_ == playing) {
        return;
    }
    playing_ = playing;
    if (!playing) {
        timer_.stop();
    } else if (work_ && !busy_) {
        timer_.start(std::max(1, int(delayMs_ / speed_)));
    } else if (!work_ && active_) {
        currentFrame_ = -1;
        start();
    }
    emit playingChanged();
}

void ImageAnimation::setSpeed(qreal speed)
{
    if (!std::isfinite(speed)) {
        return;
    }
    speed = std::clamp(speed, 0.25, 2.0);
    if (speed_ == speed) {
        return;
    }
    speed_ = speed;
    if (timer_.isActive()) {
        timer_.start(std::max(1, int(delayMs_ / speed_)));
    }
    emit speedChanged();
}

void ImageAnimation::start(int frame)
{
    if (!active_ || source_.isEmpty() || work_) {
        return;
    }
    if (!source_.isValid() || !source_.isLocalFile()) {
        error_ = QStringLiteral("Only local animated images can be opened.");
        emit errorChanged();
        return;
    }
    work_ = std::make_shared<AnimationWork>(source_.toLocalFile(), frame);
    requestFrame();
}

void ImageAnimation::seekFrame(int number)
{
    if (number < 0 || number > 100000 || (frameCount_ > 0 && number >= frameCount_)) {
        return;
    }
    reset();
    start(number);
}

void ImageAnimation::requestFrame()
{
    if (!active_ || !work_ || busy_) {
        return;
    }
    busy_ = true;
    auto work = work_;
    auto* service = &service_;
    const QPointer<ImageAnimation> guard(this);
    service_.provider_.enqueueWork([work, service, guard] {
        auto result = work->decode(service->policy_);
        if (work->cancelled.load()) {
            return;
        }
        // Service outlives the decode pool. Inspect the controller's weak
        // pointer only in this GUI-thread delivery, never on the worker.
        QMetaObject::invokeMethod(
            service,
            [guard, work, result = std::move(result)]() mutable {
                if (guard) {
                    guard->finishFrame(work, std::move(result));
                }
            },
            Qt::QueuedConnection);
    });
}

void ImageAnimation::retireWork()
{
    if (!work_) {
        return;
    }
    work_->cancelled.store(true);
    // A queued GUI delivery can still share this work. Release the heavy
    // resources explicitly on the serial decode worker, rather than relying
    // on whichever thread eventually drops the last shared pointer.
    service_.provider_.enqueueWork([old = std::move(work_)] {
        old->reader.reset();
        old->file.reset();
    });
}

void ImageAnimation::finishFrame(const std::shared_ptr<AnimationWork>& work,
                                 AnimationFrameResult result)
{
    if (work_ != work || work->cancelled.load()) {
        return;
    }
    busy_ = false;
    if (!result.error.isEmpty()) {
        error_ = result.error;
        retireWork();
        emit errorChanged();
        return;
    }
    if (result.end) {
        finishSequence(result.count);
        return;
    }
    if (!result.image.isNull()) {
        publishFrame(std::move(result));
    }
}

void ImageAnimation::finishSequence(int frameCount)
{
    if (frameCount > 0 && frameCount_ != frameCount) {
        frameCount_ = frameCount;
        emit frameChanged();
    }
    const bool repeat = repeatsLeft_ == -1 || repeatsLeft_ > 0;
    if (repeat) {
        if (repeatsLeft_ > 0) {
            --repeatsLeft_;
        }
        retireWork();
        start();
    } else {
        setPlaying(false);
        retireWork();
    }
}

void ImageAnimation::publishFrame(AnimationFrameResult result)
{
    if (currentFrame_ < 0) {
        repeatsLeft_ = result.repeats;
    }
    currentFrame_ = result.number;
    frameCount_ = result.count;
    delayMs_ = result.delay;
    ++revision_;
    {
        QMutexLocker lock(&service_.frames_->mutex);
        service_.frames_->images.insert(id_, {revision_, std::move(result.image)});
    }
    frameSource_ = QUrl(QStringLiteral("image://animation/%1/%2").arg(id_).arg(revision_));
    emit frameChanged();
    if (playing_) {
        timer_.start(int(std::max<qint64>(1, qint64(delayMs_ / speed_) - result.decodeMs)));
    }
}

} // namespace Licasa
