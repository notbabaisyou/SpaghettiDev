#include "vaccum_priv.h"
#include "vaccum_transfer.h"

static void
vaccum_copy_bail(DrawablePtr src,
                 DrawablePtr dst,
                 GCPtr gc,
                 BoxPtr box,
                 int nbox,
                 int dx,
                 int dy,
                 Bool reverse,
                 Bool upsidedown,
                 Pixel bitplane,
                 void *closure)
{
    if (vaccum_prepare_access(dst, VACCUM_ACCESS_RW) && vaccum_prepare_access(src, VACCUM_ACCESS_RO)) {
        if (bitplane) {
            if (src->bitsPerPixel > 1)
                fbCopyNto1(src, dst, gc, box, nbox, dx, dy,
                           reverse, upsidedown, bitplane, closure);
            else
                fbCopy1toN(src, dst, gc, box, nbox, dx, dy,
                           reverse, upsidedown, bitplane, closure);
        } else {
            fbCopyNtoN(src, dst, gc, box, nbox, dx, dy,
                       reverse, upsidedown, bitplane, closure);
        }
    }
    vaccum_finish_access(dst);
    vaccum_finish_access(src);
}

void
vaccum_copy(DrawablePtr src,
            DrawablePtr dst,
            GCPtr gc,
            BoxPtr box,
            int nbox,
            int dx,
            int dy,
            Bool reverse,
            Bool upsidedown,
            Pixel bitplane,
            void *closure)
{
    /* TODO */
    if (_X_LIKELY(nbox))
        vaccum_copy_bail(src, dst, gc, box, nbox, dx, dy, reverse, upsidedown, bitplane, closure);
}

RegionPtr
vaccum_copy_area(DrawablePtr src, DrawablePtr dst, GCPtr gc,
                 int srcx, int srcy, int width, int height, int dstx, int dsty)
{
    return miDoCopy(src, dst, gc,
                    srcx, srcy, width, height,
                    dstx, dsty, vaccum_copy, 0, NULL);
}

RegionPtr
vaccum_copy_plane(DrawablePtr src, DrawablePtr dst, GCPtr gc,
                  int srcx, int srcy, int width, int height, int dstx, int dsty,
                  unsigned long bitplane)
{
    if ((bitplane & FbFullMask(src->depth)) == 0)
        return miHandleExposures(src, dst, gc,
                                 srcx, srcy, width, height, dstx, dsty);
    else
        return miDoCopy(src, dst, gc,
                        srcx, srcy, width, height,
                        dstx, dsty, vaccum_copy, bitplane, NULL);
}

void
vaccum_copy_window(WindowPtr window, DDXPointRec old_origin, RegionPtr src_region)
{
    PixmapPtr pixmap = vaccum_get_drawable_pixmap(&window->drawable);
    DrawablePtr drawable = &pixmap->drawable;
    RegionRec dst_region;
    int dx, dy;

    dx = old_origin.x - window->drawable.x;
    dy = old_origin.y - window->drawable.y;
    RegionTranslate(src_region, -dx, -dy);

    RegionNull(&dst_region);

    RegionIntersect(&dst_region, &window->borderClip, src_region);

#ifdef COMPOSITE
    if (pixmap->screen_x || pixmap->screen_y)
        RegionTranslate(&dst_region, -pixmap->screen_x, -pixmap->screen_y);
#endif

    miCopyRegion(drawable, drawable,
                 0, &dst_region, dx, dy, vaccum_copy, 0, 0);

    RegionUninit(&dst_region);
}
