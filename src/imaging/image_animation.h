#pragma once

#include <QObject>
#include <QTimer>
#include <QUrl>
#include <memory>

class QQuickImageProvider;

namespace Licasa {
class ImageResourcePolicy;
class AsyncImageProvider;
class ImageAnimation;
struct AnimationFrames;
struct AnimationWork;
struct AnimationFrameResult;

class ImageAnimationService final : public QObject {
    Q_OBJECT
  public:
    // This service must outlive the engine-owned decode provider, which drains
    // its worker at shutdown. No decoder or animation thread starts here.
    ImageAnimationService(ImageResourcePolicy& policy, AsyncImageProvider& provider,
                          QObject* parent = nullptr);
    Q_INVOKABLE QObject* createController(QObject* parent);
    QQuickImageProvider* createFrameProvider();

  private:
    friend class ImageAnimation;
    ImageResourcePolicy& policy_;
    AsyncImageProvider& provider_;
    std::shared_ptr<AnimationFrames> frames_;
    quint64 nextId_ = 0;
};

class ImageAnimation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QUrl frameSource READ frameSource NOTIFY frameChanged)
    Q_PROPERTY(int currentFrame READ currentFrame NOTIFY frameChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
  public:
    ~ImageAnimation() override;
    QUrl source() const { return source_; }
    bool active() const { return active_; }
    bool playing() const { return playing_; }
    qreal speed() const { return speed_; }
    QUrl frameSource() const { return frameSource_; }
    int currentFrame() const { return currentFrame_; }
    int frameCount() const { return frameCount_; }
    QString error() const { return error_; }
    void setSource(const QUrl& source);
    void setActive(bool active);
    void setPlaying(bool playing);
    void setSpeed(qreal speed);
    Q_INVOKABLE void seekFrame(int number);
  signals:
    void sourceChanged();
    void activeChanged();
    void playingChanged();
    void speedChanged();
    void frameChanged();
    void errorChanged();

  private:
    friend class ImageAnimationService;
    ImageAnimation(ImageAnimationService& service, quint64 id, QObject* parent);
    void reset();
    void start(int frame = 0);
    void requestFrame();
    void retireWork();
    void finishFrame(const std::shared_ptr<AnimationWork>& work, AnimationFrameResult result);
    void finishSequence(int frameCount);
    void publishFrame(AnimationFrameResult result);
    ImageAnimationService& service_;
    const quint64 id_;
    std::shared_ptr<AnimationWork> work_;
    QTimer timer_;
    QUrl source_;
    QUrl frameSource_;
    QString error_;
    quint64 revision_ = 0;
    int currentFrame_ = -1;
    int frameCount_ = 0;
    int repeatsLeft_ = 0;
    int delayMs_ = 100;
    bool active_ = false;
    bool playing_ = true;
    bool busy_ = false;
    qreal speed_ = 1;
};
} // namespace Licasa
