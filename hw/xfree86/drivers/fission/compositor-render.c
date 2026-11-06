/*
 * Copyright © 2026 dec05eba
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <pixman.h>
#include <xf86.h>
#include <xf86Crtc.h>
#include <compositeext.h>
#include <cursorstr.h>
#include <picturestr.h>
#include <present.h>
#include <servermd.h>

#include "driver.h"

typedef struct {
    const struct pixman_f_transform *to_screen;
    const struct pixman_f_transform *from_screen;
    PictFilterPtr filter;
    xFixed *params;
    int nparams;
    int filter_width;
    int filter_height;
} ms_compositor_transform_rec;

static void
ms_compositor_enable_disable_fb_access(ScrnInfoPtr scrn, Bool enable)
{
    modesettingPtr ms = modesettingPTR(scrn);

    if (!enable)
        ms->compositor.EnableDisableFBAccess(scrn, FALSE);
    else if (!xf86ServerIsResetting())
        SetRootClip(scrn->pScreen, ROOT_CLIP_INPUT_ONLY);
}

void
ms_compositor_screen_init(ScrnInfoPtr scrn)
{
    modesettingPtr ms = modesettingPTR(scrn);

    xorg_list_init(&ms->compositor.sw_cursors);
    ms->compositor.EnableDisableFBAccess = scrn->EnableDisableFBAccess;
    scrn->EnableDisableFBAccess = ms_compositor_enable_disable_fb_access;
}

Bool
ms_compositor_create_screen_resources(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    PixmapPtr old_pixmap = screen->GetScreenPixmap(screen);
    PixmapPtr pixmap;

    pixmap = screen->CreatePixmap(screen, 0, 0, screen->rootDepth, 0);
    if (!pixmap)
        return FALSE;

    screen->SetScreenPixmap(pixmap);
    dixDestroyPixmap(old_pixmap, 0);

    if (!CompositeServerInit(screen)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Failed to set up per-monitor framebuffers, use "
                   "Option \"Compositing\" \"false\" for a shared framebuffer\n");
        return FALSE;
    }

    return TRUE;
}

static void
ms_compositor_add_damage(ScrnInfoPtr scrn, RegionPtr damage)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int c, i;

    for (c = 0; c < config->num_crtc; c++) {
        xf86CrtcPtr crtc = config->crtc[c];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        drmmode_compositor_ptr comp = &drmmode_crtc->compositor;
        RegionRec region;

        if (!comp->pixmap[0])
            continue;

        RegionInit(&region, &crtc->bounds, 1);
        RegionIntersect(&region, &region, damage);
        for (i = 0; i < ARRAY_SIZE(comp->pixmap); i++)
            RegionUnion(&comp->stale[i], &comp->stale[i], &region);
        if (RegionNotEmpty(&region))
            comp->repaint = TRUE;
        RegionUninit(&region);
    }
}

static void
ms_compositor_damage_box(ScrnInfoPtr scrn, BoxPtr box)
{
    PixmapDirtyUpdatePtr dirty;
    RegionRec region;

    RegionInit(&region, box, 1);
    ms_compositor_add_damage(scrn, &region);
    xorg_list_for_each_entry(dirty, &scrn->pScreen->pixmap_dirty_list, ent)
        RegionUnion(DamageRegion(dirty->damage), DamageRegion(dirty->damage),
                    &region);
    RegionUninit(&region);
}

void
ms_compositor_update_damages(ScreenPtr screen)
{
    RegionRec damage;

    RegionNull(&damage);
    CompositeServerFlush(screen, &damage);
    if (RegionNotEmpty(&damage))
        ms_compositor_add_damage(xf86ScreenToScrn(screen), &damage);
    RegionUninit(&damage);
}

static PicturePtr
ms_compositor_create_picture(ScreenPtr screen, PixmapPtr pixmap)
{
    int error;

    return CreatePicture(0, &pixmap->drawable,
                         PictureWindowFormat(screen->root), 0, NULL,
                         serverClient, &error);
}

static void
ms_compositor_paint_cursors(ScrnInfoPtr scrn, PicturePtr dst, int x, int y,
                            RegionPtr region)
{
    modesettingPtr ms = modesettingPTR(scrn);
    ms_sw_cursor_ptr sw_cursor;

    xorg_list_for_each_entry(sw_cursor, &ms->compositor.sw_cursors, entry) {
        BoxPtr box = &sw_cursor->box;
        RegionRec clip;

        RegionInit(&clip, box, 1);
        RegionIntersect(&clip, &clip, region);
        if (RegionNotEmpty(&clip)) {
            SetPictureClipRegion(dst, -x, -y, &clip);
            CompositePicture(PictOpOver, sw_cursor->picture, NULL, dst,
                             0, 0, 0, 0, box->x1 - x, box->y1 - y,
                             box->x2 - box->x1, box->y2 - box->y1);
        }
        RegionUninit(&clip);
    }

    SetPictureClipRegion(dst, 0, 0, NULL);
}

static void
ms_compositor_paint_layers(ScrnInfoPtr scrn, PicturePtr dst, int x, int y,
                           RegionPtr region)
{
    CompositeServerPaint(scrn->pScreen, dst, x, y, region, NULL);
    ms_compositor_paint_cursors(scrn, dst, x, y, region);
}

static PixmapPtr
ms_compositor_untransformed_pixmap(xf86CrtcPtr crtc, Bool *created)
{
    ScreenPtr screen = crtc->scrn->pScreen;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_compositor_ptr comp = &drmmode_crtc->compositor;
    int width = crtc->bounds.x2 - crtc->bounds.x1;
    int height = crtc->bounds.y2 - crtc->bounds.y1;

    *created = FALSE;
    if (comp->untransformed &&
        comp->untransformed->drawable.width == width &&
        comp->untransformed->drawable.height == height)
        return comp->untransformed;

    if (comp->untransformed)
        dixDestroyPixmap(comp->untransformed, 0);

    comp->untransformed = screen->CreatePixmap(screen, width, height,
                                              screen->rootDepth, 0);
    *created = comp->untransformed != NULL;
    return comp->untransformed;
}

static Bool
ms_compositor_copy_transformed(PicturePtr src, int x, int y, PicturePtr dst,
                               RegionPtr region,
                               const ms_compositor_transform_rec *transform)
{
    struct pixman_f_transform f_transform = *transform->to_screen;
    BoxPtr box = RegionRects(region);
    int n = RegionNumRects(region);
    PictTransform pict_transform;

    pixman_f_transform_translate(&f_transform, NULL, -x, -y);
    if (!pixman_transform_from_pixman_f_transform(&pict_transform,
                                                  &f_transform) ||
        SetPictureTransform(src, &pict_transform) != Success)
        return FALSE;

    if (transform->filter)
        SetPicturePictFilter(src, transform->filter, transform->params,
                             transform->nparams);

    while (n--) {
        BoxRec dst_box = *box++;

        dst_box.x1 -= transform->filter_width >> 1;
        dst_box.x2 += transform->filter_width >> 1;
        dst_box.y1 -= transform->filter_height >> 1;
        dst_box.y2 += transform->filter_height >> 1;
        if (!pixman_f_transform_bounds(transform->from_screen, &dst_box))
            return FALSE;
        CompositePicture(PictOpSrc, src, NULL, dst,
                         dst_box.x1, dst_box.y1, 0, 0, dst_box.x1, dst_box.y1,
                         dst_box.x2 - dst_box.x1, dst_box.y2 - dst_box.y1);
    }
    return TRUE;
}

static Bool
ms_compositor_paint_transformed(xf86CrtcPtr crtc, PicturePtr dst,
                                RegionPtr region)
{
    ScrnInfoPtr scrn = crtc->scrn;
    ms_compositor_transform_rec transform = {
        .to_screen = &crtc->f_crtc_to_framebuffer,
        .from_screen = &crtc->f_framebuffer_to_crtc,
        .filter = crtc->filter,
        .params = crtc->params,
        .nparams = crtc->nparams,
        .filter_width = crtc->filter_width,
        .filter_height = crtc->filter_height,
    };
    PixmapPtr pixmap;
    PicturePtr src;
    Bool created, painted;

    pixmap = ms_compositor_untransformed_pixmap(crtc, &created);
    if (!pixmap)
        return FALSE;

    src = ms_compositor_create_picture(scrn->pScreen, pixmap);
    if (!src)
        return FALSE;

    if (created) {
        RegionRec bounds;

        RegionInit(&bounds, &crtc->bounds, 1);
        ms_compositor_paint_layers(scrn, src, crtc->bounds.x1,
                                   crtc->bounds.y1, &bounds);
        RegionUninit(&bounds);
    }
    else {
        ms_compositor_paint_layers(scrn, src, crtc->bounds.x1,
                                   crtc->bounds.y1, region);
    }

    painted = ms_compositor_copy_transformed(src, crtc->bounds.x1,
                                               crtc->bounds.y1, dst, region,
                                               &transform);
    FreePicture(src, 0);
    return painted;
}

Bool
ms_compositor_paint(xf86CrtcPtr crtc, PixmapPtr dst, RegionPtr region)
{
    ScrnInfoPtr scrn = crtc->scrn;
    PicturePtr dst_picture;
    Bool painted = TRUE;

    if (RegionNil(region))
        return TRUE;

    dst_picture = ms_compositor_create_picture(scrn->pScreen, dst);
    if (!dst_picture)
        return FALSE;

    if (crtc->transform_in_use)
        painted = ms_compositor_paint_transformed(crtc, dst_picture, region);
    else
        ms_compositor_paint_layers(scrn, dst_picture, crtc->x, crtc->y,
                                   region);

    FreePicture(dst_picture, 0);
    return painted;
}

static void
ms_compositor_paint_rotated(ScrnInfoPtr scrn, PicturePtr dst,
                            PixmapDirtyUpdatePtr dirty, RegionPtr region)
{
    ScreenPtr screen = scrn->pScreen;
    BoxPtr extents = RegionExtents(region);
    ms_compositor_transform_rec transform = {
        .to_screen = &dirty->f_transform,
        .from_screen = &dirty->f_inverse,
    };
    PixmapPtr pixmap;
    PicturePtr src;

    pixmap = screen->CreatePixmap(screen, extents->x2 - extents->x1,
                                  extents->y2 - extents->y1,
                                  screen->rootDepth, 0);
    if (!pixmap)
        return;

    src = ms_compositor_create_picture(screen, pixmap);
    dixDestroyPixmap(pixmap, 0);
    if (!src)
        return;

    ms_compositor_paint_layers(scrn, src, extents->x1, extents->y1, region);
    ms_compositor_copy_transformed(src, extents->x1, extents->y1, dst, region,
                                   &transform);
    FreePicture(src, 0);
}

Bool
ms_compositor_sync_dirty(PixmapDirtyUpdatePtr dirty)
{
    ScreenPtr screen = dirty->src->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    PixmapPtr dst = dirty->secondary_dst->primary_pixmap ?
        dirty->secondary_dst->primary_pixmap : dirty->secondary_dst;
    Bool rotated = dirty->rotation == RR_Rotate_90 ||
        dirty->rotation == RR_Rotate_270;
    BoxRec box = {
        .x1 = dirty->x,
        .y1 = dirty->y,
        .x2 = dirty->x + (rotated ? dst->drawable.height : dst->drawable.width),
        .y2 = dirty->y + (rotated ? dst->drawable.width : dst->drawable.height),
    };
    PicturePtr dst_picture = NULL;
    Bool painted = FALSE;
    RegionRec region;

    RegionInit(&region, &box, 1);
    RegionIntersect(&region, &region, DamageRegion(dirty->damage));
    if (RegionNotEmpty(&region))
        dst_picture = ms_compositor_create_picture(screen, dst);

    if (dst_picture) {
        if (dirty->rotation == RR_Rotate_0)
            ms_compositor_paint_layers(scrn, dst_picture,
                                       dirty->x - dirty->dst_x,
                                       dirty->y - dirty->dst_y, &region);
        else
            ms_compositor_paint_rotated(scrn, dst_picture, dirty, &region);
        FreePicture(dst_picture, 0);
        painted = TRUE;
    }

    RegionUninit(&region);
    return painted;
}

static Bool
ms_compositor_cursor_bit(const unsigned char *bits, int stride, int x, int y)
{
    unsigned char byte = bits[y * stride + x / 8];

#if BITMAP_BIT_ORDER == MSBFirst
    return byte & (0x80 >> (x & 7));
#else
    return byte & (1 << (x & 7));
#endif
}

static CARD32
ms_compositor_cursor_color(CARD16 red, CARD16 green, CARD16 blue)
{
    return 0xff000000 | (red >> 8) << 16 | (green >> 8) << 8 | blue >> 8;
}

static CARD32 *
ms_compositor_core_cursor_argb(CursorPtr cursor)
{
    CursorBitsPtr bits = cursor->bits;
    int stride = BitmapBytePad(bits->width);
    CARD32 fg = ms_compositor_cursor_color(cursor->foreRed, cursor->foreGreen,
                                           cursor->foreBlue);
    CARD32 bg = ms_compositor_cursor_color(cursor->backRed, cursor->backGreen,
                                           cursor->backBlue);
    CARD32 *argb;
    int x, y;

    argb = calloc(bits->width * bits->height, sizeof(*argb));
    if (!argb)
        return NULL;

    for (y = 0; y < bits->height; y++) {
        for (x = 0; x < bits->width; x++) {
            if (!ms_compositor_cursor_bit(bits->mask, stride, x, y))
                continue;
            argb[y * bits->width + x] =
                ms_compositor_cursor_bit(bits->source, stride, x, y) ? fg : bg;
        }
    }

    return argb;
}

static PicturePtr
ms_compositor_cursor_picture(ScreenPtr screen, CursorPtr cursor)
{
    CursorBitsPtr bits = cursor->bits;
    CARD32 *core_argb = bits->argb ? NULL : ms_compositor_core_cursor_argb(cursor);
    CARD32 *argb = bits->argb ? bits->argb : core_argb;
    PicturePtr picture = NULL;
    PixmapPtr pixmap;
    GCPtr gc;
    int error;

    if (!argb)
        return NULL;

    pixmap = screen->CreatePixmap(screen, bits->width, bits->height, 32, 0);
    gc = GetScratchGC(32, screen);
    if (pixmap && gc) {
        ValidateGC(&pixmap->drawable, gc);
        gc->ops->PutImage(&pixmap->drawable, gc, 32, 0, 0, bits->width,
                          bits->height, 0, ZPixmap, (char *) argb);
        picture = CreatePicture(0, &pixmap->drawable,
                                PictureMatchFormat(screen, 32, PICT_a8r8g8b8),
                                0, NULL, serverClient, &error);
    }

    if (gc)
        FreeScratchGC(gc);
    if (pixmap)
        dixDestroyPixmap(pixmap, 0);
    free(core_argb);
    return picture;
}

static void
ms_compositor_place_cursor(ms_sw_cursor_ptr sw_cursor, CursorPtr cursor,
                           int x, int y)
{
    sw_cursor->box.x1 = x - cursor->bits->xhot;
    sw_cursor->box.y1 = y - cursor->bits->yhot;
    sw_cursor->box.x2 = sw_cursor->box.x1 + cursor->bits->width;
    sw_cursor->box.y2 = sw_cursor->box.y1 + cursor->bits->height;
}

void
ms_compositor_set_cursor(ScrnInfoPtr scrn, ms_sw_cursor_ptr sw_cursor,
                         CursorPtr cursor, int x, int y)
{
    modesettingPtr ms = modesettingPTR(scrn);

    if (sw_cursor->listed) {
        ms_compositor_damage_box(scrn, &sw_cursor->box);
        xorg_list_del(&sw_cursor->entry);
        sw_cursor->listed = FALSE;
    }

    if (sw_cursor->picture) {
        FreePicture(sw_cursor->picture, 0);
        sw_cursor->picture = NULL;
    }

    if (!cursor)
        return;

    sw_cursor->picture = ms_compositor_cursor_picture(scrn->pScreen, cursor);
    if (!sw_cursor->picture)
        return;

    ms_compositor_place_cursor(sw_cursor, cursor, x, y);
    xorg_list_add(&sw_cursor->entry, &ms->compositor.sw_cursors);
    sw_cursor->listed = TRUE;
    ms_compositor_damage_box(scrn, &sw_cursor->box);
}

void
ms_compositor_move_cursor(ScrnInfoPtr scrn, ms_sw_cursor_ptr sw_cursor,
                          CursorPtr cursor, int x, int y)
{
    if (!sw_cursor->listed)
        return;

    ms_compositor_damage_box(scrn, &sw_cursor->box);
    ms_compositor_place_cursor(sw_cursor, cursor, x, y);
    ms_compositor_damage_box(scrn, &sw_cursor->box);
}

#ifdef HAVE_GBM
static Bool
ms_compositor_window_vrr(modesettingPtr ms, WindowPtr window)
{
    WindowPtr child;

    if (ms_window_has_variable_refresh(ms, window))
        return TRUE;
    for (child = window->firstChild; child; child = child->nextSib)
        if (child->viewable && ms_compositor_window_vrr(ms, child))
            return TRUE;
    return FALSE;
}
#endif

Bool
ms_compositor_direct_flip(xf86CrtcPtr crtc)
{
#ifdef HAVE_GBM
    ScrnInfoPtr scrn = crtc->scrn;
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_crtc_private_ptr dc = crtc->driver_private;
    drmmode_compositor_ptr comp = &dc->compositor;
    ms_sw_cursor_ptr cursor;
    WindowPtr window;
    PixmapPtr pixmap;
    drmmode_bo bo = { 0 };
    uint32_t fb = 0, seq;

    if (!ms->drmmode.compositing || !ms->drmmode.pageflip)
        return FALSE;
    if (crtc->transform_in_use || crtc->rotatedPixmap)
        goto compose;
    xorg_list_for_each_entry(cursor, &ms->compositor.sw_cursors, entry) {
        if (cursor->box.x1 < crtc->bounds.x2 && cursor->box.x2 > crtc->bounds.x1 &&
            cursor->box.y1 < crtc->bounds.y2 && cursor->box.y2 > crtc->bounds.y1)
            goto compose;
    }
    window = CompositeServerScanoutWindow(scrn->pScreen, &crtc->bounds);
    if (!window)
        goto compose;
    pixmap = present_window_flip_pixmap(window);
    if (!pixmap || pixmap->drawable.width != crtc->mode.HDisplay ||
        pixmap->drawable.height != crtc->mode.VDisplay ||
        pixmap->drawable.depth != scrn->depth)
        goto compose;

    if (pixmap == comp->scanout_pixmap) {
        fb = comp->scanout_fb;
    } else {
        bo.gbm = ms->glamor.gbm_bo_from_pixmap(scrn->pScreen, pixmap);
        if (!bo.gbm)
            goto compose;
        bo.width = pixmap->drawable.width;
        bo.height = pixmap->drawable.height;
        if (!drmmode_is_format_supported(scrn, gbm_bo_get_format(bo.gbm),
                                          gbm_bo_get_modifier(bo.gbm), FALSE) ||
            drmmode_bo_import(&ms->drmmode, &bo, &fb) != 0) {
            drmmode_bo_destroy(&ms->drmmode, &bo);
            goto compose;
        }
        drmmode_bo_destroy(&ms->drmmode, &bo);
    }

    ms->glamor.block_handler(scrn->pScreen);
    if (ms->vrr_support)
        drmmode_crtc_set_vrr(crtc, ms_compositor_window_vrr(ms, window));
    seq = ms_drm_queue_alloc(crtc, crtc, ms_compositor_flip_handler,
                             ms_compositor_flip_abort);
    if (!seq)
        goto failed;
    if (drmmode_crtc_flip(crtc, fb, 0, 0,
                          DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK,
                          (void *)(intptr_t) seq) != 0) {
        ms_drm_abort_seq(scrn, seq);
        goto failed;
    }
    pixmap->refcnt++;
    comp->pending_pixmap = pixmap;
    comp->pending_fb = fb;
    if (!comp->repaint)
        RegionEmpty(&comp->stale[comp->back_idx]);
    comp->flip_seq = seq;
    ms_compositor_flip_submitted(crtc);
    return TRUE;

failed:
    if (fb && fb != comp->scanout_fb)
        drmModeRmFB(ms->fd, fb);
compose:
    if (ms->vrr_support)
        drmmode_crtc_set_vrr(crtc, FALSE);
#endif
    return FALSE;
}
