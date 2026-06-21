#include "vaccum_priv.h"

static void
vaccum_fill_spans_bail(DrawablePtr drawable,
                       GCPtr gc,
                       int n, DDXPointPtr points, int *widths, int sorted)
{
    if (vaccum_prepare_access(drawable, VACCUM_ACCESS_RW) &&
        vaccum_prepare_access_gc(gc)) {
        fbFillSpans(drawable, gc, n, points, widths, sorted);
    }
    vaccum_finish_access_gc(gc);
    vaccum_finish_access(drawable);
}

void
vaccum_fill_spans(DrawablePtr drawable,
                  GCPtr gc,
                  int n, DDXPointPtr points, int *widths, int sorted)
{
    //    if (vaccum_fill_spans_gl(drawable, gc, n, points, widths, sorted))
    //        return;
    vaccum_fill_spans_bail(drawable, gc, n, points, widths, sorted);
}

static void
vaccum_get_spans_bail(DrawablePtr drawable, int wmax,
                 DDXPointPtr points, int *widths, int count, char *dst)
{
    if (vaccum_prepare_access(drawable, VACCUM_ACCESS_RO))
        fbGetSpans(drawable, wmax, points, widths, count, dst);
    vaccum_finish_access(drawable);
}

void
vaccum_get_spans(DrawablePtr drawable, int wmax,
                 DDXPointPtr points, int *widths, int count, char *dst)
{
    //    if (vaccum_get_spans_gl(drawable, wmax, points, widths, count, dst))
    //return;
    vaccum_get_spans_bail(drawable, wmax, points, widths, count, dst);
}

static void
vaccum_set_spans_bail(DrawablePtr drawable, GCPtr gc, char *src,
                      DDXPointPtr points, int *widths, int numPoints, int sorted)
{
    if (vaccum_prepare_access(drawable, VACCUM_ACCESS_RW) && vaccum_prepare_access_gc(gc))
        fbSetSpans(drawable, gc, src, points, widths, numPoints, sorted);
    vaccum_finish_access_gc(gc);
    vaccum_finish_access(drawable);
}

void
vaccum_set_spans(DrawablePtr drawable, GCPtr gc, char *src,
                 DDXPointPtr points, int *widths, int numPoints, int sorted)
{
    //    if (vaccum_set_spans_gl(drawable, gc, src, points, widths, numPoints, sorted))
    //        return;
    vaccum_set_spans_bail(drawable, gc, src, points, widths, numPoints, sorted);
}
