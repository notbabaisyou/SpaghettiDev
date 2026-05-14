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

/*
 * passata_setup_viewport: bind an FBO and configure a Y-flipped orthographic
 * projection so that vertex coordinates match X11's top-left origin.
 */
static void
passata_setup_viewport(PixmapPtr pPixmap, GLuint fbo)
{
    int w = pPixmap->drawable.width;
    int h = pPixmap->drawable.height;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, 0, h, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/*
 * passata_ensure_gpu: lazily upload sys_copy to the GL texture if the GPU
 * copy is stale.  Returns FALSE if the pixmap has no GL texture.
 */
static Bool
passata_ensure_gpu(PixmapPtr pPixmap, passata_pixmap_priv *priv)
{
    if (!priv || !priv->tex)
        return FALSE;

    if (priv->cpu_valid && !priv->gpu_valid)
        passata_upload_to_gl(pPixmap, priv);

    return TRUE;
}

/*
 * passata_mark_gpu_dirty: after a GPU write, the texture is authoritative
 * and the CPU copy is stale.
 */
static void
passata_mark_gpu_dirty(passata_pixmap_priv *priv)
{
    if (priv) {
        priv->gpu_valid = TRUE;
        priv->cpu_valid = FALSE;
    }
}

/*
 * passata_pixel_to_color: convert an X11 pixel value to normalised RGBA
 * floats for use with glClearColor / glColor4f.
 */
static void
passata_pixel_to_color(PixmapPtr pPixmap, Pixel pixel, GLfloat *color)
{
    int depth = pPixmap->drawable.depth;
    int bpp   = pPixmap->drawable.bitsPerPixel;

    if (bpp == 32 && depth == 32) {
        color[0] = ((pixel >> 16) & 0xff) / 255.0f;
        color[1] = ((pixel >>  8) & 0xff) / 255.0f;
        color[2] = ((pixel      ) & 0xff) / 255.0f;
        color[3] = ((pixel >> 24) & 0xff) / 255.0f;
    } else if (bpp == 32 && depth == 30) {
        color[0] = ((pixel >> 20) & 0x3ff) / 1023.0f;
        color[1] = ((pixel >> 10) & 0x3ff) / 1023.0f;
        color[2] = ((pixel      ) & 0x3ff) / 1023.0f;
        color[3] = 1.0f;
    } else if (bpp == 32) {
        /* depth=24, XRGB8888 */
        color[0] = ((pixel >> 16) & 0xff) / 255.0f;
        color[1] = ((pixel >>  8) & 0xff) / 255.0f;
        color[2] = ((pixel      ) & 0xff) / 255.0f;
        color[3] = 1.0f;
    } else if (bpp == 16 && depth == 15) {
        color[0] = ((pixel >> 10) & 0x1f) / 31.0f;
        color[1] = ((pixel >>  5) & 0x1f) / 31.0f;
        color[2] = ((pixel      ) & 0x1f) / 31.0f;
        color[3] = 1.0f;
    } else if (bpp == 16) {
        color[0] = ((pixel >> 11) & 0x1f) / 31.0f;
        color[1] = ((pixel >>  5) & 0x3f) / 63.0f;
        color[2] = ((pixel      ) & 0x1f) / 31.0f;
        color[3] = 1.0f;
    } else if (bpp == 8) {
        color[0] = 0.0f;
        color[1] = 0.0f;
        color[2] = 0.0f;
        color[3] = (pixel & 0xff) / 255.0f;
    } else {
        color[0] = color[1] = color[2] = color[3] = 0.0f;
    }
}

/*
 * passata_draw_textured_quad: draw a textured rectangle from a source texture
 * into the currently bound FBO.
 *
 * With standard ortho (glOrtho(0,w,0,h,-1,1)), vertex y=r renders to
 * framebuffer_y=r, which is exactly where sys_copy row r was uploaded.
 * Texcoord t=r/src_h samples row r.
 */
static void
passata_draw_textured_quad(GLuint tex,
                           int src_w, int src_h,
                           int srcX, int srcY,
                           int dstX, int dstY,
                           int width, int height)
{
    GLfloat s0 = (GLfloat) srcX           / src_w;
    GLfloat s1 = (GLfloat)(srcX + width)  / src_w;
    GLfloat t0 = (GLfloat) srcY           / src_h;
    GLfloat t1 = (GLfloat)(srcY + height) / src_h;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBegin(GL_QUADS);
    glTexCoord2f(s0, t0); glVertex2f(dstX,         dstY);
    glTexCoord2f(s1, t0); glVertex2f(dstX + width, dstY);
    glTexCoord2f(s1, t1); glVertex2f(dstX + width, dstY + height);
    glTexCoord2f(s0, t1); glVertex2f(dstX,         dstY + height);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

Bool
passata_prepare_solid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pPixmap->drawable.pScreen);
    passata_pixmap_priv *priv = exaGetPixmapDriverPrivate(pPixmap);

    if (alu != GXcopy)
        return FALSE;

    if (!EXA_PM_IS_SOLID(&pPixmap->drawable, planemask))
        return FALSE;

    if (!priv || !priv->fbo)
        return FALSE;

    /*
     * Upload the CPU copy so that areas of the pixmap not covered by
     * subsequent Solid() calls retain their current contents.
     */
    passata_ensure_gpu(pPixmap, priv);

    passata_pixel_to_color(pPixmap, fg,
                           screen_priv->prepare_args.solid.color);
    screen_priv->prepare_args.solid.pDstPixmap = pPixmap;

    passata_setup_viewport(pPixmap, priv->fbo);
    return TRUE;
}

void
passata_solid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pPixmap->drawable.pScreen);
    GLfloat *c = screen_priv->prepare_args.solid.color;

    glEnable(GL_SCISSOR_TEST);
    glScissor(x1, y1, x2 - x1, y2 - y1);
    glClearColor(c[0], c[1], c[2], c[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

void
passata_done_solid(PixmapPtr pPixmap)
{
    passata_mark_gpu_dirty(exaGetPixmapDriverPrivate(pPixmap));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Bool
passata_prepare_copy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap,
                     int dx, int dy, int alu, Pixel planemask)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pSrcPixmap->drawable.pScreen);
    passata_pixmap_priv *src_priv = exaGetPixmapDriverPrivate(pSrcPixmap);
    passata_pixmap_priv *dst_priv = exaGetPixmapDriverPrivate(pDstPixmap);

    if (alu != GXcopy)
        return FALSE;

    if (!EXA_PM_IS_SOLID(&pDstPixmap->drawable, planemask))
        return FALSE;

    if (!src_priv || !src_priv->tex)
        return FALSE;

    if (!dst_priv || !dst_priv->fbo)
        return FALSE;

    if (!passata_ensure_gpu(pSrcPixmap, src_priv))
        return FALSE;

    passata_ensure_gpu(pDstPixmap, dst_priv);

    screen_priv->prepare_args.copy.pSrcPixmap = pSrcPixmap;
    screen_priv->prepare_args.copy.pDstPixmap = pDstPixmap;

    passata_setup_viewport(pDstPixmap, dst_priv->fbo);
    glDisable(GL_BLEND);
    return TRUE;
}

void
passata_copy(PixmapPtr pDstPixmap, int srcX, int srcY,
             int dstX, int dstY, int width, int height)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pDstPixmap->drawable.pScreen);
    PixmapPtr pSrc = screen_priv->prepare_args.copy.pSrcPixmap;
    passata_pixmap_priv *src_priv = exaGetPixmapDriverPrivate(pSrc);

    passata_draw_textured_quad(src_priv->tex,
                               pSrc->drawable.width,
                               pSrc->drawable.height,
                               srcX, srcY, dstX, dstY, width, height);
}

void
passata_done_copy(PixmapPtr pDstPixmap)
{
    passata_mark_gpu_dirty(exaGetPixmapDriverPrivate(pDstPixmap));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Bool
passata_check_composite(int op, PicturePtr pSrcPicture,
                        PicturePtr pMaskPicture, PicturePtr pDstPicture)
{
    if (pMaskPicture)
        return FALSE;

    /* Source must be a pixmap drawable, not a solid/gradient fill */
    if (!pSrcPicture->pDrawable)
        return FALSE;

    if (pSrcPicture->transform)
        return FALSE;

    switch (op) {
    case PictOpSrc:
    case PictOpOver:
    case PictOpAdd:
        return TRUE;
    default:
        return FALSE;
    }
}

Bool
passata_prepare_composite(int op,
                          PicturePtr pSrcPicture,
                          PicturePtr pMaskPicture,
                          PicturePtr pDstPicture,
                          PixmapPtr pSrc, PixmapPtr pMask, PixmapPtr pDst)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pDst->drawable.pScreen);
    passata_pixmap_priv *src_priv = exaGetPixmapDriverPrivate(pSrc);
    passata_pixmap_priv *dst_priv = exaGetPixmapDriverPrivate(pDst);

    if (!src_priv || !src_priv->tex)
        return FALSE;

    if (!dst_priv || !dst_priv->fbo)
        return FALSE;

    if (!passata_ensure_gpu(pSrc, src_priv))
        return FALSE;

    passata_ensure_gpu(pDst, dst_priv);

    screen_priv->prepare_args.composite.op   = op;
    screen_priv->prepare_args.composite.pSrc = pSrc;

    passata_setup_viewport(pDst, dst_priv->fbo);

    switch (op) {
    case PictOpSrc:
        glDisable(GL_BLEND);
        break;
    case PictOpOver:
        /*
         * RENDER uses premultiplied alpha throughout.
         * dst = src + (1 - src.a) * dst
         */
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case PictOpAdd:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        break;
    }

    return TRUE;
}

void
passata_composite(PixmapPtr pDst, int srcX, int srcY,
                  int maskX, int maskY, int dstX, int dstY,
                  int width, int height)
{
    passata_screen_priv *screen_priv =
        passata_get_screen_priv(pDst->drawable.pScreen);
    PixmapPtr pSrc = screen_priv->prepare_args.composite.pSrc;
    passata_pixmap_priv *src_priv = exaGetPixmapDriverPrivate(pSrc);

    passata_draw_textured_quad(src_priv->tex,
                               pSrc->drawable.width,
                               pSrc->drawable.height,
                               srcX, srcY, dstX, dstY, width, height);
}

void
passata_done_composite(PixmapPtr pDst)
{
    passata_mark_gpu_dirty(exaGetPixmapDriverPrivate(pDst));
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}