#pragma once
#include "viewer_resource_budget.h"
#include <QSize>
#include <cmath>
#include <optional>

class QQuickWindow;
namespace Licasa {
struct HdrTargetPlan {
    QSize pixelLimit;
    quint64 bytes = 0;
};
// Admission estimate: 64-pixel allocation padding; FP16 color plus up to
// 8-byte depth/stencil. Bound dimensions before multiplication/conversion.
inline std::optional<quint64> fp16TargetBytes(QSize size)
{
    if (size.width() <= 0 || size.height() <= 0 || size.width() > 32768 || size.height() > 32768) return {};
    const quint64 w = (quint64(size.width()) + 63) / 64 * 64;
    const quint64 h = (quint64(size.height()) + 63) / 64 * 64;
    return w * h * 16;
}
inline std::optional<HdrTargetPlan> hdrTargetPlan(QSize logicalLimit, double maxDpr)
{
    if (!std::isfinite(maxDpr) || maxDpr <= 0 || logicalLimit.width() <= 0 || logicalLimit.height() <= 0) return {};
    const double w = std::ceil(logicalLimit.width() * maxDpr);
    const double h = std::ceil(logicalLimit.height() * maxDpr);
    if (w > 32768 || h > 32768) return {};
    const QSize pixels{int(w), int(h)};
    const auto target = fp16TargetBytes(pixels);
    if (!target) return {};
    // Five full-window SDR layers (base, viewport, controls, shortcuts, popup),
    // eight swapchain color slots, one depth/stencil, plus a 640x384 capture.
    // Three generations cover current + the pinned two-frame QRhi retirement
    // overlap. This is deliberately conservative, not measured driver memory.
    const quint64 areaBytes = *target / 16;
    return HdrTargetPlan{pixels, 3 * (areaBytes * (5 * 16 + 8 * 8 + 8)
                                        + *fp16TargetBytes(QSize(640,384)))};
}
// GUI thread, pre-native creation. Caps the opt-in window's logical extent.
// Keeps its complete envelope charged until QQuickWindow/native teardown.
bool reserveHdrTargets(QQuickWindow*, QSize logicalLimit, double maxDpr,
                       std::shared_ptr<GainmapLab::ResourceBudget> pool = viewerResourceBudget());
bool prepareHdrTargets(QQuickWindow*, QSize logicalLimit, double maxDpr,
                       std::shared_ptr<GainmapLab::ResourceBudget> pool = viewerResourceBudget());
bool hdrTargetsCover(const QQuickWindow*, QSize physicalSize);
// GUI-thread capture gate. Bounded windows need a completed frame matching the
// current geometry; windows outside this lab policy keep their existing path.
bool hdrTargetReadbackReady(const QQuickWindow*);
// GUI-only, app-owned lab windows. Requests coalesce; QML/application state stays
// in the same object while native graphics are recreated. A rejected destination
// leaves the current window unchanged; admission failure after teardown uses SDR.
bool requestHdrAdapterMigration(QQuickWindow*, const QByteArray& adapterUuid);
bool requestHdrDeviceRecovery(QQuickWindow*);
// GUI-only output renegotiation, sharing native teardown and target admission.
bool requestHdrSurfaceChange(QQuickWindow*, bool requestHdr);
bool hdrTargetRecovering(const QQuickWindow*);
// GUI-only lifecycle gate: a temporarily hidden, previously visible window is
// still user work until restoration/cancellation/failure is settled.
bool hdrTargetKeepsWindowAlive(const QQuickWindow*);
}
