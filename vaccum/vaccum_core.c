/** @file vaccum_core.c
 *
 * This file covers core X rendering in vaccum.
 */

#include "vaccum_priv.h"

Bool
vaccum_get_drawable_location(const DrawablePtr drawable)
{
    PixmapPtr pixmap = vaccum_get_drawable_pixmap(drawable);
    vaccum_pixmap_private *pixmap_priv = vaccum_get_pixmap_private(pixmap);

    if (pixmap_priv->vk_image == VACCUM_IMAGE_UNATTACHED)
        return 'm';
    else
        return 'f';
}

static GCOps vaccum_gc_ops = {
    .FillSpans = vaccum_fill_spans,
    .SetSpans = vaccum_set_spans,
    .PutImage = vaccum_put_image,
    .CopyArea = vaccum_copy_area,
    .CopyPlane = vaccum_copy_plane,
    .PolyPoint = vaccum_poly_point,
    .Polylines = vaccum_poly_lines,
    .PolySegment = vaccum_poly_segment,
    .PolyRectangle = miPolyRectangle,
    .PolyArc = miPolyArc,
    .FillPolygon = miFillPolygon,
    .PolyFillRect = vaccum_poly_fill_rect,
    .PolyFillArc = miPolyFillArc,
    .PolyText8 = vaccum_poly_text8,
    .PolyText16 = vaccum_poly_text16,
    .ImageText8 = vaccum_image_text8,
    .ImageText16 = vaccum_image_text16,
    .ImageGlyphBlt = miImageGlyphBlt,
    .PolyGlyphBlt = vaccum_poly_glyph_blt,
    .PushPixels = vaccum_push_pixels,
};

/*
 * When the stipple is changed or drawn to, invalidate any
 * cached copy
 */
static void
vaccum_invalidate_stipple(GCPtr gc)
{
    vaccum_gc_private *gc_priv = vaccum_get_gc_private(gc);

    if (gc_priv->stipple) {
        if (gc_priv->stipple_damage)
            DamageUnregister(gc_priv->stipple_damage);
        vaccum_destroy_pixmap(gc_priv->stipple);
        gc_priv->stipple = NULL;
    }
}

static void
vaccum_stipple_damage_report(DamagePtr damage, RegionPtr region,
                             void *closure)
{
    GCPtr       gc = closure;

    vaccum_invalidate_stipple(gc);
}

static void
vaccum_stipple_damage_destroy(DamagePtr damage, void *closure)
{
    GCPtr               gc = closure;
    vaccum_gc_private   *gc_priv = vaccum_get_gc_private(gc);

    gc_priv->stipple_damage = NULL;
    vaccum_invalidate_stipple(gc);
}

void
vaccum_track_stipple(GCPtr gc)
{
    if (gc->stipple) {
        vaccum_gc_private *gc_priv = vaccum_get_gc_private(gc);

        if (!gc_priv->stipple_damage)
            gc_priv->stipple_damage = DamageCreate(vaccum_stipple_damage_report,
                                                   vaccum_stipple_damage_destroy,
                                                   DamageReportNonEmpty,
                                                   TRUE, gc->pScreen, gc);
        if (gc_priv->stipple_damage)
            DamageRegister(&gc->stipple->drawable, gc_priv->stipple_damage);
    }
}

void
vaccum_validate_gc(GCPtr gc, unsigned long changes, DrawablePtr drawable)
{
    /* fbValidateGC will do direct access to pixmaps if the tiling has changed.
     * Preempt fbValidateGC by doing its work and masking the change out, so
     * that we can do the Prepare/finish_access.
     */
    if (changes & GCTile) {
        if (!gc->tileIsPixel) {
            vaccum_pixmap_private *pixmap_priv =
                vaccum_get_pixmap_private(gc->tile.pixmap);
            if ((!VACCUM_PIXMAP_PRIV_HAS_FBO(pixmap_priv))
                && FbEvenTile(gc->tile.pixmap->drawable.width *
                              drawable->bitsPerPixel)) {
                vaccum_fallback
                    ("GC %p tile changed %p.\n", gc, gc->tile.pixmap);
                if (vaccum_prepare_access
                    (&gc->tile.pixmap->drawable, VACCUM_ACCESS_RW)) {
                    fbPadPixmap(gc->tile.pixmap);
                    vaccum_finish_access(&gc->tile.pixmap->drawable);
                }
            }
        }
        /* Mask out the GCTile change notification, now that we've done FB's
         * job for it.
         */
        changes &= ~GCTile;
    }

    if (changes & GCStipple)
        vaccum_invalidate_stipple(gc);

    if (changes & GCStipple && gc->stipple) {
        /* We can't inline stipple handling like we do for GCTile because
         * it sets fbgc privates.
         */
        if (vaccum_prepare_access(&gc->stipple->drawable, VACCUM_ACCESS_RW)) {
            fbValidateGC(gc, changes, drawable);
            vaccum_finish_access(&gc->stipple->drawable);
        }
    }
    else {
        fbValidateGC(gc, changes, drawable);
    }

    if (changes & GCDashList) {
        vaccum_gc_private *gc_priv = vaccum_get_gc_private(gc);

        if (gc_priv->dash) {
            vaccum_destroy_pixmap(gc_priv->dash);
            gc_priv->dash = NULL;
        }
    }

    gc->ops = &vaccum_gc_ops;
}

void
vaccum_destroy_gc(GCPtr gc)
{
    vaccum_gc_private *gc_priv = vaccum_get_gc_private(gc);

    if (gc_priv->dash) {
        vaccum_destroy_pixmap(gc_priv->dash);
        gc_priv->dash = NULL;
    }
    vaccum_invalidate_stipple(gc);
    if (gc_priv->stipple_damage)
        DamageDestroy(gc_priv->stipple_damage);
    miDestroyGC(gc);
}

static GCFuncs vaccum_gc_funcs = {
    vaccum_validate_gc,
    miChangeGC,
    miCopyGC,
    vaccum_destroy_gc,
    miChangeClip,
    miDestroyClip,
    miCopyClip
};


/**
 * exaCreateGC makes a new GC and hooks up its funcs handler, so that
 * exaValidateGC() will get called.
 */
int
vaccum_create_gc(GCPtr gc)
{
    vaccum_gc_private *gc_priv = vaccum_get_gc_private(gc);

    gc_priv->dash = NULL;
    gc_priv->stipple = NULL;
    if (!fbCreateGC(gc))
        return FALSE;

    gc->funcs = &vaccum_gc_funcs;

    return TRUE;
}
