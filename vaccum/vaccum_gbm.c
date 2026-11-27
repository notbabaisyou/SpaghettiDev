#include "dix-config.h"
#include "vaccum_gbm_priv.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

static uint32_t
pixel_format(unsigned depth)
{
    switch (depth) {
    case 16:
        return GBM_FORMAT_RGB565;
    case 24:
        return GBM_FORMAT_XRGB8888;
    case 30:
        return GBM_FORMAT_XRGB2101010;
    case 32:
        return GBM_FORMAT_ARGB8888;
    default:
        return 0;
    }
}

void
vaccum_release_backing(PixmapPtr pixmap)
{
    struct vaccum_gbm_pixmap *priv = pixmap_data(pixmap);

    if (priv->pixels.buffer)
        vaccum_linear_release(screen_data(pixmap->drawable.pScreen)->gpu,
                              &priv->pixels);
    if (priv->map)
        munmap(priv->map, priv->length);
    if (priv->bo)
        gbm_bo_destroy(priv->bo);
    *priv = (struct vaccum_gbm_pixmap){0};
}

/* Take ownership on success. The mapping and BO always describe the same
 * linear storage, including after conversion of a Vulkan-backed pixmap. */
static Bool
attach_bo(PixmapPtr pixmap, struct gbm_bo *bo, Bool copy)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    struct vaccum_gbm_pixmap *priv = pixmap_data(pixmap);
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    size_t row = (size_t)width * pixmap->drawable.bitsPerPixel / 8;
    Bool linear = modifier == DRM_FORMAT_MOD_LINEAR ||
                  modifier == DRM_FORMAT_MOD_INVALID;

    if (!linear || gbm_bo_get_plane_count(bo) != 1)
        return FALSE;
    if (!width || !height || stride < row)
        return FALSE;
    if (stride > INT_MAX || gbm_bo_get_offset(bo, 0))
        return FALSE;
    if (copy && (!pixmap->devPrivate.ptr || pixmap->devKind < (int)row))
        return FALSE;

    int fd = gbm_bo_get_fd(bo);

    if (fd < 0)
        return FALSE;

    off_t size = lseek(fd, 0, SEEK_END);
    uint64_t needed = (uint64_t)stride * height;

    if (size < 0 || needed > (uint64_t)size || needed > SIZE_MAX) {
        close(fd);
        return FALSE;
    }

    void *map = mmap(NULL, needed, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    close(fd);
    if (map == MAP_FAILED)
        return FALSE;
    if (copy) {
        for (unsigned y = 0; y < height; y++)
            memcpy((char *)map + (size_t)y * stride,
                   (char *)pixmap->devPrivate.ptr +
                       (size_t)y * pixmap->devKind,
                   row);
    }
    if (!screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, stride,
                                    map)) {
        munmap(map, needed);
        return FALSE;
    }
    vaccum_release_backing(pixmap);
    priv->bo = bo;
    priv->map = map;
    priv->length = needed;
    return TRUE;
}

static struct gbm_bo *
import_bo(ScreenPtr screen, int fd, unsigned width, unsigned height,
          unsigned stride, uint32_t format)
{
    struct gbm_import_fd_data data = {
        .fd = fd,
        .width = width,
        .height = height,
        .stride = stride,
        .format = format,
    };

    return gbm_bo_import(screen_data(screen)->gbm, GBM_BO_IMPORT_FD, &data,
                         0);
}

Bool
vaccum_attach_front(PixmapPtr pixmap, struct gbm_bo *front)
{
    uint64_t modifier = gbm_bo_get_modifier(front);
    Bool linear = modifier == DRM_FORMAT_MOD_LINEAR ||
                  modifier == DRM_FORMAT_MOD_INVALID;

    if (!linear || gbm_bo_get_plane_count(front) != 1)
        return FALSE;
    if (gbm_bo_get_offset(front, 0))
        return FALSE;

    int fd = gbm_bo_get_fd(front);

    if (fd < 0)
        return FALSE;

    struct gbm_bo *bo =
        import_bo(pixmap->drawable.pScreen, fd, gbm_bo_get_width(front),
                  gbm_bo_get_height(front), gbm_bo_get_stride(front),
                  gbm_bo_get_format(front));

    close(fd);
    if (!bo)
        return FALSE;
    if (attach_bo(pixmap, bo, FALSE))
        return TRUE;
    gbm_bo_destroy(bo);
    return FALSE;
}

static Bool
attach_fd(PixmapPtr pixmap, int fd, CARD16 width, CARD16 height,
                 CARD32 stride, CARD8 depth, CARD8 bpp)
{
    uint32_t format = pixel_format(depth);

    if (!format || depth != pixmap->drawable.depth ||
        bpp != pixmap->drawable.bitsPerPixel)
        return FALSE;

    struct gbm_bo *bo = import_bo(pixmap->drawable.pScreen, fd, width, height,
                                  stride, format);

    if (!bo)
        return FALSE;
    if (attach_bo(pixmap, bo, FALSE))
        return TRUE;
    gbm_bo_destroy(bo);
    return FALSE;
}

Bool
vaccum_attach_fd(PixmapPtr pixmap, int fd, CARD16 width, CARD16 height,
                 CARD16 stride, CARD8 depth, CARD8 bpp)
{
    return attach_fd(pixmap, fd, width, height, stride, depth, bpp);
}

PixmapPtr
vaccum_import_fds(ScreenPtr screen, CARD8 num, const int *fds, CARD16 width,
                  CARD16 height, const CARD32 *strides, const CARD32 *offsets,
                  CARD8 depth, CARD8 bpp, CARD64 modifier)
{
    Bool linear = modifier == DRM_FORMAT_MOD_LINEAR ||
                  modifier == DRM_FORMAT_MOD_INVALID;

    if (num != 1 || !linear)
        return NULL;
    if (offsets[0] || strides[0] > INT_MAX || !pixel_format(depth))
        return NULL;

    PixmapPtr pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);

    if (!pixmap)
        return NULL;
    if (attach_fd(pixmap, fds[0], width, height, strides[0], depth, bpp))
        return pixmap;
    screen->DestroyPixmap(pixmap);
    return NULL;
}

static struct gbm_bo *
export_bo(ScreenPtr screen, PixmapPtr pixmap)
{
    struct vaccum_gbm_pixmap *priv = pixmap_data(pixmap);
    uint32_t format = pixel_format(pixmap->drawable.depth);

    if (priv->bo && priv->map == pixmap->devPrivate.ptr)
        return priv->bo;
    if (!format || pixmap == screen->GetScreenPixmap(screen))
        return NULL;

    struct gbm_bo *bo =
        gbm_bo_create(screen_data(screen)->gbm, pixmap->drawable.width,
                      pixmap->drawable.height, format,
                      GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT);

    if (!bo)
        return NULL;
    if (attach_bo(pixmap, bo, TRUE))
        return bo;
    gbm_bo_destroy(bo);
    return NULL;
}

struct gbm_bo *
vaccum_scanout_bo(ScreenPtr screen, PixmapPtr pixmap)
{
    struct gbm_bo *bo = export_bo(screen, pixmap);

    if (!bo)
        return NULL;

    int fd = gbm_bo_get_fd(bo);

    if (fd < 0)
        return NULL;

    struct gbm_bo *reference =
        import_bo(screen, fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                  gbm_bo_get_stride(bo), gbm_bo_get_format(bo));

    close(fd);
    return reference;
}

int
vaccum_export_fd(ScreenPtr screen, PixmapPtr pixmap, CARD16 *stride,
                 CARD32 *size)
{
    struct gbm_bo *bo = export_bo(screen, pixmap);

    if (!bo || gbm_bo_get_stride(bo) > UINT16_MAX)
        return -1;
    *stride = gbm_bo_get_stride(bo);
    *size = (uint32_t)*stride * pixmap->drawable.height;
    return gbm_bo_get_fd(bo);
}

static PixmapPtr
import_fd(ScreenPtr screen, int fd, CARD16 width, CARD16 height,
          CARD16 stride, CARD8 depth, CARD8 bpp)
{
    CARD32 pitch = stride, offset = 0;

    return vaccum_import_fds(screen, 1, &fd, width, height, &pitch, &offset,
                             depth, bpp, DRM_FORMAT_MOD_INVALID);
}

static int
open_render(ScreenPtr screen, RRProviderPtr provider, int *out)
{
    char *path = drmGetRenderDeviceNameFromFd(screen_data(screen)->fd);

    if (!path)
        return BadMatch;
    *out = open(path, O_RDWR | O_CLOEXEC);
    free(path);
    return *out < 0 ? BadAlloc : Success;
}

static int
export_fds(ScreenPtr screen, PixmapPtr pixmap, int *fds, CARD32 *strides,
           CARD32 *offsets, CARD64 *modifier)
{
    struct gbm_bo *bo = export_bo(screen, pixmap);

    if (!bo)
        return 0;
    fds[0] = gbm_bo_get_fd(bo);
    if (fds[0] < 0)
        return 0;
    strides[0] = gbm_bo_get_stride(bo);
    offsets[0] = 0;
    *modifier = gbm_bo_get_modifier(bo);
    return 1;
}

static Bool
get_formats(ScreenPtr screen, CARD32 *count, CARD32 **out)
{
    static const CARD32 formats[] = {
        GBM_FORMAT_RGB565,
        GBM_FORMAT_XRGB8888,
        GBM_FORMAT_XRGB2101010,
        GBM_FORMAT_ARGB8888,
    };

    *out = malloc(sizeof(formats));
    if (!*out)
        return FALSE;
    memcpy(*out, formats, sizeof(formats));
    *count = ARRAY_SIZE(formats);
    return TRUE;
}

static Bool
get_modifiers(ScreenPtr screen, CARD32 format, CARD32 *count, CARD64 **out)
{
    *out = malloc(sizeof(**out));
    if (!*out)
        return FALSE;
    **out = DRM_FORMAT_MOD_LINEAR;
    *count = 1;
    return TRUE;
}

static Bool
drawable_modifiers(DrawablePtr draw, CARD32 format, CARD32 *count,
                   CARD64 **out)
{
    return get_modifiers(draw->pScreen, format, count, out);
}

const dri3_screen_info_rec vaccum_gbm_dri3 = {
    .version = 2,
    .open = open_render,
    .pixmap_from_fd = import_fd,
    .fd_from_pixmap = vaccum_export_fd,
    .pixmap_from_fds = vaccum_import_fds,
    .fds_from_pixmap = export_fds,
    .get_formats = get_formats,
    .get_modifiers = get_modifiers,
    .get_drawable_modifiers = drawable_modifiers,
};
