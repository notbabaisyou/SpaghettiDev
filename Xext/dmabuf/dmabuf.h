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
#ifndef DMABUF_H
#define DMABUF_H

#ifdef SPAGHETTI
#include <stdint.h>
#include <picture.h>

#define DMABUF_MAX_PLANES 4

struct dmabuf_buffer {
    uint32_t fourcc;
    uint16_t width, height;
    uint64_t modifier;
    uint8_t num_planes;
    const int *fds;
    const uint32_t *strides;
    const uint32_t *offsets;
};

/* Callbacks return X status codes. A successful query returns an owned
 * modifier array, or an empty list. On failure, leave the outputs empty.
 * The import callback validates the format, modifier and plane layout.
 */
struct dmabuf_backend {
    int (*get_modifiers)(ScreenPtr screen, uint32_t fourcc,
                         uint32_t *count, uint64_t **modifiers);
    int (*import_buffer)(ScreenPtr screen, const struct dmabuf_buffer *buffer,
                         PixmapPtr *pixmap, PictFormatPtr *source_format);
};

/* Register both callbacks before exposing capabilities. The table is borrowed
 * and must remain valid until unregistered or the screen is destroyed.
 */
extern _X_EXPORT Bool
dmabuf_register(ScreenPtr screen,
                const struct dmabuf_backend *backend);

extern _X_EXPORT void
dmabuf_unregister(ScreenPtr screen,
                  const struct dmabuf_backend *backend);

/* Returns modifiers usable for import and RENDER sampling. The caller frees
 * the array with free(). A screen without a backend returns BadMatch.
 */
extern _X_EXPORT int
dmabuf_get_modifiers(ScreenPtr screen, uint32_t fourcc,
                     uint32_t *count, uint64_t **modifiers);

/* The input description and FDs are borrowed. On success, the caller owns
 * one pixmap reference. The returned RENDER format belongs to the screen.
 */
extern _X_EXPORT int
dmabuf_import(ScreenPtr screen, const struct dmabuf_buffer *buffer,
              PixmapPtr *pixmap, PictFormatPtr *source_format);
#endif

#endif
