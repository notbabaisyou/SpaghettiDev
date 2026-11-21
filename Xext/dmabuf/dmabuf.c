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
#include <picturestr.h>
#include <pixmapstr.h>

#include "extnsionst.h"
#include "dmabuf.h"

static DevPrivateKeyRec dmabuf_screen_key;

static const struct dmabuf_backend *
screen_backend(ScreenPtr screen)
{
    if (!screen || !dixPrivateKeyRegistered(&dmabuf_screen_key))
        return NULL;

    return dixLookupPrivate(&screen->devPrivates, &dmabuf_screen_key);
}

Bool
dmabuf_register(ScreenPtr screen,
                const struct dmabuf_backend *backend)
{
    const struct dmabuf_backend *registered;

    if (!screen || !backend || !backend->get_modifiers || !backend->import_buffer ||
        !dixRegisterPrivateKey(&dmabuf_screen_key, PRIVATE_SCREEN, 0))
        return FALSE;

    registered = screen_backend(screen);
    if (registered && registered != backend)
        return FALSE;

    dixSetPrivate(&screen->devPrivates, &dmabuf_screen_key, (void *) backend);
    return TRUE;
}

void
dmabuf_unregister(ScreenPtr screen,
                  const struct dmabuf_backend *backend)
{
    if (backend && screen_backend(screen) == backend)
        dixSetPrivate(&screen->devPrivates, &dmabuf_screen_key, NULL);
}

Bool
dmabuf_has_backend(ScreenPtr screen)
{
    return screen_backend(screen) != NULL;
}

int
dmabuf_get_modifiers(ScreenPtr screen, uint32_t fourcc,
                     uint32_t *count, uint64_t **modifiers)
{
    const struct dmabuf_backend *backend = screen_backend(screen);

    if (!count || !modifiers)
        return BadValue;

    *count = 0;
    *modifiers = NULL;

    if (!fourcc)
        return BadValue;

    if (!backend)
        return BadMatch;

    return backend->get_modifiers(screen, fourcc, count, modifiers);
}

int
dmabuf_import(ScreenPtr screen, const struct dmabuf_buffer *buffer,
              PixmapPtr *pixmap, PictFormatPtr *source_format)
{
    const struct dmabuf_backend *backend;
    PictFormatPtr picture_format = NULL;
    PixmapPtr imported = NULL;
    DrawablePtr drawable;
    int ret;

    if (!pixmap || !source_format)
        return BadValue;

    *pixmap = NULL;
    *source_format = NULL;

    if (!buffer || !buffer->fourcc)
        return BadValue;

    if (!buffer->width || !buffer->height ||
         buffer->width > INT16_MAX || buffer->height > INT16_MAX)
        return BadValue;

    if (!buffer->num_planes || buffer->num_planes > DMABUF_MAX_PLANES)
        return BadValue;

    if (!buffer->fds || !buffer->strides || !buffer->offsets)
        return BadValue;

    for (unsigned int i = 0; i < buffer->num_planes; i++) {
        if (buffer->fds[i] < 0 || !buffer->strides[i])
            return BadValue;
    }

    backend = screen_backend(screen);
    if (!backend)
        return BadMatch;

    ret = backend->import_buffer(screen, buffer, &imported, &picture_format);
    if (ret != Success)
        return ret;
    if (!imported)
        return BadMatch;

    drawable = &imported->drawable;
    if (!picture_format || drawable->pScreen != screen ||
        drawable->width != buffer->width || drawable->height != buffer->height ||
        drawable->depth != picture_format->depth) {
        drawable->pScreen->DestroyPixmap(imported);
        return BadMatch;
    }

    *pixmap = imported;
    *source_format = picture_format;
    return Success;
}

static int
ProcDMABUFDispatch(ClientPtr client)
{
    return BadRequest;
}

static int
SProcDMABUFDispatch(ClientPtr client)
{
    return BadRequest;
}

void
dmabuf_extension_init(void)
{
    ExtensionEntry *extension;

    extension = AddExtension("SPAGHETTI-DMABUF", 0, 0,
                             ProcDMABUFDispatch, SProcDMABUFDispatch,
                             NULL, StandardMinorOpcode);
    if (!extension)
        FatalError("Cannot initialize the SPAGHETTI-DMABUF extension");
}
