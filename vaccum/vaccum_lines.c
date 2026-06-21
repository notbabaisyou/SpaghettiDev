#include "vaccum_priv.h"

static void
vaccum_poly_lines_bail(DrawablePtr drawable, GCPtr gc,
                       int mode, int n, DDXPointPtr points)
{
    vaccum_fallback("to %p (%c)\n", drawable,
                    vaccum_get_drawable_location(drawable));

    miPolylines(drawable, gc, mode, n, points);
}

void
vaccum_poly_lines(DrawablePtr drawable, GCPtr gc,
                  int mode, int n, DDXPointPtr points)
{
    vaccum_poly_lines_bail(drawable, gc, mode, n, points);
}
