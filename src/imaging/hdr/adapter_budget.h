#pragma once
#include "viewer_resource_budget.h"
#include <QVulkanInstance>
#include <QString>
#include <optional>
class QRhi;
class QQuickWindow;
namespace Licasa {
struct HdrAdapter {
    GainmapLab::ResourceBudget::Adapter key;
    QString name;
    quint64 localHeapBytes = 0;
    quint64 limit = 0;
};
inline quint64 hdrAdapterLimit(quint64 smallestLocalHeap)
{ return qMin(smallestLocalHeap / 4, quint64(1024) * 1024 * 1024); }
std::optional<HdrAdapter> describeHdrAdapter(QVulkanInstance*, VkPhysicalDevice);
std::optional<HdrAdapter> describeHdrAdapter(QRhi*);
struct HdrAdapterSelection {
    HdrAdapter adapter;
    QVulkanInstance* instance = nullptr;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
};
// GUI-only preflight, without changing the window on rejection. Pin the queried
// physical device only after global/user/adapter admission succeeds.
std::optional<HdrAdapterSelection> selectHdrAdapter(QQuickWindow*);
// App-owned recovery only: look up a UUID without changing a live device.
std::optional<HdrAdapterSelection> findHdrAdapter(QQuickWindow*, const QByteArray& uuid);
void applyHdrAdapter(QQuickWindow*, const HdrAdapterSelection&);
}
