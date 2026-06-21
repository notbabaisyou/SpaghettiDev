#include "vaccum_priv.h"

#include <drm_fourcc.h>
#include <xf86.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <errno.h>
#ifdef DRI3
#include "dri3.h"
#include "vaccum_dri3_syncobj.h"
#endif

static const struct {
    uint32_t drm_format;
    VkFormat vk_format;
    uint32_t gbm_format;
} vaccum_drm_format_map[] = {
    { DRM_FORMAT_R8,             VK_FORMAT_R8_UNORM,              GBM_FORMAT_R8 },
    { DRM_FORMAT_RGB565,         VK_FORMAT_R5G6B5_UNORM_PACK16,  GBM_FORMAT_RGB565 },
    { DRM_FORMAT_ABGR8888,       VK_FORMAT_R8G8B8A8_UNORM,       GBM_FORMAT_ARGB8888 },
    { DRM_FORMAT_XRGB8888,       VK_FORMAT_R8G8B8A8_UNORM,       GBM_FORMAT_XRGB8888 },
    { DRM_FORMAT_XRGB2101010,    VK_FORMAT_A2R10G10B10_UNORM_PACK32, GBM_FORMAT_ARGB2101010 },
    { 0, 0, 0 }
};

static VkFormat
vaccum_vk_format_for_drm(uint32_t drm_format)
{
    for (int i = 0; vaccum_drm_format_map[i].drm_format != 0; i++) {
        if (vaccum_drm_format_map[i].drm_format == drm_format)
            return vaccum_drm_format_map[i].vk_format;
    }
    return VK_FORMAT_UNDEFINED;
}

static uint32_t
vaccum_gbm_format_for_drm(uint32_t drm_format)
{
    for (int i = 0; vaccum_drm_format_map[i].drm_format != 0; i++) {
        if (vaccum_drm_format_map[i].drm_format == drm_format)
            return vaccum_drm_format_map[i].gbm_format;
    }
    return 0;
}

Bool
vaccum_supports_pixmap_import_export(ScreenPtr screen)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    if (!vaccum_priv)
        return FALSE;
    return vaccum_priv->drm_fd >= 0;
}

void
vaccum_set_drawable_modifiers_func(ScreenPtr screen,
                                    GetDrawableModifiersFuncPtr func)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    if (vaccum_priv)
        vaccum_priv->modifiers_func = func;
}

void
vaccum_enable_dri3(ScreenPtr screen)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    if (vaccum_priv)
        vaccum_priv->dri3_enabled = TRUE;
}

int
vaccum_name_from_pixmap(PixmapPtr pixmap, CARD16 *stride, CARD32 *size)
{
    return -1;
}

static uint64_t
vaccum_query_image_modifier(vaccum_screen_private *vaccum_priv, VkImage image)
{
    VkImageDrmFormatModifierPropertiesEXT modifier_info = {};
    modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;

    VkResult result = vkGetImageDrmFormatModifierPropertiesEXT(
        vaccum_priv->device, image, &modifier_info);
    if (result != VK_SUCCESS)
        return DRM_FORMAT_MOD_LINEAR;

    return modifier_info.drmFormatModifier;
}

int
vaccum_shareable_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                                 CARD16 *stride, CARD32 *size)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    vaccum_pixmap_private *pixmap_priv;
    struct vaccum_image *image;
    VkResult result;
    int fd;

    if (!vaccum_priv)
        return -1;

    pixmap_priv = vaccum_get_pixmap_private(pixmap);
    if (!pixmap_priv || !pixmap_priv->image)
        return -1;

    image = pixmap_priv->image;

    uint64_t modifier = vaccum_query_image_modifier(vaccum_priv, image->image);
    if (modifier == DRM_FORMAT_MOD_LINEAR) {
        VkMemoryGetFdInfoKHR get_fd_info = {};
        get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        get_fd_info.memory = image->memories[0];
        get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        result = vkGetMemoryFdKHR(vaccum_priv->device, &get_fd_info, &fd);
        if (result != VK_SUCCESS)
            return -1;

        *stride = pixmap->devKind;
        *size = *stride * pixmap->drawable.height;
        return fd;
    }

    const struct vaccum_format *f = vaccum_format_for_pixmap(pixmap);
    int w = pixmap->drawable.width;
    int h = pixmap->drawable.height;

    VkImageCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = f->format;
    create_info.extent = (VkExtent3D){ w, h, 1 };
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_LINEAR;
    create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkImage linear_image;
    result = vkCreateImage(vaccum_priv->device, &create_info, NULL, &linear_image);
    if (result != VK_SUCCESS)
        return -1;

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vaccum_priv->device, linear_image, &mem_reqs);

    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &export_info;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = 0;

    for (uint32_t i = 0; i < vaccum_priv->dev_mem_properties.memoryTypeCount; i++) {
        if ((mem_reqs.memoryTypeBits & (1 << i)) &&
            (vaccum_priv->dev_mem_properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            alloc_info.memoryTypeIndex = i;
            break;
        }
    }

    VkDeviceMemory linear_memory;
    result = vkAllocateMemory(vaccum_priv->device, &alloc_info, NULL, &linear_memory);
    if (result != VK_SUCCESS) {
        vkDestroyImage(vaccum_priv->device, linear_image, NULL);
        return -1;
    }

    result = vkBindImageMemory(vaccum_priv->device, linear_image, linear_memory, 0);
    if (result != VK_SUCCESS) {
        vkDestroyImage(vaccum_priv->device, linear_image, NULL);
        return -1;
    }

    vaccum_alloc_cmd_buffer(vaccum_priv);

    VkImageCopy copy_region = {};
    copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.srcSubresource.layerCount = 1;
    copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.dstSubresource.layerCount = 1;
    copy_region.extent = (VkExtent3D){ w, h, 1 };

    vkCmdCopyImage(vaccum_priv->current_cmd,
                   image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   linear_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copy_region);

    vaccum_flush_cmds(vaccum_priv);

    VkImageSubresource subres = {};
    subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

    VkSubresourceLayout linear_layout;
    vkGetImageSubresourceLayout(vaccum_priv->device, linear_image,
                                &subres, &linear_layout);

    VkMemoryGetFdInfoKHR get_fd_info = {};
    get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get_fd_info.memory = linear_memory;
    get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    result = vkGetMemoryFdKHR(vaccum_priv->device, &get_fd_info, &fd);

    vkFreeMemory(vaccum_priv->device, linear_memory, NULL);
    vkDestroyImage(vaccum_priv->device, linear_image, NULL);

    if (result != VK_SUCCESS)
        return -1;

    *stride = linear_layout.rowPitch;
    *size = *stride * h;

    return fd;
}

int
vaccum_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                      CARD16 *stride, CARD32 *size)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    vaccum_pixmap_private *pixmap_priv;
    struct vaccum_image *image;
    VkResult result;
    int fd;

    if (!vaccum_priv)
        return -1;

    pixmap_priv = vaccum_get_pixmap_private(pixmap);
    if (!pixmap_priv || !pixmap_priv->image)
        return -1;

    image = pixmap_priv->image;

    VkMemoryGetFdInfoKHR get_fd_info = {};
    get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get_fd_info.memory = image->memories[0];
    get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    result = vkGetMemoryFdKHR(vaccum_priv->device, &get_fd_info, &fd);
    if (result != VK_SUCCESS)
        return -1;

    *stride = pixmap->devKind;
    *size = *stride * pixmap->drawable.height;

    return fd;
}

int
vaccum_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                       int *fds, uint32_t *strides, uint32_t *offsets,
                       uint64_t *modifier)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    vaccum_pixmap_private *pixmap_priv;
    struct vaccum_image *image;

    if (!vaccum_priv)
        return 0;

    pixmap_priv = vaccum_get_pixmap_private(pixmap);
    if (!pixmap_priv || !pixmap_priv->image)
        return 0;

    image = pixmap_priv->image;

    if (image->num_memories > 1) {
        uint32_t plane_count = image->num_memories;

        for (uint32_t p = 0; p < plane_count; p++) {
            VkMemoryGetFdInfoKHR get_fd_info = {};
            get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            get_fd_info.memory = image->memories[p];
            get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

            VkResult result = vkGetMemoryFdKHR(vaccum_priv->device, &get_fd_info, &fds[p]);
            if (result != VK_SUCCESS) {
                for (uint32_t j = 0; j < p; j++)
                    close(fds[j]);
                return 0;
            }

            VkImageSubresource subres = {};
            subres.aspectMask = (1 << p);
            subres.mipLevel = 0;
            subres.arrayLayer = 0;

            VkSubresourceLayout layout;
            vkGetImageSubresourceLayout(vaccum_priv->device, image->image,
                                        &subres, &layout);
            strides[p] = layout.rowPitch;
            offsets[p] = layout.offset;
        }

        *modifier = vaccum_query_image_modifier(vaccum_priv, image->image);
        return plane_count;
    }

    CARD16 stride;
    CARD32 size;
    int fd = vaccum_fd_from_pixmap(screen, pixmap, &stride, &size);
    if (fd < 0)
        return 0;

    fds[0] = fd;
    strides[0] = stride;
    offsets[0] = 0;
    *modifier = vaccum_query_image_modifier(vaccum_priv, image->image);

    return 1;
}

static Bool
vaccum_import_fd_to_pixmap(PixmapPtr pixmap, int fd,
                            CARD16 width, CARD16 height,
                            CARD16 stride, CARD8 depth, CARD8 bpp,
                            uint64_t modifier)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    struct vaccum_image *image;
    const struct vaccum_format *f;
    VkResult result;

    if (!vaccum_priv)
        return FALSE;

    f = &vaccum_priv->formats[depth];
    if (!f->format)
        return FALSE;

    image = calloc(1, sizeof(*image));
    if (!image)
        return FALSE;

    VkExternalMemoryImageCreateInfo ext_info = {};
    ext_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageDrmFormatModifierListCreateInfoEXT modifier_list_info = {};
    modifier_list_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_explicit_info = {};
    modifier_explicit_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;

    VkSubresourceLayout plane_layout = {};
    plane_layout.rowPitch = stride;
    plane_layout.size = (VkDeviceSize)stride * height;

    VkImageCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.pNext = &ext_info;
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = f->format;
    create_info.extent = (VkExtent3D){ width, height, 1 };
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;

    if (modifier == DRM_FORMAT_MOD_INVALID) {
        VkDrmFormatModifierPropertiesListEXT props_list = {};
        props_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

        VkFormatProperties2 fmt_props = {};
        fmt_props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        fmt_props.pNext = &props_list;

        props_list.drmFormatModifierCount = 0;
        vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device, f->format, &fmt_props);

        if (props_list.drmFormatModifierCount > 0) {
            uint64_t *mods = calloc(props_list.drmFormatModifierCount, sizeof(uint64_t));
            if (mods) {
                VkDrmFormatModifierPropertiesEXT *mod_props =
                    calloc(props_list.drmFormatModifierCount,
                           sizeof(VkDrmFormatModifierPropertiesEXT));
                if (mod_props) {
                    props_list.pDrmFormatModifierProperties = mod_props;
                    vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device,
                                                         f->format, &fmt_props);

                    uint32_t count = 0;
                    for (uint32_t i = 0; i < props_list.drmFormatModifierCount; i++) {
                        if (mod_props[i].drmFormatModifierTilingFeatures &
                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                            mods[count++] = mod_props[i].drmFormatModifier;
                    }

                    if (count > 0) {
                        modifier_list_info.drmFormatModifierCount = count;
                        modifier_list_info.pDrmFormatModifiers = mods;
                        modifier_list_info.pNext = create_info.pNext;
                        create_info.pNext = &modifier_list_info;
                        create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                    }
                    free(mod_props);
                }
                free(mods);
            }
        }
    } else if (modifier == DRM_FORMAT_MOD_LINEAR) {
        create_info.tiling = VK_IMAGE_TILING_LINEAR;
    } else {
        modifier_explicit_info.drmFormatModifier = modifier;
        modifier_explicit_info.drmFormatModifierPlaneCount = 1;

        modifier_explicit_info.pPlaneLayouts = &plane_layout;
        modifier_explicit_info.pNext = create_info.pNext;
        create_info.pNext = &modifier_explicit_info;
        create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    }

    result = vkCreateImage(vaccum_priv->device, &create_info, NULL, &image->image);
    if (result != VK_SUCCESS) {
        ErrorF("vaccum: vkCreateImage failed with %d\n", (int)result);
        free(image);
        return FALSE;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vaccum_priv->device, image->image, &mem_reqs);

    VkImportMemoryFdInfoKHR import_info = {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = fd;

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &import_info;
    alloc_info.allocationSize = mem_reqs.size;

    uint32_t compatible_bits = mem_reqs.memoryTypeBits;

    alloc_info.memoryTypeIndex = 0;
    for (uint32_t i = 0; i < vaccum_priv->dev_mem_properties.memoryTypeCount; i++) {
        if ((compatible_bits & (1 << i)) &&
            (vaccum_priv->dev_mem_properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc_info.memoryTypeIndex = i;
            break;
        }
    }

    if (alloc_info.memoryTypeIndex == 0) {
        for (uint32_t i = 0; i < vaccum_priv->dev_mem_properties.memoryTypeCount; i++) {
            if (compatible_bits & (1 << i)) {
                alloc_info.memoryTypeIndex = i;
                break;
            }
        }
    }

    result = vkAllocateMemory(vaccum_priv->device, &alloc_info, NULL, &image->memories[0]);
    if (result != VK_SUCCESS) {
        ErrorF("vaccum: vkAllocateMemory failed with %d\n", (int)result);
        vkDestroyImage(vaccum_priv->device, image->image, NULL);
        free(image);
        return FALSE;
    }

    image->num_memories = 1;

    result = vkBindImageMemory(vaccum_priv->device, image->image, image->memories[0], 0);
    if (result != VK_SUCCESS) {
        ErrorF("vaccum: vkBindImageMemory failed with %d\n", (int)result);
        vkDestroyImage(vaccum_priv->device, image->image, NULL);
        free(image);
        return FALSE;
    }

    VkImageViewCreateInfo imgv_info = {};
    imgv_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imgv_info.image = image->image;
    imgv_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imgv_info.format = f->format;
    imgv_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imgv_info.subresourceRange.levelCount = 1;
    imgv_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(vaccum_priv->device, &imgv_info, NULL, &image->image_view);
    if (result != VK_SUCCESS) {
        ErrorF("vaccum: vkCreateImageView failed with %d\n", (int)result);
        vkFreeMemory(vaccum_priv->device, image->memories[0], NULL);
        vkDestroyImage(vaccum_priv->device, image->image, NULL);
        free(image);
        return FALSE;
    }

    screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, stride, NULL);
    vaccum_pixmap_attach_image(pixmap, image);
    return TRUE;
}

PixmapPtr
vaccum_pixmap_from_fds(ScreenPtr screen,
                       CARD8 num_fds, const int *fds,
                       CARD16 width, CARD16 height,
                       const CARD32 *strides,
                       const CARD32 *offsets,
                       CARD8 depth, CARD8 bpp, uint64_t modifier)
{
    PixmapPtr pixmap;

    if (num_fds != 1)
        return NULL;

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    if (!pixmap)
        return NULL;

    if (!vaccum_import_fd_to_pixmap(pixmap, fds[0], width, height,
                                    strides[0], depth, bpp, modifier)) {
        screen->DestroyPixmap(pixmap);
        return NULL;
    }

    return pixmap;
}

PixmapPtr
vaccum_pixmap_from_fd(ScreenPtr screen, int fd,
                      CARD16 width, CARD16 height,
                      CARD16 stride, CARD8 depth, CARD8 bpp)
{
    PixmapPtr pixmap;

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    if (!pixmap)
        return NULL;

    if (!vaccum_import_fd_to_pixmap(pixmap, fd, width, height,
                                    stride, depth, bpp,
                                    DRM_FORMAT_MOD_LINEAR)) {
        screen->DestroyPixmap(pixmap);
        return NULL;
    }

    return pixmap;
}

Bool
vaccum_back_pixmap_from_fd(PixmapPtr pixmap, int fd,
                           CARD16 width, CARD16 height,
                           CARD16 stride, CARD8 depth, CARD8 bpp)
{
    return vaccum_import_fd_to_pixmap(pixmap, fd, width, height,
                                      stride, depth, bpp,
                                      DRM_FORMAT_MOD_LINEAR);
}

Bool
vaccum_get_formats(ScreenPtr screen, CARD32 *num_formats, CARD32 **formats)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    VkDrmFormatModifierPropertiesListEXT modifier_list = {};
    VkFormatProperties2 props2 = {};
    CARD32 count = 0;
    CARD32 *result_formats;

    *num_formats = 0;
    *formats = NULL;

    if (!vaccum_priv)
        return TRUE;

    for (int i = 0; vaccum_drm_format_map[i].drm_format != 0; i++) {
        modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        props2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        props2.pNext = &modifier_list;

        modifier_list.drmFormatModifierCount = 0;

        vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device,
                                              vaccum_drm_format_map[i].vk_format,
                                              &props2);

        if (modifier_list.drmFormatModifierCount == 0)
            continue;

        VkDrmFormatModifierPropertiesEXT *mod_props =
            calloc(modifier_list.drmFormatModifierCount,
                   sizeof(VkDrmFormatModifierPropertiesEXT));
        if (!mod_props)
            continue;

        modifier_list.pDrmFormatModifierProperties = mod_props;
        vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device,
                                             vaccum_drm_format_map[i].vk_format,
                                             &props2);

        for (uint32_t j = 0; j < modifier_list.drmFormatModifierCount; j++) {
            if (mod_props[j].drmFormatModifierTilingFeatures &
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
                count++;
                break;
            }
        }

        free(mod_props);
    }

    if (count == 0)
        return TRUE;

    result_formats = calloc(count, sizeof(CARD32));
    if (!result_formats)
        return FALSE;

    CARD32 idx = 0;
    for (int i = 0; vaccum_drm_format_map[i].drm_format != 0; i++) {
        modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        props2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        props2.pNext = &modifier_list;
        modifier_list.drmFormatModifierCount = 0;

        vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device,
                                              vaccum_drm_format_map[i].vk_format,
                                              &props2);

        if (modifier_list.drmFormatModifierCount == 0)
            continue;

        VkDrmFormatModifierPropertiesEXT *mod_props =
            calloc(modifier_list.drmFormatModifierCount,
                   sizeof(VkDrmFormatModifierPropertiesEXT));
        if (!mod_props)
            continue;

        modifier_list.pDrmFormatModifierProperties = mod_props;
        vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device,
                                             vaccum_drm_format_map[i].vk_format,
                                             &props2);

        Bool found = FALSE;
        for (uint32_t j = 0; j < modifier_list.drmFormatModifierCount; j++) {
            if (mod_props[j].drmFormatModifierTilingFeatures &
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
                found = TRUE;
                break;
            }
        }

        if (found)
            result_formats[idx++] = vaccum_drm_format_map[i].drm_format;

        free(mod_props);
    }

    *num_formats = idx;
    *formats = result_formats;
    return TRUE;
}

Bool
vaccum_get_modifiers(ScreenPtr screen, uint32_t format,
                     uint32_t *num_modifiers, uint64_t **modifiers)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    VkFormat vk_format;
    VkDrmFormatModifierPropertiesListEXT modifier_list = {};
    VkFormatProperties2 props2 = {};
    uint32_t count = 0;

    *num_modifiers = 0;
    *modifiers = NULL;

    if (!vaccum_priv)
        return TRUE;

    vk_format = vaccum_vk_format_for_drm(format);
    if (vk_format == VK_FORMAT_UNDEFINED)
        return TRUE;

    modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    props2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props2.pNext = &modifier_list;
    modifier_list.drmFormatModifierCount = 0;

    vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device,
                                         vk_format, &props2);

    if (modifier_list.drmFormatModifierCount == 0)
        return TRUE;

    VkDrmFormatModifierPropertiesEXT *mod_props =
        calloc(modifier_list.drmFormatModifierCount,
               sizeof(VkDrmFormatModifierPropertiesEXT));
    if (!mod_props)
        return FALSE;

    modifier_list.pDrmFormatModifierProperties = mod_props;
    vkGetPhysicalDeviceFormatProperties2(vaccum_priv->phys_device,
                                          vk_format, &props2);

    for (uint32_t i = 0; i < modifier_list.drmFormatModifierCount; i++) {
        if (mod_props[i].drmFormatModifierTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            count++;
    }

    if (count == 0) {
        free(mod_props);
        return TRUE;
    }

    uint64_t *result_modifiers = calloc(count, sizeof(uint64_t));
    if (!result_modifiers) {
        free(mod_props);
        return FALSE;
    }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < modifier_list.drmFormatModifierCount; i++) {
        if (mod_props[i].drmFormatModifierTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            result_modifiers[idx++] = mod_props[i].drmFormatModifier;
    }

    free(mod_props);

    *num_modifiers = idx;
    *modifiers = result_modifiers;
    return TRUE;
}

Bool
vaccum_get_drawable_modifiers(DrawablePtr draw, uint32_t format,
                              uint32_t *num_modifiers, uint64_t **modifiers)
{
    vaccum_screen_private *vaccum_priv =
        vaccum_get_screen_private(draw->pScreen);

    if (vaccum_priv && vaccum_priv->modifiers_func) {
        return vaccum_priv->modifiers_func(draw, format,
                                           num_modifiers, modifiers);
    }

    *num_modifiers = 0;
    *modifiers = NULL;
    return TRUE;
}

struct gbm_device *
vaccum_egl_get_gbm_device(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    vaccum_vk_screen_private *vk_priv = 
        (vaccum_vk_screen_private *)scrn->privates[xf86VaccumVKPrivateIndex].ptr;
    
    if (!vk_priv || vk_priv->drm_fd < 0)
        return NULL;
    
    if (!vk_priv->gbm)
        vk_priv->gbm = gbm_create_device(vk_priv->drm_fd);
    
    return vk_priv->gbm;
}

struct gbm_bo *
vaccum_gbm_bo_from_pixmap(ScreenPtr screen, PixmapPtr pixmap)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    vaccum_pixmap_private *pixmap_priv;
    struct vaccum_image *image;
    ScrnInfoPtr scrn;
    vaccum_vk_screen_private *vk_priv;
    struct gbm_device *gbm;
    VkMemoryGetFdInfoKHR get_fd_info;
    VkResult result;
    int fd;
    uint32_t gbm_format;
    struct gbm_import_fd_data import_data;
    struct gbm_bo *bo;
    const struct vaccum_format *f;
    
    if (!vaccum_priv)
        return NULL;
    
    pixmap_priv = vaccum_get_pixmap_private(pixmap);
    if (!pixmap_priv || !pixmap_priv->image)
        return NULL;
    
    image = pixmap_priv->image;
    
    scrn = xf86ScreenToScrn(screen);
    vk_priv = (vaccum_vk_screen_private *)scrn->privates[xf86VaccumVKPrivateIndex].ptr;
    if (!vk_priv)
        return NULL;
    
    gbm = vaccum_egl_get_gbm_device(screen);
    if (!gbm)
        return NULL;
    
    f = vaccum_format_for_pixmap(pixmap);
    if (!f || !f->format)
        return NULL;
    
    /* Get DMA-BUF fd from the Vulkan image */
    get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get_fd_info.memory = image->memories[0];
    get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    
    result = vkGetMemoryFdKHR(vaccum_priv->device, &get_fd_info, &fd);
    if (result != VK_SUCCESS)
        return NULL;
    
    /* Map DRM format to GBM format */
    gbm_format = vaccum_gbm_format_for_drm(f->render_format);
    if (!gbm_format) {
        close(fd);
        return NULL;
    }
    
    /* Import as GBM BO */
    import_data.fd = fd;
    import_data.width = pixmap->drawable.width;
    import_data.height = pixmap->drawable.height;
    import_data.stride = pixmap->devKind;
    import_data.format = gbm_format;
    
    bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &import_data, 0);
    close(fd);
    
    return bo;
}

Bool
vaccum_egl_create_textured_pixmap_from_gbm_bo(PixmapPtr pixmap,
                                              struct gbm_bo *bo, Bool used_modifiers)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(pixmap->drawable.pScreen);
    ScrnInfoPtr scrn;
    vaccum_vk_screen_private *vk_priv;
    struct gbm_device *gbm;
    int fd;
    uint32_t width, height, stride;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    const struct vaccum_format *f;
    
    if (!vaccum_priv || !bo)
        return FALSE;
    
    scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
    vk_priv = (vaccum_vk_screen_private *)scrn->privates[xf86VaccumVKPrivateIndex].ptr;
    if (!vk_priv)
        return FALSE;
    
    gbm = vaccum_egl_get_gbm_device(pixmap->drawable.pScreen);
    if (!gbm)
        return FALSE;
    
    /* Get DMA-BUF fd from GBM BO */
    fd = gbm_bo_get_fd(bo);
    if (fd < 0)
        return FALSE;
    
    width = gbm_bo_get_width(bo);
    height = gbm_bo_get_height(bo);
    stride = gbm_bo_get_stride(bo);
    
    if (used_modifiers)
        modifier = gbm_bo_get_modifier(bo);
    
    /* Get the DRM format from the GBM BO */
    f = vaccum_format_for_pixmap(pixmap);
    if (!f || !f->format) {
        close(fd);
        return FALSE;
    }
    
    /* Finally, import the GBM BO into VACCUM */
    Bool result = vaccum_import_fd_to_pixmap(pixmap, fd, width, height,
                                             stride, pixmap->drawable.depth,
                                             pixmap->drawable.bitsPerPixel,
                                             modifier);

    close(fd);
    return result;
}

#ifdef DRI3
static int
vaccum_dri3_open_client(ClientPtr client, ScreenPtr screen,
                        RRProviderPtr provider, int *fdp)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    int fd;
    drm_magic_t magic;

    if (!vaccum_priv)
        return BadAlloc;

    if (!vaccum_priv->drm_device_path)
        return BadImplementation;

    fd = open(vaccum_priv->drm_device_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return BadAlloc;

    if (drmGetMagic(fd, &magic) < 0) {
        if (errno == EACCES) {
            *fdp = fd;
            return Success;
        }
        close(fd);
        return BadMatch;
    }

    if (drmAuthMagic(vaccum_priv->drm_fd, magic) < 0) {
        close(fd);
        return BadMatch;
    }

    *fdp = fd;
    return Success;
}

static const dri3_screen_info_rec vaccum_dri3_info = {
    .version = 4,

    .open = NULL,
    /* XXX: These aren't really needed as we 
     * mandate proper DMA-BUF support, remove these. */
    .pixmap_from_fd = vaccum_pixmap_from_fd,
    .fd_from_pixmap = vaccum_fd_from_pixmap,

    .open_client = vaccum_dri3_open_client,

    .pixmap_from_fds = vaccum_pixmap_from_fds,
    .fds_from_pixmap = vaccum_fds_from_pixmap,
    .get_formats = vaccum_get_formats,
    .get_modifiers = vaccum_get_modifiers,
    .get_drawable_modifiers = vaccum_get_drawable_modifiers,

    .import_syncobj = vaccum_import_syncobj,
};

Bool
vaccum_dri3_screen_init(ScreenPtr screen)
{
    return dri3_screen_init(screen, &vaccum_dri3_info);
}
#endif
