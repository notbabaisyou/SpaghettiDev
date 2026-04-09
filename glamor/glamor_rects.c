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

#include "glamor_priv.h"
#include "glamor_program.h"
#include "glamor_transform.h"

static const glamor_facet glamor_facet_polyfillrect_130 = {
    .name = "poly_fill_rect",
    .version = 130,
    .source_name = "size",
    .vs_vars = "in vec2 primitive;\n"
               "in vec2 size;\n",
    .vs_exec = ("       vec2 pos = size * vec2(gl_VertexID&1, (gl_VertexID&2)>>1);\n"
                GLAMOR_POS(gl_Position, (primitive.xy + pos))),
};

static const glamor_facet glamor_facet_polyfillrect_120 = {
    .name = "poly_fill_rect",
    .vs_vars = "attribute vec2 primitive;\n",
    .vs_exec = ("        vec2 pos = vec2(0,0);\n"
                GLAMOR_POS(gl_Position, primitive.xy)),
};

static Bool
glamor_poly_fill_rect_gl(DrawablePtr drawable,
                         GCPtr gc, int nrect, xRectangle *prect)
{
    ScreenPtr screen = drawable->pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    PixmapPtr pixmap = glamor_get_drawable_pixmap(drawable);
    glamor_pixmap_private *pixmap_priv;
    glamor_program *prog;
    int off_x, off_y;
    int box_index;
    Bool ret = FALSE;
    Bool use_ints;
    BoxRec bounds = glamor_no_rendering_bounds();

    pixmap_priv = glamor_get_pixmap_private(pixmap);
    if (!GLAMOR_PIXMAP_PRIV_HAS_FBO(pixmap_priv))
        goto bail;

    glamor_make_current(glamor_priv);

    /*
     * Compute the union bounding box of all rects so that each scissor
     * region can be tightened against it (and skipped entirely if they
     * don't overlap).  Always do this for small batches; for large ones
     * only bother when there are multiple clip boxes, since the savings
     * scale with clip complexity while the cost stays O(nrect).
     */
    if (nrect < 100 || RegionNumRects(gc->pCompositeClip) > 1) {
        bounds = glamor_start_rendering_bounds();
        for (int i = 0; i < nrect; i++)
            glamor_bounds_union_rect(&bounds, &prect[i]);
    }

    use_ints = glamor_glsl_has_ints(glamor_priv);

    if (use_ints) {
        prog = glamor_use_program_fill(drawable, gc,
                                       &glamor_priv->poly_fill_rect_program,
                                       &glamor_facet_polyfillrect_130);
        if (!prog)
            goto bail;

        glEnableVertexAttribArray(GLAMOR_VERTEX_POS);
        glVertexAttribDivisor(GLAMOR_VERTEX_POS, 1);
        glEnableVertexAttribArray(GLAMOR_VERTEX_SOURCE);
        glVertexAttribDivisor(GLAMOR_VERTEX_SOURCE, 1);
    } else {
        prog = glamor_use_program_fill(drawable, gc,
                                       &glamor_priv->poly_fill_rect_program,
                                       &glamor_facet_polyfillrect_120);
        if (!prog)
            goto bail;

        glEnableVertexAttribArray(GLAMOR_VERTEX_POS);
    }

    glEnable(GL_SCISSOR_TEST);

    glamor_pixmap_loop(pixmap_priv, box_index) {
        int nbox = RegionNumRects(gc->pCompositeClip);
        BoxPtr box = RegionRects(gc->pCompositeClip);

        if (!glamor_set_destination_drawable(drawable, box_index, TRUE, FALSE,
                                             prog->matrix_uniform, &off_x, &off_y))
            goto bail;

        while (nbox--) {
            GLshort *v;
            char *vbo_offset;
            int nfiltered = 0;

            BoxRec scissor = {
                .x1 = max(box->x1, bounds.x1 + drawable->x),
                .y1 = max(box->y1, bounds.y1 + drawable->y),
                .x2 = min(box->x2, bounds.x2 + drawable->x),
                .y2 = min(box->y2, bounds.y2 + drawable->y),
            };
            box++;

            if (scissor.x1 >= scissor.x2 ||
                scissor.y1 >= scissor.y2)
                continue;

            /*
             * Rather than uploading all nrect rectangles and relying on the
             * GPU's scissor test to discard the non-overlapping ones, we
             * filter the list on the CPU first and upload only the rects
             * that actually intersect this scissor box.  This reduces VBO
             * bandwidth and GPU vertex-processing work, which pays off
             * whenever a meaningful fraction of rects lie outside the
             * current clip box.
             */
            int sx1 = scissor.x1 - drawable->x;
            int sy1 = scissor.y1 - drawable->y;
            int sx2 = scissor.x2 - drawable->x;
            int sy2 = scissor.y2 - drawable->y;

            if (use_ints) {
                v = glamor_get_vbo_space(screen, nrect * sizeof(xRectangle), &vbo_offset);
                if (_X_UNLIKELY(v == NULL))
                    goto bail;

                xRectangle *dst = (xRectangle *)v;
                for (int i = 0; i < nrect; i++) {
                    if (prect[i].x                    < sx2 &&
                        prect[i].x + prect[i].width   > sx1 &&
                        prect[i].y                    < sy2 &&
                        prect[i].y + prect[i].height  > sy1)
                        dst[nfiltered++] = prect[i];
                }

                if (nfiltered > 0) {
                    glVertexAttribPointer(GLAMOR_VERTEX_POS, 2, GL_SHORT, GL_FALSE,
                                          4 * sizeof(short), vbo_offset);
                    glVertexAttribPointer(GLAMOR_VERTEX_SOURCE, 2, GL_UNSIGNED_SHORT, GL_FALSE,
                                          4 * sizeof(short), vbo_offset + 2 * sizeof(short));
                }

                glamor_put_vbo_space(screen);

                if (nfiltered == 0)
                    continue;

                glScissor(scissor.x1 + off_x,
                          scissor.y1 + off_y,
                          scissor.x2 - scissor.x1,
                          scissor.y2 - scissor.y1);
                glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, nfiltered);
            } else {
                v = glamor_get_vbo_space(screen, nrect * 8 * sizeof(short), &vbo_offset);
                if (_X_UNLIKELY(v == NULL))
                    goto bail;

                for (int i = 0; i < nrect; i++) {
                    if (prect[i].x                    < sx2 &&
                        prect[i].x + prect[i].width   > sx1 &&
                        prect[i].y                    < sy2 &&
                        prect[i].y + prect[i].height  > sy1) {
                        v[0] = prect[i].x;                        v[1] = prect[i].y;
                        v[2] = prect[i].x;                        v[3] = prect[i].y + prect[i].height;
                        v[4] = prect[i].x + prect[i].width;       v[5] = prect[i].y + prect[i].height;
                        v[6] = prect[i].x + prect[i].width;       v[7] = prect[i].y;
                        v += 8;
                        nfiltered++;
                    }
                }

                if (nfiltered > 0) {
                    glVertexAttribPointer(GLAMOR_VERTEX_POS, 2, GL_SHORT, GL_FALSE,
                                          2 * sizeof(short), vbo_offset);
                }

                glamor_put_vbo_space(screen);

                if (nfiltered == 0)
                    continue;

                glScissor(scissor.x1 + off_x,
                          scissor.y1 + off_y,
                          scissor.x2 - scissor.x1,
                          scissor.y2 - scissor.y1);
                glamor_glDrawArrays_GL_QUADS(glamor_priv, nfiltered);
            }
        }
    }

    ret = TRUE;

bail:
    glDisable(GL_SCISSOR_TEST);
    if (glamor_glsl_has_ints(glamor_priv)) {
        glVertexAttribDivisor(GLAMOR_VERTEX_SOURCE, 0);
        glDisableVertexAttribArray(GLAMOR_VERTEX_SOURCE);
        glVertexAttribDivisor(GLAMOR_VERTEX_POS, 0);
    }
    glDisableVertexAttribArray(GLAMOR_VERTEX_POS);

    return ret;
}

static void
glamor_poly_fill_rect_bail(DrawablePtr drawable,
                           GCPtr gc, int nrect, xRectangle *prect)
{
    glamor_fallback("to %p (%c)\n", drawable,
                    glamor_get_drawable_location(drawable));
    if (glamor_prepare_access(drawable, GLAMOR_ACCESS_RW) &&
        glamor_prepare_access_gc(gc)) {
        fbPolyFillRect(drawable, gc, nrect, prect);
    }
    glamor_finish_access_gc(gc);
    glamor_finish_access(drawable);
}

void
glamor_poly_fill_rect(DrawablePtr drawable,
                      GCPtr gc, int nrect, xRectangle *prect)
{
    if (glamor_poly_fill_rect_gl(drawable, gc, nrect, prect))
        return;
    glamor_poly_fill_rect_bail(drawable, gc, nrect, prect);
}