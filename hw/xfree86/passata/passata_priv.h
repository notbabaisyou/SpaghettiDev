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

#ifndef PASSATA_PRIV_H
#define PASSATA_PRIV_H

#include "passata.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <xf86.h>
#include "exa.h"
#include "privates.h"

#define PASSATA_LOG_PREFIX "passata"

typedef struct _passata_screen_priv {
    ScrnInfoPtr  scrn;
    int          fd;
    GLint        max_texture_size;
    ExaDriverPtr exa;
 
    EGLDisplay   display;
    EGLContext   context;
 
    Bool         has_fbo;
    Bool         has_shaders;
    Bool         has_npot;
    Bool         has_texture_swizzle;
    Bool         has_texture_barrier;
    Bool         has_dma_buf_export;
    Bool         has_dma_buf_modifiers;
} passata_screen_priv;

typedef struct {
    void *ptr;   /* pixel data */
    int   pitch; /* row stride in bytes */
    Bool  owned; /* FALSE if ptr is owned externally (e.g. screen pixmap) */
} passata_pixmap_priv;

passata_screen_priv *passata_get_screen_priv(ScreenPtr pScreen);

Bool passata_egl_init(ScrnInfoPtr scrn, int fd);
void passata_egl_fini(ScrnInfoPtr scrn);

#endif /* PASSATA_PRIV_H */
