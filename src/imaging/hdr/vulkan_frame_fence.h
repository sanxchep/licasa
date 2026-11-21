#pragma once
#include "../../../tools/gainmap-lab/gpu_retirement.h"
#include <vulkan/vulkan_core.h>
class QRhi;
namespace Licasa {
inline GainmapLab::GpuFence::Status hdrFenceStatus(VkResult result)
{
    using Status = GainmapLab::GpuFence::Status;
    return result == VK_SUCCESS ? Status::Complete : result == VK_NOT_READY ? Status::Pending
        : result == VK_ERROR_DEVICE_LOST ? Status::DeviceLost : Status::Failed;
}
inline bool hdrDeviceQuiescent(VkResult result) { return result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST; }
// Submit only on the Vulkan render thread, outside an active QRhi frame. The
// fence orders all previous uses on the same queue; no resource may be used again.
std::shared_ptr<GainmapLab::GpuFence> submitFrameFence(QRhi* rhi);
bool waitForHdrDevice(QRhi* rhi);
}
