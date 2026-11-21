#include "surface_format.h"

#include <QVulkanInstance>
#include <private/qrhivulkan_p.h>
#include <rhi/qrhi_platform.h>
#include <vector>

std::optional<VkSurfaceFormatKHR> nativeSwapchainFormat(const QRhiSwapChain* swapchain)
{
    if (!swapchain || swapchain->rhi()->backend() != QRhi::Vulkan) {
        return std::nullopt;
    }

    // Use the native pair supplied to vkCreateSwapchainKHR. The QRhi format
    // describes the request and can disagree with the Vulkan color space.
    const auto* vulkanSwapchain = static_cast<const QVkSwapChain*>(swapchain);
    if (vulkanSwapchain->sc == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    return VkSurfaceFormatKHR{vulkanSwapchain->colorFormat, vulkanSwapchain->colorSpace};
}

int nativeSwapchainBufferCount(const QRhiSwapChain* swapchain)
{
    if (!swapchain || swapchain->rhi()->backend() != QRhi::Vulkan) return 0;
    const auto* native = static_cast<const QVkSwapChain*>(swapchain);
    return native->sc != VK_NULL_HANDLE ? native->bufferCount : 0;
}

bool waylandTaggedPassthroughEnabled()
{
    // This ownership contract relies on QRhi's Vulkan format selection, so
    // each Qt release is admitted by name after its selection is reviewed and
    // the tagged Wayland journey passes on it. Qt 6.11.3 changes neither the
    // surface format nor the color space choice of 6.11.2.
    const bool reviewedQt = QT_VERSION == QT_VERSION_CHECK(6, 11, 2)
        || QT_VERSION == QT_VERSION_CHECK(6, 11, 3);
    return reviewedQt
        && qEnvironmentVariable("LICASA_HDR_WAYLAND_TAGGED_PASSTHROUGH") == QStringLiteral("1");
}

bool supportsTaggedWaylandHdrSwapchain(const QRhiSwapChain* swapchain, bool allowPassthrough)
{
    if (!swapchain || swapchain->rhi()->backend() != QRhi::Vulkan) return false;
    const auto* chain = static_cast<const QVkSwapChain*>(swapchain);
    const auto* handles = static_cast<const QRhiVulkanNativeHandles*>(swapchain->rhi()->nativeHandles());
    if (!chain->surface || !handles || !handles->inst || !handles->physDev) return false;
    const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        handles->inst->getInstanceProcAddr("vkGetPhysicalDeviceSurfaceFormatsKHR"));
    uint32_t count = 0;
    if (!query || query(handles->physDev, chain->surface, &count, nullptr) != VK_SUCCESS
        || !count || count > 4096) return false;
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (query(handles->physDev, chain->surface, &count, formats.data()) != VK_SUCCESS) return false;

    bool linear = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto format = classifySurfaceFormat(formats[i]);
        // Qt 6.11 replaces the selected color space with pass-through when
        // the same pixel format offers it. An untagged surface is ineligible.
        if (format == SurfaceFormat::PassThrough && !allowPassthrough) return false;
        linear |= format == SurfaceFormat::LinearFp16;
    }
    return linear;
}
