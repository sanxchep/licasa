#pragma once

#include <QJsonObject>
#include <QtGlobal>

#include <functional>
#include <memory>

class QMutex;
class QString;
class QQuickWindow;
class QUrl;
namespace Licasa {
class AsyncImageProvider;
class WindowManager;
}

namespace LicasaDiagnostics {

class DiagnosticCheck {
  public:
    virtual ~DiagnosticCheck() = default;
    virtual bool attach(QQuickWindow* window) = 0;
};

qint64 monotonicNs();
QJsonObject processMetrics();
// Open descriptors by kind, with `photo` for descriptors on that path, and a total.
QJsonObject descriptorKinds(const QString& photo = QString());
void printJson(const QJsonObject& value);
QString fileSha256(const QString& path);

std::unique_ptr<DiagnosticCheck> makeRawBehaviorCheck(bool previewOnly, bool editOnly);
std::unique_ptr<DiagnosticCheck>
makeMotionPhotoControlsCheck(const QString& frameOutput = QString());
std::unique_ptr<DiagnosticCheck> makeViewerMeasurement(qint64 started, const QUrl& imageUrl,
                                                       int cycles);
std::unique_ptr<DiagnosticCheck> makeHdrNavigationCheck(const QUrl& fixture, quint32 seed);
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
std::unique_ptr<DiagnosticCheck>
makeWaylandHdrCheck(const QUrl& imageUrl, const QString& expectedOutput, bool windowChurn = false,
                   const QString& windowSeed = {});
// Seeded output changes, hides and reopens in two windows; resources measured every 50 actions.
std::unique_ptr<DiagnosticCheck> makeMultiwindowHdrCheck(const QUrl&, Licasa::WindowManager*, quint32 seed,
                                                         int actions = 100);
// Moves, recalibrations and hotplugs between two outputs in cycles of 12 actions,
// measured at the end of every cycle; the default 8 cycles and 4 final actions make 100.
std::unique_ptr<DiagnosticCheck> makeOutputMigrationCheck(const QUrl&, Licasa::WindowManager*, quint32 seed,
                                                          int cycles = 8);
// Opens a scratch copy that the check replaces; *opened is the path to view.
std::unique_ptr<DiagnosticCheck> makeReplacementHdrCheck(const QUrl& gray, Licasa::WindowManager*,
                                                         quint32 seed, QUrl* opened);

// Settled native frame receipts with an FP16/8-bit centre readback for one
// window, as the multiwindow checks use them. A white of 0 samples SDR output.
class HdrFrameSampler {
  public:
    // SdrBaseOnHdr: the photo's SDR base on HDR output, its HDR rendition refused.
    enum class Content { HdrStill, MotionFrame, SdrBaseOnHdr, PendingSdrBaseOnHdr };
    virtual ~HdrFrameSampler() = default;
    virtual bool attach(QQuickWindow* window) = 0;
    // sdrCode names the output state an SDR sample expects, such as "disabled" or "memory".
    virtual void sample(const QString& phase, float white, float peak, Content content,
                        const QString& sdrCode = QStringLiteral("disabled")) = 0;
};
std::unique_ptr<HdrFrameSampler> makeHdrFrameSampler(const QUrl& image,
                                                     std::function<void(QJsonObject)> frame,
                                                     bool matchPresented = false);
// HDR cover, playback and viewer actions on one Motion Photo; outputs go to a new directory.
// With rounds > 0, the playback lifecycle is repeated and measured after each round.
std::unique_ptr<DiagnosticCheck> makeMotionHdrJourneyCheck(const QUrl& image, const QUrl& peer,
                                                           Licasa::WindowManager*, QMutex* processingGate,
                                                           quint32 seed, const QString& outputDirectory,
                                                           int rounds = 0);
// Two camera-sized gain-map photos in one HDR window; outputs go to a new directory.
std::unique_ptr<DiagnosticCheck> makeCameraNavigationCheck(const QUrl& first, const QUrl& second,
                                                           Licasa::WindowManager*, quint32 seed,
                                                           const QString& outputDirectory);
#endif
std::unique_ptr<DiagnosticCheck> makeNavigationTransitionCheck(const QUrl& nextImageUrl,
                                                               bool duringFull = false);
std::unique_ptr<DiagnosticCheck> makeNavigationSequenceCheck(Licasa::AsyncImageProvider* provider,
                                                             int presses, int intervalMs,
                                                             int warmupMs, int idleMs,
                                                             int direction, bool releaseLastKey);

} // namespace LicasaDiagnostics
