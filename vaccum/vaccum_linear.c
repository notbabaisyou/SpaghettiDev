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

    if (vkGetPhysicalDeviceImageFormatProperties(
            screen->phys_device, VACCUM_LINEAR_FORMAT, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VACCUM_IMAGE_TRANSFER_USAGE, 0,
            &image) == VK_SUCCESS)
        screen->optimal_image_extent = image.maxExtent;
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
    vkFreeMemory(screen->device, pixels->image_memory, NULL);
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

static VkResult
create_image(VkDevice device, VkImage *image, VkImageTiling tiling,
              int width, int height)
{
    VkImageLayout initial = tiling == VK_IMAGE_TILING_LINEAR ?
        VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED;
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        /* Copy four-byte texels without changing the X pixel bits. */
        .format = VACCUM_LINEAR_FORMAT,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = tiling,
        .usage = VACCUM_IMAGE_TRANSFER_USAGE,
        .initialLayout = initial,
    };

    return vkCreateImage(device, &info, NULL, image);
}

static Bool
create_linear_image(vaccum_screen_private *screen,
                     struct vaccum_linear_pixmap *pixels, int width,
                     int height, int stride)
{
    const VkImageSubresource subresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    };

    if (create_image(screen->device, &pixels->image, VK_IMAGE_TILING_LINEAR,
                     width, height) != VK_SUCCESS)
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
    pixels->layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    return TRUE;
}

/* Keep the mapped buffer usable if the image allocation fails. */
static void
attach_optimal_image(vaccum_screen_private *screen,
                      struct vaccum_linear_pixmap *pixels,
                      int width, int height)
{
    VkDevice device = screen->device;
    VkImage image = VK_NULL_HANDLE;

    if (create_image(device, &image, VK_IMAGE_TILING_OPTIMAL, width,
                     height) != VK_SUCCESS)
        return;

    VkMemoryRequirements req;

    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type =
        vaccum_find_memory_type(screen, req.memoryTypeBits, properties);

    if (type == UINT32_MAX)
        goto free_image;

    const VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };

    VkDeviceMemory memory = VK_NULL_HANDLE;

    if (vkAllocateMemory(device, &alloc, NULL, &memory) != VK_SUCCESS)
        goto free_image;
    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, NULL);
        goto free_image;
    }

    pixels->image = image;
    pixels->image_memory = memory;
    pixels->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return;

free_image:
    vkDestroyImage(device, image, NULL);
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

    Bool linear_fits = width <= screen->linear_image_extent.width &&
                       height <= screen->linear_image_extent.height;
    Bool optimal_fits = width <= screen->optimal_image_extent.width &&
                        height <= screen->optimal_image_extent.height;
    VkMemoryRequirements req;

    if (linear_fits && create_linear_image(screen, pixels, width, height,
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
    if (optimal_fits)
        attach_optimal_image(screen, pixels, width, height);

allocated:
    pixels->width = width;
    pixels->height = height;
    return TRUE;
}

static VkImageMemoryBarrier
transfer_image(const struct vaccum_linear_pixmap *pixels,
               VkImageLayout layout, VkAccessFlags access)
{
    return (VkImageMemoryBarrier) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = access,
        .oldLayout = pixels->layout,
        .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = pixels->image,
        .subresourceRange = linear_subresources,
    };
}

static void
staging_read_barrier(VkCommandBuffer cmd,
                      const struct vaccum_linear_pixmap *pixels)
{
    VkImageMemoryBarrier ready =
        transfer_image(pixels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_TRANSFER_READ_BIT);

    ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ready.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &ready);
}

static VkBufferImageCopy
buffer_image_region(const struct vaccum_linear_pixmap *buffer,
                    VkOffset3D buffer_offset, VkOffset3D image_offset,
                    VkExtent3D extent)
{
    return (VkBufferImageCopy) {
        .bufferOffset = (VkDeviceSize)buffer_offset.y * buffer->stride +
                        buffer_offset.x * sizeof(uint32_t),
        .bufferRowLength = buffer->stride / sizeof(uint32_t),
        .imageSubresource = linear_layers,
        .imageOffset = image_offset,
        .imageExtent = extent,
    };
}

static void
copy_staging_boxes(VkCommandBuffer cmd,
                    const struct vaccum_linear_pixmap *pixels, BoxPtr boxes,
                    int count, int dx, int dy, Bool upload)
{
    for (int i = 0; i < count; i++) {
        BoxPtr box = &boxes[i];
        VkOffset3D offset = { box->x1 + dx, box->y1 + dy, 0 };
        VkExtent3D extent = { box->x2 - box->x1, box->y2 - box->y1, 1 };
        VkBufferImageCopy region =
            buffer_image_region(pixels, offset, offset, extent);

        if (upload)
            vkCmdCopyBufferToImage(cmd, pixels->buffer, pixels->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
        else
            vkCmdCopyImageToBuffer(cmd, pixels->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   pixels->buffer, 1, &region);
    }
}

static void
copy_image_box(VkCommandBuffer cmd, const struct vaccum_linear_pixmap *src,
                const struct vaccum_linear_pixmap *dst, BoxPtr box,
                int dx, int dy)
{
    VkOffset3D src_offset = { box->x1 + dx, box->y1 + dy, 0 };
    VkOffset3D dst_offset = { box->x1, box->y1, 0 };
    VkExtent3D extent = { box->x2 - box->x1, box->y2 - box->y1, 1 };

    VkImageLayout src_layout = src->image_memory ?
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkImageLayout dst_layout = dst->image_memory ?
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;

    if (src->image && dst->image) {
        const VkImageCopy region = {
            .srcSubresource = linear_layers,
            .srcOffset = src_offset,
            .dstSubresource = linear_layers,
            .dstOffset = dst_offset,
            .extent = extent,
        };

        vkCmdCopyImage(cmd, src->image, src_layout, dst->image, dst_layout, 1,
                       &region);
        return;
    }

    if (dst->image) {
        VkBufferImageCopy region =
            buffer_image_region(src, src_offset, dst_offset, extent);

        vkCmdCopyBufferToImage(cmd, src->buffer, dst->image, dst_layout, 1,
                               &region);
    } else {
        VkBufferImageCopy region =
            buffer_image_region(dst, dst_offset, src_offset, extent);

        vkCmdCopyImageToBuffer(cmd, src->image, src_layout, dst->buffer, 1,
                               &region);
    }
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
    Bool upload = src->image_memory != VK_NULL_HANDLE;
    Bool download = dst->image_memory != VK_NULL_HANDLE;

    VkImageLayout src_initial = upload ?
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkImageLayout dst_initial = download ?
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkAccessFlags src_access = upload ? VK_ACCESS_TRANSFER_WRITE_BIT :
                                       VK_ACCESS_TRANSFER_READ_BIT;

    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS)
        return FALSE;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
        return FALSE;

    if (src->image)
        images[image_count++] = transfer_image(src, src_initial, src_access);
    if (dst->image)
        images[image_count++] =
            transfer_image(dst, dst_initial, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &ready, 0,
                         NULL, image_count, images);

    if (upload) {
        copy_staging_boxes(cmd, src, boxes, count, dx, dy, TRUE);
        staging_read_barrier(cmd, src);
    }

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

    if (download) {
        staging_read_barrier(cmd, dst);
        copy_staging_boxes(cmd, dst, boxes, count, 0, 0, FALSE);
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
        src->layout = upload ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
                               VK_IMAGE_LAYOUT_GENERAL;
    if (dst->image)
        dst->layout = download ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
                                 VK_IMAGE_LAYOUT_GENERAL;
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
