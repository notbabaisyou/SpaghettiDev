/*
 * Copyright © 2014 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "vaccum_priv.h"


static void
vaccum_poly_fill_rect_bail(DrawablePtr drawable,
                           GCPtr gc, int nrect, xRectangle *prect)
{
    vaccum_fallback("to %p (%c)\n", drawable,
                    vaccum_get_drawable_location(drawable));
    if (vaccum_prepare_access(drawable, VACCUM_ACCESS_RW) &&
        vaccum_prepare_access_gc(gc)) {
        fbPolyFillRect(drawable, gc, nrect, prect);
    }
    vaccum_finish_access_gc(gc);
    vaccum_finish_access(drawable);
}

void
vaccum_poly_fill_rect(DrawablePtr drawable,
                      GCPtr gc, int nrect, xRectangle *prect)
{
    vaccum_poly_fill_rect_bail(drawable, gc, nrect, prect);
}
