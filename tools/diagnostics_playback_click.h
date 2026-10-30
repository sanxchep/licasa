#pragma once

#include <QElapsedTimer>
#include <QObject>

namespace LicasaDiagnostics {

// QML's click arbitration and the media backend both complete asynchronously.
// A matching playback state alone can also mean that the clip reached EOF.
class PlaybackClickCheck final : public QObject {
    Q_OBJECT

  public:
    enum class Outcome { Pending, Applied, Failed };

    PlaybackClickCheck(QObject* viewport, bool expectedPlaying, int timeoutMs = 3000)
        : expectedPlaying_(expectedPlaying), timeoutMs_(timeoutMs)
    {
        connected_ = viewport && connect(viewport, SIGNAL(playbackToggleRequested()), this,
                                         SLOT(toggleRequested()));
        elapsed_.start();
    }

    Outcome check(bool active, bool playing) const
    {
        if (!connected_ || requests_ > 1 || elapsed_.elapsed() >= timeoutMs_) {
            return Outcome::Failed;
        }
        return requests_ == 1 && active && playing == expectedPlaying_ ? Outcome::Applied
                                                                       : Outcome::Pending;
    }

    int requests() const { return requests_; }

  private slots:
    void toggleRequested() { ++requests_; }

  private:
    QElapsedTimer elapsed_;
    bool expectedPlaying_;
    int timeoutMs_;
    int requests_ = 0;
    bool connected_ = false;
};

} // namespace LicasaDiagnostics
