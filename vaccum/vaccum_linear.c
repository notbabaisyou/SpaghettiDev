#include "vaccum_priv.h"
#include "vaccum_linear.h"

#define VACCUM_LINEAR_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define VACCUM_IMAGE_TRANSFER_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT)

static const VkImageSubresourceRange linear_subresources = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .levelCount = 1,
    .layerCount = 1,
};

static const VkImageSubresourceLayers linear_layers = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .layerCount = 1,
};

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
    VkImageFormatProperties image;

    if (vkGetPhysicalDeviceImageFormatProperties(
            screen->phys_device, VACCUM_LINEAR_FORMAT, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_LINEAR, VACCUM_IMAGE_TRANSFER_USAGE, 0,
            &image) == VK_SUCCESS)
        screen->linear_image_extent = image.maxExtent;
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
    vkDestroyImage(screen->device, pixels->image, NULL);
    vkFreeMemory(screen->device, pixels->memory, NULL);
    *pixels = (struct vaccum_linear_pixmap) { 0 };
}

static Bool
map_backing(vaccum_screen_private *screen,
             struct vaccum_linear_pixmap *pixels, VkMemoryRequirements req,
             VkDeviceSize offset)
{
    VkDevice device = screen->device;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
    };

    const VkPhysicalDeviceMemoryProperties *memory =
        &screen->dev_mem_properties;
    VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (unsigned i = 0; i < memory->memoryTypeCount; i++) {
        VkMemoryPropertyFlags properties =
            memory->memoryTypes[i].propertyFlags;
        Bool allowed = (req.memoryTypeBits & (1u << i)) != 0;
        Bool coherent_mapping = (properties & flags) == flags;

        if (!allowed || !coherent_mapping)
            continue;

        alloc.memoryTypeIndex = i;
        if (vkAllocateMemory(device, &alloc, NULL, &pixels->memory) !=
            VK_SUCCESS)
            continue;

        VkResult bound = pixels->image ?
            vkBindImageMemory(device, pixels->image, pixels->memory, 0) :
            vkBindBufferMemory(device, pixels->buffer, pixels->memory, 0);

        if (bound != VK_SUCCESS)
            return FALSE;

        void *map = NULL;

        if (vkMapMemory(device, pixels->memory, 0, VK_WHOLE_SIZE, 0,
                        &map) != VK_SUCCESS)
            return FALSE;

        pixels->map = (char *)map + offset;
        return TRUE;
    }

    return FALSE;
}

static Bool
create_linear_image(vaccum_screen_private *screen,
                     struct vaccum_linear_pixmap *pixels, int width,
                     int height, int stride)
{
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        /* Copy four-byte texels without changing the X pixel bits. */
        .format = VACCUM_LINEAR_FORMAT,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VACCUM_IMAGE_TRANSFER_USAGE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };

    const VkImageSubresource subresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    };

    if (vkCreateImage(screen->device, &info, NULL, &pixels->image) !=
        VK_SUCCESS)
        return FALSE;

    VkSubresourceLayout layout;
    VkMemoryRequirements req;

    vkGetImageSubresourceLayout(screen->device, pixels->image, &subresource,
                                &layout);
    vkGetImageMemoryRequirements(screen->device, pixels->image, &req);

    Bool aligned = layout.offset % sizeof(FbBits) == 0 &&
                   layout.rowPitch % sizeof(FbBits) == 0;
    Bool pitch_valid = layout.rowPitch >= (VkDeviceSize)stride &&
                       layout.rowPitch <= INT_MAX;
    Bool fits = layout.offset <= req.size &&
                layout.rowPitch <= (req.size - layout.offset) / height;

    if (!aligned || !pitch_valid || !fits)
        return FALSE;
    if (!map_backing(screen, pixels, req, layout.offset))
        return FALSE;

    pixels->stride = layout.rowPitch;
    pixels->layout = info.initialLayout;
    return TRUE;
}

Bool
vaccum_linear_allocate(vaccum_screen_private *screen,
                       struct vaccum_linear_pixmap *pixels, int width,
                       int height, int stride)
{
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)stride * height,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };

    Bool image_size = width <= screen->linear_image_extent.width &&
                      height <= screen->linear_image_extent.height;
    VkMemoryRequirements req;

    if (image_size && create_linear_image(screen, pixels, width, height,
                                          stride))
        goto allocated;

    vaccum_linear_release(screen, pixels);
    if (vkCreateBuffer(screen->device, &info, NULL, &pixels->buffer) !=
        VK_SUCCESS)
        return FALSE;

    vkGetBufferMemoryRequirements(screen->device, pixels->buffer, &req);
    if (!map_backing(screen, pixels, req, 0)) {
        vaccum_linear_release(screen, pixels);
        return FALSE;
    }

    pixels->stride = stride;
allocated:
    pixels->width = width;
    pixels->height = height;
    return TRUE;
}

static VkImageMemoryBarrier
transfer_image(const struct vaccum_linear_pixmap *pixels,
               VkAccessFlags access)
{
    return (VkImageMemoryBarrier) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = access,
        .oldLayout = pixels->layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = pixels->image,
        .subresourceRange = linear_subresources,
    };
}

static void
copy_image_box(VkCommandBuffer cmd, const struct vaccum_linear_pixmap *src,
                const struct vaccum_linear_pixmap *dst, BoxPtr box,
                int dx, int dy)
{
    VkOffset3D src_offset = { box->x1 + dx, box->y1 + dy, 0 };
    VkOffset3D dst_offset = { box->x1, box->y1, 0 };
    VkExtent3D extent = { box->x2 - box->x1, box->y2 - box->y1, 1 };

    if (src->image && dst->image) {
        const VkImageCopy region = {
            .srcSubresource = linear_layers,
            .srcOffset = src_offset,
            .dstSubresource = linear_layers,
            .dstOffset = dst_offset,
            .extent = extent,
        };

        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return;
    }

    const struct vaccum_linear_pixmap *buffer = src->buffer ? src : dst;
    VkOffset3D buffer_offset = src->buffer ? src_offset : dst_offset;
    const VkBufferImageCopy region = {
        .bufferOffset = (VkDeviceSize)buffer_offset.y * buffer->stride +
                        buffer_offset.x * sizeof(uint32_t),
        .bufferRowLength = buffer->stride / sizeof(uint32_t),
        .imageSubresource = linear_layers,
        .imageOffset = src->buffer ? dst_offset : src_offset,
        .imageExtent = extent,
    };

    if (src->buffer)
        vkCmdCopyBufferToImage(cmd, buffer->buffer, dst->image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    else
        vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL,
                               buffer->buffer, 1, &region);
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

    const VkMemoryBarrier ready = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
    };

    /* The first layout transition can write the source image too. */
    const VkMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT,
    };

    VkImageMemoryBarrier images[2];
    uint32_t image_count = 0;

    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS)
        return FALSE;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
        return FALSE;

    if (src->image)
        images[image_count++] =
            transfer_image(src, VK_ACCESS_TRANSFER_READ_BIT);
    if (dst->image)
        images[image_count++] =
            transfer_image(dst, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &ready, 0,
                         NULL, image_count, images);

    for (int i = 0; i < count; i++) {
        BoxPtr b = &boxes[i];

        if (image_count) {
            copy_image_box(cmd, src, dst, b, dx, dy);
            continue;
        }

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

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0,
                         NULL);
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

    if (src->image)
        src->layout = VK_IMAGE_LAYOUT_GENERAL;
    if (dst->image)
        dst->layout = VK_IMAGE_LAYOUT_GENERAL;
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
    Bool fits = drawable->width <= pixels->width &&
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
