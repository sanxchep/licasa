#include "vulkan_frame_fence.h"
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <limits>
namespace Licasa {
namespace {
struct FrameFence final : GainmapLab::GpuFence {
    QVulkanDeviceFunctions* functions;
    VkDevice device;
    VkFence fence;
    FrameFence(QVulkanDeviceFunctions* f, VkDevice d, VkFence v) : functions(f), device(d), fence(v) {}
    ~FrameFence() override { functions->vkDestroyFence(device, fence, nullptr); }
    Status poll() noexcept override {
        const auto result = functions->vkGetFenceStatus(device, fence);
        return hdrFenceStatus(result);
    }
    bool wait() noexcept override {
        const auto result = functions->vkWaitForFences(device, 1, &fence, VK_TRUE,
                                                       std::numeric_limits<uint64_t>::max());
        if (hdrDeviceQuiescent(result)) return true;
        const auto idle = functions->vkDeviceWaitIdle(device);
        return hdrDeviceQuiescent(idle);
    }
};
}
bool waitForHdrDevice(QRhi* rhi)
{
    if (!rhi || rhi->backend() != QRhi::Vulkan) return false;
    if (rhi->isDeviceLost()) return true;
    const auto* native = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles());
    const auto result = native->inst->deviceFunctions(native->dev)->vkDeviceWaitIdle(native->dev);
    return hdrDeviceQuiescent(result);
}
std::shared_ptr<GainmapLab::GpuFence> submitFrameFence(QRhi* rhi)
{
    if (!rhi || rhi->backend() != QRhi::Vulkan || rhi->isDeviceLost()) return {};
    const auto* native = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles());
    auto* functions = native->inst->deviceFunctions(native->dev);
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    if (functions->vkCreateFence(native->dev, &info, nullptr, &fence) != VK_SUCCESS) return {};
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (functions->vkQueueSubmit(native->gfxQueue, 1, &submit, fence) != VK_SUCCESS) {
        const auto idle = functions->vkDeviceWaitIdle(native->dev);
        if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST)
            qFatal("HDR fence submission could not establish safe teardown");
        functions->vkDestroyFence(native->dev, fence, nullptr);
        return {};
    }
    return std::make_shared<FrameFence>(functions, native->dev, fence);
}
}
