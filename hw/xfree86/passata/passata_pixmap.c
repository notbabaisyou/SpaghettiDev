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

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include "passata_priv.h"

static Bool
passata_get_gl_format(passata_screen_priv *screen_priv,
                      int depth, int bitsPerPixel,
                      GLenum *format, GLenum *type, GLenum *internal)
{
    switch (bitsPerPixel) {
    case 32:
        if (depth == 30) {
            *format   = GL_BGRA;
            *type     = GL_UNSIGNED_INT_2_10_10_10_REV;
            *internal = GL_RGB10_A2;
        } else {
            *format   = GL_BGRA;
            *type     = GL_UNSIGNED_BYTE;
            *internal = GL_RGBA8;
        }
        return TRUE;
    case 16:
        if (depth == 15) {
            *format   = GL_BGRA;
            *type     = GL_UNSIGNED_SHORT_1_5_5_5_REV;
            *internal = GL_RGB5_A1;
        } else {
            *format   = GL_RGB;
            *type     = GL_UNSIGNED_SHORT_5_6_5;
            *internal = GL_RGB565;
        }
        return TRUE;
    case 8:
        if (screen_priv->has_texture_rg && screen_priv->has_texture_swizzle) {
            *format   = GL_RED;
            *type     = GL_UNSIGNED_BYTE;
            *internal = GL_R8;
        } else {
            *format   = GL_ALPHA;
            *type     = GL_UNSIGNED_BYTE;
            *internal = GL_ALPHA8;
        }
        return TRUE;
    default:
        return FALSE;
    }
}

static void
passata_set_texture_swizzle(passata_screen_priv *screen_priv,
                            int depth, int bitsPerPixel, GLenum format)
{
    if (!screen_priv->has_texture_swizzle)
        return;
 
    if (bitsPerPixel == 8 && format == GL_RED) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ZERO);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    } else if ((bitsPerPixel == 16 && depth == 15) ||
               (bitsPerPixel == 32 && depth == 30)) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
}

/*
 * Allocate a GL texture and attempt to attach an FBO to it.
 * Texture storage is allocated but not initialised -
 * cpu_valid governs which side is authoritative until the first GPU use.
 *
 * Returns without creating anything if the depth has no GL equivalent.
 */
static void
passata_create_gl_texture(passata_screen_priv *screen_priv,
                          passata_pixmap_priv *priv,
                          int width, int height,
                          int depth, int bitsPerPixel)
{
    GLenum format, type, internal;

    if (!passata_get_gl_format(screen_priv, depth, bitsPerPixel,
                               &format, &type, &internal))
        return; /* 1bpp and other unhandled depths stay sys_copy only */

    glGenTextures(1, &priv->tex);
    glBindTexture(GL_TEXTURE_2D, priv->tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    passata_set_texture_swizzle(screen_priv, depth, bitsPerPixel, format);

    /* Allocate storage without uploading data; the upload happens lazily. */
    glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height,
                 0, format, type, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Try to create an FBO for GPU rendering to this pixmap. */
    glGenFramebuffers(1, &priv->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, priv->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, priv->tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &priv->fbo);
        priv->fbo = 0;
        /* Texture is still usable as a source; just not renderable. */
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void
passata_upload_to_gl(PixmapPtr pPixmap, passata_pixmap_priv *priv)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pPixmap->drawable.pScreen);
    GLenum format, type, internal;
    int    bpp    = pPixmap->drawable.bitsPerPixel;
    int    width  = pPixmap->drawable.width;
    int    height = pPixmap->drawable.height;

    if (!priv->tex || !priv->sys_copy)
        return;

    if (!passata_get_gl_format(screen_priv, pPixmap->drawable.depth, bpp,
                               &format, &type, &internal))
        return;

    glBindTexture(GL_TEXTURE_2D, priv->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, priv->pitch / (bpp / 8));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    format, type, priv->sys_copy);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    priv->gpu_valid = TRUE;
}

void
passata_download_from_gl(PixmapPtr pPixmap, passata_pixmap_priv *priv)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pPixmap->drawable.pScreen);
    GLenum format, type, internal;
    int    bpp = pPixmap->drawable.bitsPerPixel;

    if (!priv->tex || !priv->sys_copy)
        return;

    if (!passata_get_gl_format(screen_priv, pPixmap->drawable.depth, bpp,
                               &format, &type, &internal))
        return;

    glBindTexture(GL_TEXTURE_2D, priv->tex);
    glPixelStorei(GL_PACK_ROW_LENGTH, priv->pitch / (bpp / 8));
    glGetTexImage(GL_TEXTURE_2D, 0, format, type, priv->sys_copy);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    priv->cpu_valid = TRUE;
}

void *
passata_create_pixmap2(ScreenPtr pScreen, int width, int height,
                       int depth, int usage_hint, int bitsPerPixel,
                       int *new_fb_pitch)
{
    passata_screen_priv *screen_priv = passata_get_screen_priv(pScreen);
    passata_pixmap_priv *priv;
    int pitch;

    priv = calloc(1, sizeof(*priv));
    if (!priv)
        return NULL;

    if (width == 0 || height == 0 || bitsPerPixel == 0) {
        *new_fb_pitch = 0;
        return priv;
    }

    pitch         = BitmapBytePad(width * bitsPerPixel);
    *new_fb_pitch = pitch;
    priv->pitch   = pitch;

    priv->sys_copy = calloc(1, pitch * height);
    if (!priv->sys_copy) {
        free(priv);
        return NULL;
    }
    priv->cpu_valid = TRUE;

    /*
     * Attempt to back this pixmap with a GL texture.  Unsupported depths
     * (1bpp) leave tex=0; the pixmap stays in sys_copy only.
     */
    passata_create_gl_texture(screen_priv, priv, width, height,
                              depth, bitsPerPixel);

    return priv;
}

void
passata_destroy_pixmap(ScreenPtr pScreen, void *driverPriv)
{
    passata_pixmap_priv *priv = driverPriv;

    if (!priv)
        return;

    if (priv->fbo)
        glDeleteFramebuffers(1, &priv->fbo);
    if (priv->tex)
        glDeleteTextures(1, &priv->tex);
    if (!priv->is_external)
        free(priv->sys_copy);

    free(priv);
}

Bool
passata_modify_pixmap_header(PixmapPtr pPixmap, int width, int height,
                              int depth, int bitsPerPixel, int devKind,
                              void *pPixData)
{
    passata_pixmap_priv *priv = exaGetPixmapDriverPrivate(pPixmap);

    if (!priv)
        return FALSE;

    if (devKind > 0)
        priv->pitch = devKind;

    if (pPixData) {
        if (!priv->is_external)
            free(priv->sys_copy);
        priv->sys_copy    = pPixData;
        priv->is_external = TRUE;
        priv->cpu_valid   = TRUE;
        priv->gpu_valid   = FALSE;
    }

    return FALSE; /* let EXA update drawable dimensions */
}

Bool
passata_pixmap_is_offscreen(PixmapPtr pPixmap)
{
    passata_pixmap_priv *priv = exaGetPixmapDriverPrivate(pPixmap);
    return priv != NULL && priv->sys_copy != NULL;
}

Bool
passata_prepare_access(PixmapPtr pPix, int index)
{
    passata_pixmap_priv *priv = exaGetPixmapDriverPrivate(pPix);

    if (!priv || !priv->sys_copy)
        return FALSE;

    if (priv->gpu_valid && !priv->cpu_valid)
        passata_download_from_gl(pPix, priv);

    pPix->devPrivate.ptr = priv->sys_copy;
    pPix->devKind        = priv->pitch;
    return TRUE;
}

void
passata_finish_access(PixmapPtr pPix, int index)
{
    passata_pixmap_priv *priv = exaGetPixmapDriverPrivate(pPix);

    if (!priv)
        return;

    /*
     * A write access makes the CPU copy authoritative and invalidates the
     * GL texture.  The upload to the texture is deferred until the next
     * GPU operation that needs it.
     */
    if (index == EXA_PREPARE_DEST || index == EXA_PREPARE_AUX_DEST) {
        priv->cpu_valid = TRUE;
        priv->gpu_valid = FALSE;
    }

    pPix->devPrivate.ptr = NULL;
}