#include "vaccum_priv.h"

static void
vaccum_poly_segment_bail(DrawablePtr drawable, GCPtr gc,
                         int nseg, xSegment *segs)
{
    vaccum_fallback("to %p (%c)\n", drawable,
                    vaccum_get_drawable_location(drawable));

    if (gc->lineWidth == 0) {
        if (vaccum_prepare_access(drawable, VACCUM_ACCESS_RW) &&
            vaccum_prepare_access_gc(gc)) {
            fbPolySegment(drawable, gc, nseg, segs);
        }
        vaccum_finish_access_gc(gc);
        vaccum_finish_access(drawable);
    } else
        miPolySegment(drawable, gc, nseg, segs);
}

void
vaccum_poly_segment(DrawablePtr drawable, GCPtr gc,
                    int nseg, xSegment *segs)
{
    //    if (vaccum_poly_segment_gl(drawable, gc, nseg, segs))
    //        return;

    vaccum_poly_segment_bail(drawable, gc, nseg, segs);
}
