#include "vaccum_priv.h"
#include "vaccum_linear.h"

struct vaccum_screen_private *
vaccum_linear_init(int fd)
{
    vaccum_screen_private *screen = calloc(1, sizeof(*screen));

    if (!screen)
        return NULL;

    if (!vaccum_vulkan_init(screen, fd)) {
        vaccum_linear_fini(screen);
        return NULL;
    }
    return screen;
}

void
vaccum_linear_fini(vaccum_screen_private *screen)
{
    vaccum_vulkan_fini(screen);
    free(screen);
}

void
vaccum_linear_release(vaccum_screen_private *screen,
                      struct vaccum_linear_pixmap *pixels)
{
    if (pixels->map)
        vkUnmapMemory(screen->device, pixels->memory);
    vkDestroyBuffer(screen->device, pixels->buffer, NULL);
    vkFreeMemory(screen->device, pixels->memory, NULL);
    *pixels = (struct vaccum_linear_pixmap) { 0 };
}

Bool
vaccum_linear_allocate(vaccum_screen_private *screen,
                       struct vaccum_linear_pixmap *pixels, int stride,
                       int height)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)stride * height,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };

    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    };

    VkMemoryRequirements req;
    const VkPhysicalDeviceMemoryProperties *memory =
        &screen->dev_mem_properties;
    VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (vkCreateBuffer(screen->device, &info, NULL, &pixels->buffer) !=
        VK_SUCCESS)
        return FALSE;
    vkGetBufferMemoryRequirements(screen->device, pixels->buffer, &req);
    alloc.allocationSize = req.size;

    for (unsigned i = 0; i < memory->memoryTypeCount; i++) {
        VkMemoryPropertyFlags properties =
            memory->memoryTypes[i].propertyFlags;
        Bool allowed = (req.memoryTypeBits & (1u << i)) != 0;
        Bool has_flags = (properties & flags) == flags;

        if (!allowed || !has_flags)
            continue;
        alloc.memoryTypeIndex = i;
        if (vkAllocateMemory(screen->device, &alloc, NULL, &pixels->memory) !=
            VK_SUCCESS)
            continue;
        if (vkBindBufferMemory(screen->device, pixels->buffer, pixels->memory,
                               0) != VK_SUCCESS)
            break;
        if (vkMapMemory(screen->device, pixels->memory, 0, info.size, 0,
                        &pixels->map) != VK_SUCCESS)
            break;

        pixels->stride = stride;
        pixels->height = height;
        return TRUE;
    }

    vaccum_linear_release(screen, pixels);
    return FALSE;
}

Bool
vaccum_linear_copy(vaccum_screen_private *screen,
                   struct vaccum_linear_pixmap *src,
                   struct vaccum_linear_pixmap *dst, BoxPtr boxes, int count,
                   int dx, int dy)
{
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = screen->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };

    Bool copied = FALSE;

    if (vkAllocateCommandBuffers(screen->device, &alloc, &cmd) != VK_SUCCESS)
        return FALSE;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
        goto out;

    for (int i = 0; i < count; i++) {
        BoxPtr b = &boxes[i];

        for (int y = b->y1; y < b->y2; y++) {
            VkBufferCopy row = {
                .srcOffset = (VkDeviceSize)(y + dy) * src->stride +
                             (b->x1 + dx) * sizeof(uint32_t),
                .dstOffset =
                    (VkDeviceSize)y * dst->stride + b->x1 * sizeof(uint32_t),
                .size = (b->x2 - b->x1) * sizeof(uint32_t),
            };

            vkCmdCopyBuffer(cmd, src->buffer, dst->buffer, 1, &row);
        }
    }

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL,
                         0, NULL);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        goto out;
    if (vkQueueSubmit(screen->queue, 1, &submit, VK_NULL_HANDLE) ==
        VK_SUCCESS)
        copied = vkQueueWaitIdle(screen->queue) == VK_SUCCESS;

out:
    vkFreeCommandBuffers(screen->device, screen->command_pool, 1, &cmd);
    return copied;
}
