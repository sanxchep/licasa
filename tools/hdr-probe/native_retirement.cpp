#include "native_retirement.h"
#include "../gainmap-lab/gpu_retirement.h"
#include <QElapsedTimer>
#include <QThread>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <cstring>
#include <limits>

namespace {
using namespace Licasa::GainmapLab;
struct Fence final : GpuFence {
    QVulkanDeviceFunctions* f;
    VkDevice device;
    VkFence handle = VK_NULL_HANDLE;
    bool submitted = false;
    Fence(QVulkanDeviceFunctions* functions, VkDevice d) : f(functions), device(d) {}
    ~Fence() override { if (handle) f->vkDestroyFence(device, handle, nullptr); }
    Status poll() noexcept override
    {
        if (!submitted) return Status::Pending;
        const auto result = f->vkGetFenceStatus(device, handle);
        return result == VK_SUCCESS ? Status::Complete :
               result == VK_NOT_READY ? Status::Pending : Status::Failed;
    }
    bool wait() noexcept override
    {
        if (!submitted) return true; // Submission failed or never happened.
        const auto result = f->vkWaitForFences(device, 1, &handle, VK_TRUE,
                                              std::numeric_limits<uint64_t>::max());
        if (result == VK_SUCCESS) return true;
        // Legal device-loss teardown must be established at the device level.
        const auto idle = f->vkDeviceWaitIdle(device);
        return idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST;
    }
};
struct Copy {
    QVulkanDeviceFunctions* f;
    VkDevice device;
    VkBuffer buffers[2] = {};
    VkDeviceMemory memory[2] = {};
    void* mapped[2] = {};
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    Copy(QVulkanDeviceFunctions* functions, VkDevice d) : f(functions), device(d) {}
    ~Copy()
    {
        if (commands) f->vkDestroyCommandPool(device, commands, nullptr);
        if (image) f->vkDestroyImage(device, image, nullptr);
        if (imageMemory) f->vkFreeMemory(device, imageMemory, nullptr);
        for (int i = 0; i < 2; ++i) {
            if (mapped[i]) f->vkUnmapMemory(device, memory[i]);
            if (buffers[i]) f->vkDestroyBuffer(device, buffers[i], nullptr);
            if (memory[i]) f->vkFreeMemory(device, memory[i], nullptr);
        }
    }
};
}

bool checkNativeRetirement(QRhi* rhi, const QImage& source, std::shared_ptr<void>& persistent,
                           const std::shared_ptr<ResourceBudget>& pool, QJsonObject& details)
{
    if (rhi->backend() != QRhi::Vulkan) return false;
    const auto* native = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles());
    auto* f = native->inst->deviceFunctions(native->dev);
    const quint64 bytes = quint64(source.sizeInBytes());
    // Two host-visible transfer buffers and the native FP16 image, separately
    // charged from the persistent QRhi texture.
    auto allowance = pool->acquire({0, 0, bytes * 3});
    if (!allowance) return false;
    auto copy = std::make_unique<Copy>(f, native->dev);
    VkPhysicalDeviceMemoryProperties memoryProperties;
    native->inst->functions()->vkGetPhysicalDeviceMemoryProperties(native->physDev, &memoryProperties);
    for (int i = 0; i < 2; ++i) {
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = bytes;
        buffer.usage = i == 0 ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (f->vkCreateBuffer(native->dev, &buffer, nullptr, &copy->buffers[i]) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements;
        f->vkGetBufferMemoryRequirements(native->dev, copy->buffers[i], &requirements);
        uint32_t type = memoryProperties.memoryTypeCount;
        for (uint32_t j = 0; j < memoryProperties.memoryTypeCount; ++j) {
            const auto flags = memoryProperties.memoryTypes[j].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << j)) &&
                (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = j; break;
            }
        }
        if (type == memoryProperties.memoryTypeCount) return false;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (f->vkAllocateMemory(native->dev, &allocation, nullptr, &copy->memory[i]) != VK_SUCCESS ||
            f->vkBindBufferMemory(native->dev, copy->buffers[i], copy->memory[i], 0) != VK_SUCCESS ||
            f->vkMapMemory(native->dev, copy->memory[i], 0, bytes, 0, &copy->mapped[i]) != VK_SUCCESS)
            return false;
    }
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image.extent = {uint32_t(source.width()), uint32_t(source.height()), 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (f->vkCreateImage(native->dev, &image, nullptr, &copy->image) != VK_SUCCESS) return false;
    VkMemoryRequirements imageRequirements;
    f->vkGetImageMemoryRequirements(native->dev, copy->image, &imageRequirements);
    uint32_t imageType = memoryProperties.memoryTypeCount;
    for (uint32_t j = 0; j < memoryProperties.memoryTypeCount; ++j) {
        if (imageRequirements.memoryTypeBits & (1u << j)) { imageType = j; break; }
    }
    if (imageType == memoryProperties.memoryTypeCount) return false;
    VkMemoryAllocateInfo imageAllocation{};
    imageAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAllocation.allocationSize = imageRequirements.size;
    imageAllocation.memoryTypeIndex = imageType;
    if (f->vkAllocateMemory(native->dev, &imageAllocation, nullptr, &copy->imageMemory) != VK_SUCCESS ||
        f->vkBindImageMemory(native->dev, copy->image, copy->imageMemory, 0) != VK_SUCCESS) return false;
    std::memcpy(copy->mapped[0], source.constBits(), size_t(bytes));
    std::memset(copy->mapped[1], 0, size_t(bytes));
    VkCommandPoolCreateInfo commandPool{};
    commandPool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPool.queueFamilyIndex = native->gfxQueueFamilyIdx;
    if (f->vkCreateCommandPool(native->dev, &commandPool, nullptr, &copy->commands) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = copy->commands;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands;
    if (f->vkAllocateCommandBuffers(native->dev, &allocate, &commands) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (f->vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) return false;
    VkImageMemoryBarrier imageBarrier{};
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = copy->image;
    imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = image.extent;
    f->vkCmdCopyBufferToImage(commands, copy->buffers[0], copy->image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    f->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
    f->vkCmdCopyImageToBuffer(commands, copy->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              copy->buffers[1], 1, &region);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = copy->buffers[1];
    barrier.size = VK_WHOLE_SIZE;
    f->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 0, nullptr, 1, &barrier, 0, nullptr);
    if (f->vkEndCommandBuffer(commands) != VK_SUCCESS) return false;
    auto fence = std::make_shared<Fence>(f, native->dev);
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (f->vkCreateFence(native->dev, &fenceInfo, nullptr, &fence->handle) != VK_SUCCESS) return false;
    Copy* raw = copy.get();
    auto transfer = std::make_shared<LeasedResource<Copy>>(std::move(allowance), std::move(copy));
    GpuRetirement retirement(2); // Declared last: drain before device resources die.
    if (!retirement.retire(transfer, fence) || !retirement.retire(persistent, fence)) return false;
    transfer.reset();
    persistent.reset();
    if (pool->acquire({0, 0, 1})) return false;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    if (f->vkQueueSubmit(native->gfxQueue, 1, &submit, fence->handle) != VK_SUCCESS) {
        const auto idle = f->vkDeviceWaitIdle(native->dev);
        if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST)
            qFatal("Failed Vulkan submission could not establish safe teardown");
        return false;
    }
    fence->submitted = true;
    // No QRhi::finish or queue/device-idle in this successful retirement path.
    // Keep resources until both the fence and the owner-thread retirement agree.
    QElapsedTimer timeout;
    timeout.start();
    while (fence->poll() == GpuFence::Status::Pending && timeout.elapsed() < 5000)
        QThread::yieldCurrentThread();
    if (fence->poll() != GpuFence::Status::Complete) return false;
    if (std::memcmp(raw->mapped[1], source.constBits(), size_t(bytes))) return false;
    const auto charged = pool->used().upload;
    if (!retirement.collect() || retirement.pending() || pool->used().upload) return false;
    details["native_fence_retirement"] = true;
    details["native_transfer_bytes"] = qint64(bytes);
    details["native_fp16_image_upload"] = true;
    details["bytes_charged_before_retirement"] = qint64(charged);
    details["queue_drained"] = true;
    return true;
}
