#include "vaccum_priv.h"
#include "vaccum_transfer.h"
#include "vaccum_prepare.h"

static Bool
vaccum_prep_pixmap_box(PixmapPtr pixmap, vaccum_access_t access, BoxPtr box)
{
    return TRUE;
}

static void
vaccum_fini_pixmap(PixmapPtr pixmap)
{

}

Bool
vaccum_prepare_access(DrawablePtr drawable, vaccum_access_t access)
{
    PixmapPtr pixmap = vaccum_get_drawable_pixmap(drawable);
    BoxRec box;
    int off_x, off_y;

    vaccum_get_drawable_deltas(drawable, pixmap, &off_x, &off_y);

    box.x1 = drawable->x + off_x;
    box.x2 = box.x1 + drawable->width;
    box.y1 = drawable->y + off_y;
    box.y2 = box.y1 + drawable->height;
    return vaccum_prep_pixmap_box(pixmap, access, &box);
}

Bool
vaccum_prepare_access_box(DrawablePtr drawable, vaccum_access_t access,
                         int x, int y, int w, int h)
{
    PixmapPtr pixmap = vaccum_get_drawable_pixmap(drawable);
    BoxRec box;
    int off_x, off_y;

    vaccum_get_drawable_deltas(drawable, pixmap, &off_x, &off_y);
    box.x1 = drawable->x + x + off_x;
    box.x2 = box.x1 + w;
    box.y1 = drawable->y + y + off_y;
    box.y2 = box.y1 + h;
    return vaccum_prep_pixmap_box(pixmap, access, &box);
}

void
vaccum_finish_access(DrawablePtr drawable)
{
    vaccum_fini_pixmap(vaccum_get_drawable_pixmap(drawable));
}

/*
 * Make a picture ready to use with fb.
 */

Bool
vaccum_prepare_access_picture(PicturePtr picture, vaccum_access_t access)
{
    if (!picture || !picture->pDrawable)
        return TRUE;

    return vaccum_prepare_access(picture->pDrawable, access);
}

Bool
vaccum_prepare_access_picture_box(PicturePtr picture, vaccum_access_t access,
                        int x, int y, int w, int h)
{
    if (!picture || !picture->pDrawable)
        return TRUE;

    /* If a transform is set, we don't know what the bounds is on the
     * source, so just prepare the whole pixmap.  XXX: We could
     * potentially work out where in the source would be sampled based
     * on the transform, and we don't need do do this for destination
     * pixmaps at all.
     */
    if (picture->transform) {
        return vaccum_prepare_access_box(picture->pDrawable, access,
                                         0, 0,
                                         picture->pDrawable->width,
                                         picture->pDrawable->height);
    } else {
        return vaccum_prepare_access_box(picture->pDrawable, access,
                                         x, y, w, h);
    }
}

void
vaccum_finish_access_picture(PicturePtr picture)
{
    if (!picture || !picture->pDrawable)
        return;

    vaccum_finish_access(picture->pDrawable);
}

/*
 * Make a GC ready to use with fb. This just
 * means making sure the appropriate fill pixmap is
 * in CPU memory again
 */

Bool
vaccum_prepare_access_gc(GCPtr gc)
{
    switch (gc->fillStyle) {
    case FillTiled:
        return vaccum_prepare_access(&gc->tile.pixmap->drawable,
                                     VACCUM_ACCESS_RO);
    case FillStippled:
    case FillOpaqueStippled:
        return vaccum_prepare_access(&gc->stipple->drawable, VACCUM_ACCESS_RO);
    default:
        return TRUE;
    }
}

/*
 * Free any temporary CPU pixmaps for the GC
 */
void
vaccum_finish_access_gc(GCPtr gc)
{
    switch (gc->fillStyle) {
    case FillTiled:
        vaccum_finish_access(&gc->tile.pixmap->drawable);
        break;
    case FillStippled:
    case FillOpaqueStippled:
        vaccum_finish_access(&gc->stipple->drawable);
        break;
    }
}
