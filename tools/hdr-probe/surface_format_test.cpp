#include "surface_format.h"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <cstdio>
#include <memory>

int main()
{
    struct Case {
        const char* name;
        VkSurfaceFormatKHR format;
        SurfaceFormat expected;
    };
    const Case cases[] = {
        {"extended linear sRGB",
         {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
         SurfaceFormat::LinearFp16},
        {"pass-through",
         {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_PASS_THROUGH_EXT},
         SurfaceFormat::PassThrough},
        {"FP16 sRGB",
         {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
         SurfaceFormat::Unsupported},
        {"FP16 linear P3",
         {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT},
         SurfaceFormat::Unsupported},
        {"8-bit extended linear sRGB",
         {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
         SurfaceFormat::Unsupported},
        {"HDR10 PQ",
         {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
         SurfaceFormat::Unsupported},
    };
    for (const auto& test : cases) {
        if (classifySurfaceFormat(test.format) != test.expected) {
            std::fprintf(stderr, "Unexpected surface classification for %s\n", test.name);
            return 1;
        }
    }

    QRhiNullInitParams params;
    const std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
    if (!rhi) {
        std::fputs("Could not initialize the null test backend\n", stderr);
        return 1;
    }
    const std::unique_ptr<QRhiSwapChain> request(rhi->newSwapChain());
    request->setFormat(QRhiSwapChain::HDRExtendedSrgbLinear);
    if (nativeSwapchainFormat(nullptr) || nativeSwapchainFormat(request.get())
        || nativeSwapchainBufferCount(nullptr) || nativeSwapchainBufferCount(request.get())) {
        std::fputs("A missing or non-Vulkan swapchain reported a native Vulkan format\n", stderr);
        return 1;
    }
    return 0;
}
