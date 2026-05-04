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

static XF86ModuleVersionInfo passata_version_info = {
    "passata",
    "Spaghetti Fork",
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    1, 0, 0,
    ABI_CLASS_EXTENSION,
    ABI_EXTENSION_VERSION,
    MOD_CLASS_NONE,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData passataModuleData = {
    &passata_version_info,
    NULL,
    NULL
};

static DevPrivateKeyRec passata_screen_private_key;

passata_screen_priv *
passata_get_screen_priv(ScreenPtr pScreen)
{
    return dixGetPrivate(&pScreen->devPrivates, &passata_screen_private_key);
}

/*
 * EXA hooks — all operations return FALSE for now, causing EXA to fall back
 * to software rendering for everything.  Acceleration will be added in later
 * phases once EGL and GL are initialised.
 */

static Bool
passata_prepare_solid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg)
{
    return FALSE;
}

static void
passata_solid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
}

static void
passata_done_solid(PixmapPtr pPixmap)
{
}

static Bool
passata_prepare_copy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap,
                     int dx, int dy, int alu, Pixel planemask)
{
    return FALSE;
}

static void
passata_copy(PixmapPtr pDstPixmap, int srcX, int srcY,
             int dstX, int dstY, int width, int height)
{
}

static void
passata_done_copy(PixmapPtr pDstPixmap)
{
}

static Bool
passata_check_composite(int op, PicturePtr pSrcPicture,
                        PicturePtr pMaskPicture, PicturePtr pDstPicture)
{
    return FALSE;
}

static Bool
passata_prepare_composite(int op,
                          PicturePtr pSrcPicture,
                          PicturePtr pMaskPicture,
                          PicturePtr pDstPicture,
                          PixmapPtr pSrc, PixmapPtr pMask, PixmapPtr pDst)
{
    return FALSE;
}

static void
passata_composite(PixmapPtr pDst, int srcX, int srcY,
                  int maskX, int maskY, int dstX, int dstY,
                  int width, int height)
{
}

static void
passata_done_composite(PixmapPtr pDst)
{
}

static void *
passata_create_pixmap2(ScreenPtr pScreen, int width, int height,
                       int depth, int usage_hint, int bitsPerPixel,
                       int *new_fb_pitch)
{
    passata_pixmap_priv *priv;
    int pitch;

    priv = calloc(1, sizeof(*priv));
    if (!priv)
        return NULL;

    if (width == 0 || height == 0 || bitsPerPixel == 0) {
        *new_fb_pitch = 0;
        return priv;
    }

    pitch = BitmapBytePad(width * bitsPerPixel);
    *new_fb_pitch = pitch;

    priv->ptr = calloc(1, pitch * height);
    if (!priv->ptr) {
        free(priv);
        return NULL;
    }

    priv->pitch = pitch;
    priv->owned = TRUE;
    return priv;
}

static void
passata_destroy_pixmap(ScreenPtr pScreen, void *driverPriv)
{
    passata_pixmap_priv *priv = driverPriv;

    if (priv && priv->owned)
        free(priv->ptr);

    free(priv);
}

static Bool
passata_prepare_access(PixmapPtr pPix, int index)
{
    passata_pixmap_priv *priv = exaGetPixmapDriverPrivate(pPix);

    if (!priv || !priv->ptr)
        return FALSE;

    pPix->devPrivate.ptr = priv->ptr;
    pPix->devKind        = priv->pitch;
    return TRUE;
}

static void
passata_finish_access(PixmapPtr pPix, int index)
{
    pPix->devPrivate.ptr = NULL;
}

static Bool
passata_pixmap_is_offscreen(PixmapPtr pPixmap)
{
    return exaGetPixmapDriverPrivate(pPixmap) != NULL;
}

static void
passata_wait_marker(ScreenPtr pScreen, int marker)
{
    /* TODO: sync against outstanding GL work */
}

static Bool
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
        priv->ptr   = pPixData;
        priv->owned = FALSE;
    }

    return FALSE;
}

static void
passata_setup_exa(ExaDriverPtr exa)
{
    exa->exa_major = EXA_VERSION_MAJOR;
    exa->exa_minor = EXA_VERSION_MINOR;

    exa->flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS;

    /*
     * Temporary limits — will be replaced with GL_MAX_TEXTURE_SIZE
     */
    exa->maxX = 4096;
    exa->maxY = 4096;
    exa->pixmapPitchAlign = 4;

    exa->PrepareSolid = passata_prepare_solid;
    exa->Solid        = passata_solid;
    exa->DoneSolid    = passata_done_solid;

    exa->PrepareCopy = passata_prepare_copy;
    exa->Copy        = passata_copy;
    exa->DoneCopy    = passata_done_copy;

    exa->CheckComposite   = passata_check_composite;
    exa->PrepareComposite = passata_prepare_composite;
    exa->Composite        = passata_composite;
    exa->DoneComposite    = passata_done_composite;

    exa->WaitMarker        = passata_wait_marker;
    exa->PrepareAccess     = passata_prepare_access;
    exa->FinishAccess      = passata_finish_access;
    exa->PixmapIsOffscreen = passata_pixmap_is_offscreen;

    exa->DestroyPixmap      = passata_destroy_pixmap;
    exa->ModifyPixmapHeader = passata_modify_pixmap_header;
    exa->CreatePixmap2      = passata_create_pixmap2;
}

Bool
passata_init(ScrnInfoPtr scrn, int fd)
{
    ScreenPtr pScreen = scrn->pScreen;
    passata_screen_priv *priv;
    ExaDriverPtr exa;

    if (!dixRegisterPrivateKey(&passata_screen_private_key,
                               PRIVATE_SCREEN, 0))
        return FALSE;

    priv = calloc(1, sizeof(*priv));
    if (!priv)
        return FALSE;

    priv->fd   = fd;
    priv->scrn = scrn;
    dixSetPrivate(&pScreen->devPrivates, &passata_screen_private_key, priv);

    exa = exaDriverAlloc();
    if (!exa)
        goto bail;

    priv->exa = exa;
    passata_setup_exa(exa);

    if (!exaDriverInit(pScreen, exa))
        goto bail;

    xf86DrvMsg(scrn->scrnIndex, X_INFO,
               "%s: initialised (software fallback only)\n", PASSATA_LOG_PREFIX);
    return TRUE;

bail:
    passata_fini(scrn);
    return FALSE;
}

void
passata_fini(ScrnInfoPtr scrn)
{
    ScreenPtr pScreen = scrn->pScreen;
    passata_screen_priv *priv = passata_get_screen_priv(pScreen);

    if (!priv)
        return;

    if (priv->exa) {
        exaDriverFini(pScreen);
        free(priv->exa);
    }

    free(priv);
    dixSetPrivate(&pScreen->devPrivates, &passata_screen_private_key, NULL);
}
