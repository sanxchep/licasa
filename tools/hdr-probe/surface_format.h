#pragma once

#include <vulkan/vulkan_core.h>

#include <optional>

class QRhiSwapChain;

enum class SurfaceFormat { Unsupported, PassThrough, LinearFp16 };

constexpr SurfaceFormat classifySurfaceFormat(VkSurfaceFormatKHR format)
{
    if (format.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        return SurfaceFormat::Unsupported;
    }
    switch (format.colorSpace) {
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        return SurfaceFormat::LinearFp16;
    case VK_COLOR_SPACE_PASS_THROUGH_EXT:
        return SurfaceFormat::PassThrough;
    default:
        return SurfaceFormat::Unsupported;
    }
}

std::optional<VkSurfaceFormatKHR> nativeSwapchainFormat(const QRhiSwapChain* swapchain);

int nativeSwapchainBufferCount(const QRhiSwapChain* swapchain);
bool waylandTaggedPassthroughEnabled();
bool supportsTaggedWaylandHdrSwapchain(const QRhiSwapChain* swapchain, bool allowPassthrough);
