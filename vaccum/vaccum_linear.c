#include "vaccum_priv.h"
#include "vaccum_linear.h"

struct vaccum_screen_private *
vaccum_linear_init(int fd)
{
    vaccum_screen_private *screen = calloc(1, sizeof(*screen));

    if (!screen)
        return NULL;
    if (!vaccum_vulkan_init(screen, fd))
        goto fail;

    const VkCommandBufferAllocateInfo command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = screen->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    const VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };

    if (vkAllocateCommandBuffers(screen->device, &command,
                                 &screen->linear_copy_cmd) != VK_SUCCESS)
        goto fail;
    if (vkCreateFence(screen->device, &fence, NULL,
                      &screen->linear_copy_fence) != VK_SUCCESS)
        goto fail;
    return screen;

fail:
    vaccum_linear_fini(screen);
    return NULL;
}

void
vaccum_linear_fini(vaccum_screen_private *screen)
{
    if (screen->linear_copy_fence)
        vkDestroyFence(screen->device, screen->linear_copy_fence, NULL);
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
    VkCommandBuffer cmd = screen->linear_copy_cmd;
    VkFence fence = screen->linear_copy_fence;
    VkDevice device = screen->device;
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    const VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst->buffer,
        .size = VK_WHOLE_SIZE,
    };

    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS)
        return FALSE;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
        return FALSE;

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
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier,
                         0, NULL);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return FALSE;
    if (vkResetFences(device, 1, &fence) != VK_SUCCESS)
        return FALSE;

    VkResult submitted = vkQueueSubmit(screen->queue, 1, &submit, fence);

    if (submitted == VK_ERROR_DEVICE_LOST)
        FatalError("vaccum: Vulkan device lost while submitting pixmap"
                   " copy\n");
    if (submitted != VK_SUCCESS)
        return FALSE;

    /* Framebuffer code can access either mapping as soon as we return. */
    VkResult completed = vkWaitForFences(device, 1, &fence, VK_TRUE,
                                         UINT64_MAX);

    if (completed != VK_SUCCESS)
        FatalError("vaccum: Waiting for Vulkan pixmap copy failed (%d)\n",
                   completed);
    return TRUE;
}

struct vaccum_copy_context {
    vaccum_screen_private *screen;
    struct vaccum_linear_pixmap *src;
    struct vaccum_linear_pixmap *dst;
};

static Bool
vaccum_can_copy_pixmap(DrawablePtr drawable,
                       struct vaccum_linear_pixmap *pixels)
{
    PixmapPtr pixmap = (PixmapPtr)drawable;
    Bool origin = drawable->x == 0 && drawable->y == 0;
    Bool layout =
        drawable->bitsPerPixel == 32 && pixmap->devKind == pixels->stride;
    Bool fits = drawable->width <= pixels->stride / sizeof(uint32_t) &&
                drawable->height <= pixels->height;
    Bool mapped = pixels->map && pixmap->devPrivate.ptr == pixels->map;
    Bool supported = origin && layout;

    return supported && fits && mapped;
}

static void
vaccum_copy_boxes(DrawablePtr src, DrawablePtr dst, GCPtr gc, BoxPtr boxes,
                  int count, int dx, int dy, Bool reverse, Bool upsidedown,
                  Pixel bitplane, void *closure)
{
    struct vaccum_copy_context *copy = closure;

    if (vaccum_can_copy_pixmap(src, copy->src) &&
        vaccum_can_copy_pixmap(dst, copy->dst) &&
        vaccum_linear_copy(copy->screen, copy->src, copy->dst, boxes, count,
                           dx, dy))
        return;
    fbCopyNtoN(src, dst, gc, boxes, count, dx, dy, reverse, upsidedown,
               bitplane, NULL);
}

RegionPtr
vaccum_linear_copy_area(struct vaccum_screen_private *screen,
                        struct vaccum_linear_pixmap *src,
                        struct vaccum_linear_pixmap *dst, DrawablePtr source,
                        DrawablePtr dest, GCPtr gc, int srcx, int srcy,
                        int width, int height, int dstx, int dsty)
{
    unsigned long mask = FbFullMask(dest->depth);
    Bool plain_copy = gc->alu == GXcopy && (gc->planemask & mask) == mask;

    if (!plain_copy || source == dest)
        return fbCopyArea(source, dest, gc, srcx, srcy, width, height, dstx,
                          dsty);

    struct vaccum_copy_context context = { screen, src, dst };

    return miDoCopy(source, dest, gc, srcx, srcy, width, height, dstx, dsty,
                    vaccum_copy_boxes, 0, &context);
}
