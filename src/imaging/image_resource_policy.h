#pragma once

#include "media/motion/motion_photo_playback_policy.h"
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QTimer>
#include <atomic>

namespace Licasa {

class ViewerPreferences;

class ImageResourcePolicy final : public QObject, public MotionPhotoResourcePolicy {
    Q_OBJECT

  public:
    // Call before constructing QGuiApplication or starting any threads. Qt
    // caches QT_IMAGEIO_MAXALLOC on first use, even when setAllocationLimit is
    // called later. The application policy must own the decoder cap.
    static void initializeDecoderEnvironment();
    explicit ImageResourcePolicy(ViewerPreferences& preferences, QObject* parent = nullptr);

    int maximumImageMegapixels() const noexcept;
    quint64 maximumImagePixels() const noexcept;
    int decoderAllocationLimitMiB() const noexcept;
    bool allows(const QSize& size) const noexcept;
    QMutex& processingMutex() noexcept override;
    // Caller must hold processingMutex for the complete operation. Returns the
    // admitted policy snapshot; an in-flight operation keeps that snapshot.
    int prepareForProcessing() override;

  private:
    void applyMaximumImageMegapixels(int value);
    void tryUpdateDecoderAllocationLimit();

    std::atomic_int maximumImageMegapixels_;
    QMutex processingMutex_;
    QTimer allocationUpdateTimer_;
};

} // namespace Licasa
