#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>
#include <memory>
struct wl_display;
struct wl_surface;
class QQuickWindow;
namespace Licasa {
struct WaylandHdrReport {
    enum class State { Pending, Unavailable, Sdr, Ready };
    State state = State::Pending;
    QString reason = QStringLiteral("registry_pending");
    quint64 revision = 1;
    quint32 preferredIdentity = 0;
    double referenceNits = 0, targetNits = 0;
    bool protocol = false, descriptionAccepted = false, preferredChanged = false;
    QByteArray preferredFingerprint; // Canonical information, not a persistent monitor ID.
    bool surfacePresent = false;
    quint64 surfaceEpoch = 0;
};
// GUI-owned feedback and optional content tag for a confirmed FP16
// pass-through swapchain. The caller must exclude other tag owners.
// The borrowed connection is read by Qt (or a test transport); this object only
// dispatches its own pending event queue. No roundtrip/blocking socket read.
class WaylandHdrObserver final : public QObject {
public:
    WaylandHdrObserver(wl_display*, std::function<void()> changed, QObject* parent = nullptr);
    ~WaylandHdrObserver() override;
    void setSurface(wl_surface*);
    void watchWindow(QQuickWindow*);
    WaylandHdrReport report() const;
    bool tagLinearSurface();
    bool linearSurfaceTagged() const;
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void attachNativeSurface();
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
// A null result means another QPA; it does not claim a Wayland capability.
std::unique_ptr<WaylandHdrObserver> observeWaylandHdr(QQuickWindow*, std::function<void()> changed);
}
