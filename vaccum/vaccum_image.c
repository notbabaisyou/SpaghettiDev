#include "vaccum_priv.h"

static uint32_t
vaccum_find_memory_type(struct vaccum_screen_private *vaccum_priv,
                        uint32_t type_filter,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties *mem_props = &vaccum_priv->dev_mem_properties;
    for (uint32_t i = 0; i < mem_props->memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props->memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return 0;
}

void
vaccum_destroy_image(struct vaccum_screen_private *vaccum_priv, struct vaccum_image *image)
{
    if (image->image_view)
        vkDestroyImageView(vaccum_priv->device, image->image_view, NULL);

    vkDestroyImage(vaccum_priv->device, image->image, NULL);

    for (uint32_t i = 0; i < image->num_memories; i++) {
        if (image->memories[i])
            vkFreeMemory(vaccum_priv->device, image->memories[i], NULL);
    }
}

struct vaccum_image *
vaccum_create_image(struct vaccum_screen_private *vaccum_priv, PixmapPtr pixmap,
                    int w, int h)
{
    const struct vaccum_format *f = vaccum_format_for_pixmap(pixmap);
    struct vaccum_image *image;

    image = calloc(1, sizeof(*image));
    if (image == NULL)
        return NULL;

    VkExtent3D extent = { w, h, 1 };
    VkImageCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = f->format;
    create_info.extent = extent;
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;

    VkResult result = vkCreateImage(vaccum_priv->device, &create_info, NULL, &image->image);
    if (result != VK_SUCCESS) {
        free(image);
        return NULL;
    }

    VkMemoryRequirements img_reqs;
    vkGetImageMemoryRequirements(vaccum_priv->device, image->image, &img_reqs);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = img_reqs.size;
    allocate_info.memoryTypeIndex = vaccum_find_memory_type(
        vaccum_priv,
        img_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    result = vkAllocateMemory(vaccum_priv->device, &allocate_info, NULL, &image->memories[0]);
    if (result != VK_SUCCESS) {
        vkDestroyImage(vaccum_priv->device, image->image, NULL);
        free(image);
        return NULL;
    }

    image->num_memories = 1;

    vkBindImageMemory(vaccum_priv->device, image->image, image->memories[0], 0);

    VkImageViewCreateInfo imgv_create_info = {};
    imgv_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imgv_create_info.image = image->image;
    imgv_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imgv_create_info.format = f->format;
    imgv_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imgv_create_info.subresourceRange.levelCount = 1;
    imgv_create_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(vaccum_priv->device, &imgv_create_info, NULL, &image->image_view);
    return image;
}

struct vaccum_image *
vaccum_create_image_exportable(struct vaccum_screen_private *vaccum_priv,
                               PixmapPtr pixmap, int w, int h)
{
    const struct vaccum_format *f = vaccum_format_for_pixmap(pixmap);
    struct vaccum_image *image;

    image = calloc(1, sizeof(*image));
    if (image == NULL)
        return NULL;

    VkExtent3D extent = { w, h, 1 };

    VkExternalMemoryImageCreateInfo ext_info = {};
    ext_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.pNext = &ext_info;
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = f->format;
    create_info.extent = extent;
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;

    VkResult result = vkCreateImage(vaccum_priv->device, &create_info, NULL, &image->image);
    if (result != VK_SUCCESS) {
        free(image);
        return NULL;
    }

    VkMemoryRequirements img_reqs;
    vkGetImageMemoryRequirements(vaccum_priv->device, image->image, &img_reqs);

    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &export_info;
    allocate_info.allocationSize = img_reqs.size;
    allocate_info.memoryTypeIndex = vaccum_find_memory_type(
        vaccum_priv,
        img_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    result = vkAllocateMemory(vaccum_priv->device, &allocate_info, NULL, &image->memories[0]);
    if (result != VK_SUCCESS) {
        vkDestroyImage(vaccum_priv->device, image->image, NULL);
        free(image);
        return NULL;
    }

    image->num_memories = 1;

    vkBindImageMemory(vaccum_priv->device, image->image, image->memories[0], 0);

    VkImageViewCreateInfo imgv_create_info = {};
    imgv_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imgv_create_info.image = image->image;
    imgv_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imgv_create_info.format = f->format;
    imgv_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imgv_create_info.subresourceRange.levelCount = 1;
    imgv_create_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(vaccum_priv->device, &imgv_create_info, NULL, &image->image_view);
    return image;
}

struct vaccum_image *
vaccum_pixmap_detach_image(vaccum_pixmap_private *pixmap_priv)
{
    struct vaccum_image *image;

    if (pixmap_priv == NULL)
        return NULL;

    image = pixmap_priv->image;
    if (image == NULL)
        return NULL;

    pixmap_priv->image = NULL;
    return image;
}

/* The pixmap must not be attached to another fbo. */
void
vaccum_pixmap_attach_image(PixmapPtr pixmap, struct vaccum_image *image)
{
    vaccum_pixmap_private *pixmap_priv;

    pixmap_priv = vaccum_get_pixmap_private(pixmap);

    if (pixmap_priv->image)
        return;

    pixmap_priv->image = image;

    switch (pixmap_priv->type) {
    case VACCUM_IMAGE_ONLY:
        //    case VACCUM_TEXTURE_DRM:
        pixmap_priv->vk_image = VACCUM_IMAGE_NORMAL;
        pixmap->devPrivate.ptr = NULL;
    default:
        break;
    }
}
