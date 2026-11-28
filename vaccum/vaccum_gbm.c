#include "dix-config.h"
#include "vaccum_gbm_priv.h"
#include "os/xserver_poll.h"

#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#endif

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

struct vaccum_dmabuf {
    struct xorg_list link;
    dev_t device;
    ino_t inode;
    int fd;
    unsigned refs;
    unsigned cpu_users;
    Bool cpu_write;
};

/* Consume fd on every path. Aliases share one reservation and CPU scope. */
static struct vaccum_dmabuf *
ref_dmabuf(struct vaccum_gbm_screen *screen, int fd)
{
    struct stat stat;
    struct vaccum_dmabuf *buffer = NULL;

    if (fstat(fd, &stat) < 0)
        goto close_fd;

    xorg_list_for_each_entry(buffer, screen->dmabufs, link) {
        Bool same_inode = buffer->device == stat.st_dev &&
                          buffer->inode == stat.st_ino;

        if (same_inode) {
            buffer->refs++;
            goto close_fd;
        }
    }

    buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        goto close_fd;
    buffer->device = stat.st_dev;
    buffer->inode = stat.st_ino;
    buffer->fd = fd;
    buffer->refs = 1;
    xorg_list_add(&buffer->link, screen->dmabufs);
    return buffer;

close_fd:
    close(fd);
    return buffer;
}

static void
unref_dmabuf(struct vaccum_dmabuf *buffer)
{
    if (--buffer->refs)
        return;

    assert(!buffer->cpu_users);
    xorg_list_del(&buffer->link);
    close(buffer->fd);
    free(buffer);
}

static Bool
sync_dmabuf(struct vaccum_dmabuf *buffer, Bool start, Bool write)
{
#ifdef __linux__
    struct dma_buf_sync sync = {
        .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                 (write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ),
    };

    int status = 0;

    do {
        status = ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (status < 0 && (errno == EINTR || errno == EAGAIN));

    return status == 0;
#else
    return TRUE;
#endif
}

static Bool
begin_dmabuf(struct vaccum_dmabuf *buffer, Bool write)
{
    if (buffer->cpu_users && (!write || buffer->cpu_write)) {
        buffer->cpu_users++;
        return TRUE;
    }

    /* A destination may alias an already prepared source. */
    if (buffer->cpu_users && !sync_dmabuf(buffer, FALSE, FALSE))
        return FALSE;

    struct pollfd fence = {
        .fd = buffer->fd,
        .events = write ? POLLOUT : POLLIN,
    };

    int status = 0;

    do {
        status = xserver_poll(&fence, 1, -1);
    } while (status < 0 && errno == EINTR);

    if (status < 0)
        return FALSE;
    if (!(fence.revents & fence.events)) {
        errno = EIO;
        return FALSE;
    }
    if (!sync_dmabuf(buffer, TRUE, write))
        return FALSE;

    buffer->cpu_write = write;
    buffer->cpu_users++;
    return TRUE;
}

static Bool
end_dmabuf(struct vaccum_dmabuf *buffer)
{
    assert(buffer->cpu_users);
    if (--buffer->cpu_users)
        return TRUE;
    return sync_dmabuf(buffer, FALSE, buffer->cpu_write);
}

void
vaccum_cpu_access(PixmapPtr pixmap, FbPixmapAccess access)
{
    struct vaccum_gbm_pixmap *priv = pixmap_data(pixmap);

    if (!priv->dmabuf || priv->map != pixmap->devPrivate.ptr)
        return;

    Bool synced = access == FB_PIXMAP_ACCESS_FINISH ?
        end_dmabuf(priv->dmabuf) :
        begin_dmabuf(priv->dmabuf, access == FB_PIXMAP_ACCESS_WRITE);

    if (!synced)
        FatalError("vaccum: failed to synchronize CPU dma-buf access (%s)\n",
                   strerror(errno));
}

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

    if (priv->pixels.map)
        vaccum_linear_release(screen_data(pixmap->drawable.pScreen)->gpu,
                              &priv->pixels);
    if (priv->map)
        munmap(priv->map, priv->length);
    if (priv->bo)
        gbm_bo_destroy(priv->bo);
    if (priv->dmabuf)
        unref_dmabuf(priv->dmabuf);
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

    if (map == MAP_FAILED) {
        close(fd);
        return FALSE;
    }

    struct vaccum_dmabuf *dmabuf = ref_dmabuf(screen_data(screen), fd);

    if (!dmabuf)
        goto unmap;
    if (copy) {
        if (!begin_dmabuf(dmabuf, TRUE))
            goto release_dmabuf;

        fbPreparePixmapAccess(pixmap);
        vaccum_cpu_access(pixmap, FB_PIXMAP_ACCESS_READ);

        for (unsigned y = 0; y < height; y++)
            memcpy((char *)map + (size_t)y * stride,
                   (char *)pixmap->devPrivate.ptr +
                       (size_t)y * pixmap->devKind,
                   row);

        vaccum_cpu_access(pixmap, FB_PIXMAP_ACCESS_FINISH);
        if (!end_dmabuf(dmabuf))
            goto release_dmabuf;
    }
    if (!screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, stride, map))
        goto release_dmabuf;

    vaccum_release_backing(pixmap);
    priv->bo = bo;
    priv->dmabuf = dmabuf;
    priv->map = map;
    priv->length = needed;
    return TRUE;

release_dmabuf:
    unref_dmabuf(dmabuf);
unmap:
    munmap(map, needed);
    return FALSE;
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

    int fd = pixmap_data(pixmap)->dmabuf->fd;

    return import_bo(screen, fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                     gbm_bo_get_stride(bo), gbm_bo_get_format(bo));
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
    return fcntl(pixmap_data(pixmap)->dmabuf->fd, F_DUPFD_CLOEXEC, 0);
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
    fds[0] = fcntl(pixmap_data(pixmap)->dmabuf->fd, F_DUPFD_CLOEXEC, 0);
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
