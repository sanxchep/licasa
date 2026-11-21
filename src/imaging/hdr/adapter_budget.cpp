#include "adapter_budget.h"
#include <QQuickWindow>
#include <QQuickGraphicsDevice>
#include <QQuickGraphicsConfiguration>
#include <QVulkanFunctions>
#include <private/qvulkandefaultinstance_p.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <limits>
#include <algorithm>
#include <vector>
#include <QVariant>

namespace Licasa {
std::optional<HdrAdapter> describeHdrAdapter(QVulkanInstance* instance, VkPhysicalDevice device)
{
    if (!instance || !instance->isValid() || instance->apiVersion() < QVersionNumber(1,1) || !device) return {};
    auto* functions = instance->functions();
    VkPhysicalDeviceProperties basic{};
    functions->vkGetPhysicalDeviceProperties(device, &basic);
    if (basic.apiVersion < VK_API_VERSION_1_1) return {};
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    functions->vkGetPhysicalDeviceProperties2(device, &properties);
    const QByteArray key(reinterpret_cast<const char*>(id.deviceUUID), VK_UUID_SIZE);
    if (std::all_of(key.begin(), key.end(), [](char byte) { return byte == 0; })) return {};
    VkPhysicalDeviceMemoryProperties memory{};
    functions->vkGetPhysicalDeviceMemoryProperties(device, &memory);
    quint64 heap = std::numeric_limits<quint64>::max();
    for (uint32_t i = 0; i < qMin(memory.memoryHeapCount, uint32_t(VK_MAX_MEMORY_HEAPS)); ++i)
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && memory.memoryHeaps[i].size)
            heap = qMin(heap, quint64(memory.memoryHeaps[i].size));
    if (heap == std::numeric_limits<quint64>::max()) return {};
    const quint64 limit = hdrAdapterLimit(heap);
    if (!limit) return {};
    return HdrAdapter{key, QString::fromUtf8(properties.properties.deviceName), heap, limit};
}
std::optional<HdrAdapter> describeHdrAdapter(QRhi* rhi)
{
    if (!rhi || rhi->backend() != QRhi::Vulkan) return {};
    const auto* native = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles());
    return native ? describeHdrAdapter(native->inst, native->physDev) : std::nullopt;
}
std::optional<HdrAdapterSelection> selectHdrAdapter(QQuickWindow* window)
{
    if (!window || !window->graphicsDevice().isNull()) return {};
    HdrAdapterSelection selection;
    selection.instance = window->vulkanInstance();
    // Use the same instance as Qt's ordinary/gesture windows. Creating a
    // separate instance would make shared-QRhi surface handles incompatible.
    if (!selection.instance) selection.instance = QVulkanDefaultInstance::instance();
    if (!selection.instance) return {};
    if (!selection.instance->isValid() || selection.instance->apiVersion() < QVersionNumber(1,1)) return {};
    auto* functions = selection.instance->functions();
    uint32_t count = 0;
    if (functions->vkEnumeratePhysicalDevices(selection.instance->vkInstance(), &count, nullptr) != VK_SUCCESS
        || !count || count > 256) return {};
    std::vector<VkPhysicalDevice> devices(count);
    if (functions->vkEnumeratePhysicalDevices(selection.instance->vkInstance(), &count, devices.data()) != VK_SUCCESS || !count) return {};
    int index = -1;
    if (qEnvironmentVariableIsSet("QT_VK_PHYSICAL_DEVICE_INDEX")) {
        bool valid;
        index = qEnvironmentVariableIntValue("QT_VK_PHYSICAL_DEVICE_INDEX", &valid);
        if (!valid || index >= int(count)) return {};
    }
    if (qEnvironmentVariableIsSet("QSG_RHI_PREFER_SOFTWARE_RENDERER") && index < 0) return {};
    if (index < 0 && window->graphicsConfiguration().prefersSoftwareDevice()) {
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties properties{};
            functions->vkGetPhysicalDeviceProperties(devices[i], &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { index = int(i); break; }
        }
    }
    if (index < 0) index = 0; // Match the pinned Qt Vulkan default selection.
    selection.physicalDevice = devices[uint32_t(index)];
    const auto description = describeHdrAdapter(selection.instance, selection.physicalDevice);
    if (!description) return {};
    selection.adapter = *description;
    return selection;
}
std::optional<HdrAdapterSelection> findHdrAdapter(QQuickWindow* window, const QByteArray& uuid)
{
    if (!window || uuid.isEmpty()) return {};
    auto* instance = window->vulkanInstance();
    if (!instance || !instance->isValid()) return {};
    uint32_t count = 0;
    auto* f = instance->functions();
    if (f->vkEnumeratePhysicalDevices(instance->vkInstance(), &count, nullptr) != VK_SUCCESS || !count || count > 256) return {};
    std::vector<VkPhysicalDevice> devices(count);
    if (f->vkEnumeratePhysicalDevices(instance->vkInstance(), &count, devices.data()) != VK_SUCCESS) return {};
    for (uint32_t i = 0; i < count; ++i) {
        const auto adapter = describeHdrAdapter(instance, devices[i]);
        if (adapter && adapter->key == uuid) return HdrAdapterSelection{*adapter, instance, devices[i]};
    }
    return {};
}
void applyHdrAdapter(QQuickWindow* window, const HdrAdapterSelection& selection)
{
    window->setVulkanInstance(selection.instance);
    window->setGraphicsDevice(QQuickGraphicsDevice::fromPhysicalDevice(selection.physicalDevice));
    window->setProperty("licasaHdrAdapter", selection.adapter.key);
    window->setProperty("licasaHdrAdapterLimit", QVariant::fromValue(selection.adapter.limit));
}
}
