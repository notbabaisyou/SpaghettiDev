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
#include "picturestr.h"
#include "windowstr.h"

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
    Bool         has_texture_rg;
    Bool         has_texture_swizzle;
    Bool         has_texture_barrier;
    Bool         has_dma_buf_export;
    Bool         has_dma_buf_modifiers;

    /* Per-operation state set in Prepare* and consumed in the operation hooks */
    struct {
        struct {
            PixmapPtr pDstPixmap;
            GLfloat   color[4];
        } solid;
        struct {
            PixmapPtr pSrcPixmap;
            PixmapPtr pDstPixmap;
        } copy;
        struct {
            int        op;
            PixmapPtr  pSrc;
        } composite;
    } prepare_args;
} passata_screen_priv;

typedef struct _passata_pixmap_priv {
    GLuint  tex;         /* GL texture name; 0 if no GPU copy (e.g. 1bpp) */
    GLuint  fbo;         /* FBO with tex attached; 0 if format not renderable */
    void   *sys_copy;    /* CPU-accessible buffer; NULL if zero-size pixmap */
    int     pitch;       /* row stride in bytes */
    Bool    gpu_valid;   /* tex has up-to-date data */
    Bool    cpu_valid;   /* sys_copy has up-to-date data */
    Bool    is_external; /* sys_copy is owned externally — do not free */
} passata_pixmap_priv;

passata_screen_priv *passata_get_screen_priv(ScreenPtr pScreen);

Bool passata_egl_init(ScrnInfoPtr scrn, int fd);
void passata_egl_fini(ScrnInfoPtr scrn);

void passata_upload_to_gl(PixmapPtr pPixmap, passata_pixmap_priv *priv);
void passata_download_from_gl(PixmapPtr pPixmap, passata_pixmap_priv *priv);

Bool passata_upload_to_screen(PixmapPtr pDst,
                              int x, int y, int w, int h,
                              char *src, int src_pitch);
Bool passata_download_from_screen(PixmapPtr pSrc,
                                  int x, int y, int w, int h,
                                  char *dst, int dst_pitch);
 
Bool passata_prepare_solid(PixmapPtr pPixmap, int alu, 
                           Pixel planemask, Pixel fg);

void passata_solid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2);
void passata_done_solid(PixmapPtr pPixmap);
 
Bool passata_prepare_copy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap,
                          int dx, int dy, int alu, Pixel planemask);

void passata_copy(PixmapPtr pDstPixmap, int srcX, int srcY,
                  int dstX, int dstY, int width, int height);

void passata_done_copy(PixmapPtr pDstPixmap);
 
Bool passata_check_composite(int op, PicturePtr pSrcPicture,
                             PicturePtr pMaskPicture,
                             PicturePtr pDstPicture);

Bool passata_prepare_composite(int op, PicturePtr pSrcPicture,
                               PicturePtr pMaskPicture,
                               PicturePtr pDstPicture,
                               PixmapPtr pSrc, PixmapPtr pMask,
                               PixmapPtr pDst);

void passata_composite(PixmapPtr pDst, int srcX, int srcY,
                       int maskX, int maskY, int dstX, int dstY,
                       int width, int height);

void passata_done_composite(PixmapPtr pDst);

void *passata_create_pixmap2(ScreenPtr pScreen, int width, int height,
                             int depth, int usage_hint, int bitsPerPixel,
                             int *new_fb_pitch);

void passata_destroy_pixmap(ScreenPtr pScreen, void *driverPriv);

Bool passata_modify_pixmap_header(PixmapPtr pPixmap, int width, int height,
                                  int depth, int bitsPerPixel, int devKind,
                                  void *pPixData);

Bool passata_pixmap_is_offscreen(PixmapPtr pPixmap);
Bool passata_prepare_access(PixmapPtr pPix, int index);
void passata_finish_access(PixmapPtr pPix, int index);

#endif /* PASSATA_PRIV_H */
