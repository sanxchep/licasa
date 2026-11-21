#pragma once
#include "output_state.h"
#include <memory>
#include <QVariantMap>
class QQuickWindow;
#ifdef LICASA_HDR_RENDER_TEST
struct wl_display;
struct wl_surface;
#endif
namespace Licasa {
class ViewerPreferences;
// GUI thread, before showing/creating the native window. A private Qt request
// is only available in the opt-in Qt6.11 lab; success is not native confirmation.
bool requestHdrOutput(QQuickWindow* window, ViewerPreferences* preferences = nullptr);
QVariantMap hdrMonitorSettings(const QQuickWindow*);
bool setHdrMonitorSettings(QQuickWindow*, quint64 revision, float white, float peak, bool remember);
bool useSdrForMonitor(QQuickWindow*, quint64 revision);
bool forgetHdrMonitorSettings(QQuickWindow*, quint64 revision);
// GUI-thread platform/output notification and explicit calibration. The caller
// must use the current revision; a monitor/output change rejects stale replies.
quint64 notifyHdrOutputChanged(QQuickWindow*);
bool recalibrateHdrOutput(QQuickWindow*, quint64 revision, float paperWhiteNits, float peakNits);
// Shared immutable-handle state; snapshots are safe on GUI and render threads.
std::shared_ptr<HdrOutputState> hdrOutputState(const QQuickWindow*);
quint64 hdrOutputRevision(const QQuickWindow*);
// Render-safe: compositor acknowledgment and current-surface feedback gate.
bool hdrWaylandPermitsHdr(const QQuickWindow*);
bool hdrWaylandSurfaceTagged(const QQuickWindow*);
bool hdrWaylandAwaitsSurfaceTag(const QQuickWindow*);
#ifdef LICASA_HDR_RENDER_TEST
class WaylandHdrObserver;
// Use an isolated protocol client and simulate initial native swapchain support
// without mapping a real window on the test compositor.
WaylandHdrObserver* attachWaylandOutputForTest(QQuickWindow*, wl_display*, wl_surface*);
bool calibrateWaylandOutputForTest(QQuickWindow*, float white, float peak);
#endif
}
