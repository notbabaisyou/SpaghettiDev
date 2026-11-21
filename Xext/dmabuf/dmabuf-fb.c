/**
 * Spaghetti Display Server
 * Copyright (C) 2026  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <dix-config.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dmabuf-fb.h"
#include "os/busfault.h"
#include "picturestr.h"
#include "pixmapstr.h"
#include "servermd.h"

#define DMABUF_FB_FENCE_TIMEOUT_MS 1500

static PictFormatPtr
dmabuf_fb_format(ScreenPtr screen, uint32_t fourcc)
{
    CARD32 format;
    int depth;

    if (IMAGE_BYTE_ORDER != LSBFirst)
        return NULL;

    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
        format = PICT_x8r8g8b8;
        break;
    case DRM_FORMAT_ARGB8888:
        format = PICT_a8r8g8b8;
        break;
    case DRM_FORMAT_XBGR8888:
        format = PICT_x8b8g8r8;
        break;
    case DRM_FORMAT_ABGR8888:
        format = PICT_a8b8g8r8;
        break;
    case DRM_FORMAT_BGRX8888:
        format = PICT_b8g8r8x8;
        break;
    case DRM_FORMAT_BGRA8888:
        format = PICT_b8g8r8a8;
        break;
    case DRM_FORMAT_RGBA8888:
        format = PICT_r8g8b8a8;
        break;
    case DRM_FORMAT_RGB565:
        format = PICT_r5g6b5;
        break;
    default:
        return NULL;
    }

    depth = PIXMAN_FORMAT_DEPTH(format);
    if (BitsPerPixel(depth) != PICT_FORMAT_BPP(format))
        return NULL;

    return PictureMatchFormat(screen, depth, format);
}

static size_t
dmabuf_fb_extent(const struct dmabuf_buffer *buffer, size_t row_bytes)
{
    uint64_t end;
    off_t length;

    if (buffer->modifier != DRM_FORMAT_MOD_LINEAR || buffer->num_planes != 1 ||
        !buffer->height || !buffer->fds || !buffer->offsets || !buffer->strides ||
        buffer->strides[0] < row_bytes)
        return 0;

    end = (uint64_t) buffer->offsets[0] +
          (uint64_t) (buffer->height - 1) * buffer->strides[0] + row_bytes;
    if (end > SIZE_MAX)
        return 0;

    length = lseek(buffer->fds[0], 0, SEEK_END);
    if (length < 0)
        return 0;

    lseek(buffer->fds[0], 0, SEEK_SET);
    return end <= (uint64_t) length ? end : 0;
}

static int
dmabuf_fb_get_modifiers(ScreenPtr screen, uint32_t fourcc,
                        uint32_t *count, uint64_t **modifiers)
{
    if (!dmabuf_fb_format(screen, fourcc))
        return BadMatch;

    *modifiers = malloc(sizeof(**modifiers));
    if (!*modifiers)
        return BadAlloc;

    **modifiers = DRM_FORMAT_MOD_LINEAR;
    *count = 1;
    return Success;
}

static int
dmabuf_fb_import(ScreenPtr screen, const struct dmabuf_buffer *buffer,
                  PixmapPtr *pixmap, PictFormatPtr *source_format)
{
    PictFormatPtr format = dmabuf_fb_format(screen, buffer->fourcc);
    PixmapPtr imported;

    if (!format || !dmabuf_fb_extent(buffer, buffer->width *
                                     (BitsPerPixel(format->depth) / CHAR_BIT)))
        return BadMatch;

    /* Soft2D's screen hook allocates CPU-backed pixmaps. */
    imported = screen->CreatePixmap(screen, buffer->width, buffer->height,
                                    format->depth, 0);
    if (!imported)
        return BadAlloc;

    *pixmap = imported;
    *source_format = format;
    return Success;
}

static Bool
dmabuf_fb_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags | DMA_BUF_SYNC_READ };
    int ret;

    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));

    return ret == 0;
}

static Bool
dmabuf_fb_copy(const struct dmabuf_buffer *buffer, PixmapPtr pixmap,
                const char *mapping, size_t size, size_t row_bytes)
{
    struct busfault *guard =
        busfault_register_mmap((void *) mapping, size, NULL, NULL);
    sigjmp_buf jump;

    if (!guard)
        return FALSE;

    if (sigsetjmp(jump, 1) != 0) {
        busfault_unregister(guard);
        return FALSE;
    }

    busfault_set_jmp(guard, &jump);

    const char *src = mapping + buffer->offsets[0];
    char *dst = pixmap->devPrivate.ptr;
    for (unsigned int row = 0; row < buffer->height; row++)
        memcpy(dst + (size_t) row * pixmap->devKind,
               src + (size_t) row * buffer->strides[0], row_bytes);

    busfault_unregister(guard);
    return TRUE;
}

int
dmabuf_fb_prepare(ScreenPtr screen, const struct dmabuf_buffer *buffer,
                   PixmapPtr pixmap)
{
    size_t row_bytes = (size_t)buffer->width *
                       (pixmap->drawable.bitsPerPixel / CHAR_BIT);
    size_t size = dmabuf_fb_extent(buffer, row_bytes);
    void *mapping;
    int fd;
    Bool copied, synchronized;

    if (!size || !pixmap->devPrivate.ptr || pixmap->devKind < row_bytes)
        return BadMatch;

    fd = buffer->fds[0];
    struct pollfd ready = { .fd = fd, .events = POLLIN };
    int ret;

    /* POLLIN waits for producer writes. The SYNC ioctl handles CPU cache
     * coherency for the read that follows.
     */
    do {
        ret = poll(&ready, 1, DMABUF_FB_FENCE_TIMEOUT_MS);
    } while (ret < 0 && errno == EINTR);
    if (ret <= 0 || !(ready.revents & POLLIN) ||
        (ready.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return BadMatch;

    mapping = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return BadMatch;

    if (!dmabuf_fb_sync(fd, DMA_BUF_SYNC_START)) {
        munmap(mapping, size);
        return BadMatch;
    }

    copied = dmabuf_fb_copy(buffer, pixmap, mapping, size, row_bytes);
    synchronized = dmabuf_fb_sync(fd, DMA_BUF_SYNC_END);
    munmap(mapping, size);

    return copied && synchronized ? Success : BadMatch;
}

const struct dmabuf_backend dmabuf_fb_backend = {
    .get_modifiers = dmabuf_fb_get_modifiers,
    .import_buffer = dmabuf_fb_import,
};
