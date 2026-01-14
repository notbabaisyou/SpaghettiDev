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
#include "glamor_transfer.h"
#include "glamor_transform.h"

static inline size_t
round_up_pow2(size_t n, size_t align)
{
    if (n == 0) {
        return 0;
    } else {
        return (n + align - 1) & ~(align - 1);
    }
}

static const char vs_vars_bitmap[] =
    "in vec4 primitive;\n"
    "in vec2 source;\n"
    "out vec2 bitmap_pos;\n";

static const char vs_exec_bitmap[] =
    "vec2 pos = primitive.zw * vec2(gl_VertexID & 1, (gl_VertexID & 2) >> 1);\n"
    GLAMOR_POS(gl_Position, (primitive.xy + pos))
    "bitmap_pos = source + pos;\n";

static const char fs_vars_bitmap[] =
    "in vec2 bitmap_pos;\n"
    "uniform usampler2D bitmap_tex;\n"
    "uniform vec4 fg;\n"
    "uniform vec4 bg;\n"
    "uniform int bitorder;\n";

static const char fs_exec_bitmap[] =
    "ivec2 tc = ivec2(bitmap_pos);\n"
    "uint bit = uint(tc.x & 7);\n"
    "if (bitorder == 1) bit = 7u - bit;\n"
    "tc.x >>= 3;\n"
    "uint byte_val = texelFetch(bitmap_tex, tc, 0).r;\n"
    "frag_color = ((byte_val >> bit) & 1u) != 0u ? fg : bg;\n";

static Bool
bitmap_use(DrawablePtr draw, GCPtr gc, glamor_program *prog, _X_UNUSED void *arg)
{
    if (!glamor_set_solid(draw, gc, TRUE, prog->fg_uniform))
    {
        return FALSE;
    }
    else
    {
        glamor_set_color(draw, gc->bgPixel, prog->bg_uniform);
        return TRUE;
    }
}

static const glamor_facet glamor_facet_xybitmap = {
    .name = "xybitmap",
    .version = 130,
    .vs_vars = vs_vars_bitmap,
    .vs_exec = vs_exec_bitmap,
    .fs_vars = fs_vars_bitmap,
    .fs_exec = fs_exec_bitmap,
    .locations = glamor_program_location_fg | glamor_program_location_bg |
                 glamor_program_location_font,
    .use = bitmap_use,
};

static Bool
glamor_put_image_xybitmap_gl(DrawablePtr drawable, GCPtr gc,
                             int x, int y, int w, int h,
                             int leftPad, int format, char *bits)
{
    ScreenPtr screen = drawable->pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    PixmapPtr pixmap = glamor_get_drawable_pixmap(drawable);
    glamor_pixmap_private *pixmap_priv;
    glamor_program *prog = &glamor_priv->put_bitmap_prog;
    image_state_t *img_state = &glamor_priv->image_state;
    uint32_t    byte_stride = PixmapBytePad(w, drawable->depth);
    RegionRec   region;
    BoxRec      box;
    GLenum      old_active_tex = 0;
    GLint       old_tex_unit1 = 0;
    GLshort     *vbo;
    char        *vbo_offset;
    int off_x, off_y, box_index;

    if (format != XYBitmap)
        goto bail;

    if (w < 128)
        goto bail;

    if (h < 128)
        goto bail;

    pixmap_priv = glamor_get_pixmap_private(pixmap);
    if (!GLAMOR_PIXMAP_PRIV_HAS_FBO(pixmap_priv))
        return FALSE;

    if (drawable->depth != 1)
        goto bail;

    if (gc->alu != GXcopy)
        goto bail;

    if (!glamor_pm_is_solid(gc->depth, gc->planemask))
        goto bail;

    /* Build shader program lazily. */
    if (!prog->prog && !prog->failed) {
        if (!glamor_build_program(screen, prog,
                                  &glamor_facet_xybitmap,
                                  NULL, NULL, NULL)) {
            goto bail;
        }
    }

    if (!glamor_use_program(&pixmap->drawable, gc, prog, NULL))
        goto bail;

    /* Cache bitorder uniform location. */
    if (img_state->last_bitmap_prog != prog->prog) {
        img_state->bitorder_uniform =
            glGetUniformLocation(prog->prog, "bitorder");
        img_state->last_bitmap_prog = prog->prog;
    }

    if (img_state->bitorder_uniform != -1) {
#if BITMAP_BIT_ORDER == MSBFirst
        glUniform1i(img_state->bitorder_uniform, 1);
#else
        glUniform1i(img_state->bitorder_uniform, 0);
#endif
    }

    /* Ensure bitmap texture exists. */
    if (!img_state->bitmap_tex) {
        glGenTextures(1, &img_state->bitmap_tex);
        img_state->bitmap_w = 0;
        img_state->bitmap_h = 0;
    }

    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_tex);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex_unit1);

    glBindTexture(GL_TEXTURE_2D, img_state->bitmap_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* Round up texture size to reduce realloc churn and align to cache-friendly sizes. */
    GLsizei tex_w = (GLsizei)round_up_pow2((size_t)byte_stride, 256);
    GLsizei tex_h = (GLsizei)round_up_pow2((size_t)h, 64);

    if (tex_w != img_state->bitmap_w || tex_h != img_state->bitmap_h) {
        GLint old_align;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_align);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI,
                     tex_w, tex_h, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
        glPixelStorei(GL_UNPACK_ALIGNMENT, old_align);

        if (glGetError()) {
            glBindTexture(GL_TEXTURE_2D, old_tex_unit1);
            glActiveTexture(old_active_tex);
            return FALSE;
        }

        img_state->bitmap_w = tex_w;
        img_state->bitmap_h = tex_h;
    }

    GLint old_align;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_align);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    byte_stride, h,
                    GL_RED_INTEGER, GL_UNSIGNED_BYTE, bits);
    glPixelStorei(GL_UNPACK_ALIGNMENT, old_align);

    /* Bind 'font' sampler to texture unit 1. */
    if (prog->font_uniform != -1)
        glUniform1i(prog->font_uniform, 1);

    /* Build a 6-short VBO with primitive + source. */
    vbo = glamor_get_vbo_space(screen,
                               (unsigned)(6 * sizeof(GLshort)),
                               &vbo_offset);
    if (!vbo) {
        glBindTexture(GL_TEXTURE_2D, old_tex_unit1);
        glActiveTexture(old_active_tex);
        return FALSE;
    }

    vbo[0] = (GLshort)x;
    vbo[1] = (GLshort)y;
    vbo[2] = (GLshort)w;
    vbo[3] = (GLshort)h;
    vbo[4] = (GLshort)leftPad;
    vbo[5] = 0;

    glamor_put_vbo_space(screen);

    glEnableVertexAttribArray(GLAMOR_VERTEX_POS);
    glVertexAttribPointer(GLAMOR_VERTEX_POS, 4, GL_SHORT, GL_FALSE,
                          6 * (GLsizei)sizeof(GLshort), vbo_offset);
    glVertexAttribDivisor(GLAMOR_VERTEX_POS, 1);

    glEnableVertexAttribArray(GLAMOR_VERTEX_SOURCE);
    glVertexAttribPointer(GLAMOR_VERTEX_SOURCE, 2, GL_SHORT, GL_FALSE,
                          6 * (GLsizei)sizeof(GLshort),
                          vbo_offset + 4 * (int)sizeof(GLshort));
    glVertexAttribDivisor(GLAMOR_VERTEX_SOURCE, 1);

    glEnable(GL_SCISSOR_TEST);

    glamor_pixmap_loop(pixmap_priv, box_index) {
        if (!glamor_set_destination_drawable(drawable, box_index,
                                             TRUE, FALSE,
                                             prog->matrix_uniform,
                                             &off_x, &off_y)) {
            continue;
        }

        if (gc->pCompositeClip) {
            int i;
            int nclip = RegionNumRects(gc->pCompositeClip);
            BoxPtr boxes = RegionRects(gc->pCompositeClip);

            for (i = 0; i < nclip; i++) {
                glScissor(boxes[i].x1 + off_x,
                          boxes[i].y1 + off_y,
                          boxes[i].x2 - boxes[i].x1,
                          boxes[i].y2 - boxes[i].y1);
                glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
            }
        } else {
            glScissor(drawable->x + off_x, drawable->y + off_y,
                      drawable->width, drawable->height);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
        }
    }

    glDisable(GL_SCISSOR_TEST);

    glVertexAttribDivisor(GLAMOR_VERTEX_SOURCE, 0);
    glDisableVertexAttribArray(GLAMOR_VERTEX_SOURCE);
    glVertexAttribDivisor(GLAMOR_VERTEX_POS, 0);
    glDisableVertexAttribArray(GLAMOR_VERTEX_POS);

    glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex_unit1);
    glActiveTexture((GLenum)old_active_tex);
    
    return TRUE;
bail:
    return FALSE;
}

static Bool
glamor_put_image_gl(DrawablePtr drawable, GCPtr gc, int depth, int x, int y,
                    int w, int h, int leftPad, int format, char *bits)
{
    ScreenPtr screen = drawable->pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    PixmapPtr pixmap = glamor_get_drawable_pixmap(drawable);
    glamor_pixmap_private *pixmap_priv;
    image_state_t *img_state = &glamor_priv->image_state;
    uint32_t    byte_stride = PixmapBytePad(w, drawable->depth);
    RegionRec   region;
    BoxRec      box;
    int         off_x, off_y;
    size_t      transfer_size = (h * byte_stride);

    pixmap_priv = glamor_get_pixmap_private(pixmap);

    if (!GLAMOR_PIXMAP_PRIV_HAS_FBO(pixmap_priv))
        return FALSE;

    if (gc->alu != GXcopy)
        goto bail;

    if (!glamor_pm_is_solid(gc->depth, gc->planemask))
        goto bail;

    if (format == XYPixmap && drawable->depth == 1 && leftPad == 0)
        format = ZPixmap;

    if (format != ZPixmap)
        goto bail;

    x += drawable->x;
    y += drawable->y;
    box.x1 = x;
    box.y1 = y;
    box.x2 = box.x1 + w;
    box.y2 = box.y1 + h;
        
    RegionInit(&region, &box, 1);
    RegionIntersect(&region, &region, gc->pCompositeClip);

    glamor_get_drawable_deltas(drawable, pixmap, &off_x, &off_y);
    if (off_x || off_y) {
        x += off_x;
        y += off_y;
        RegionTranslate(&region, off_x, off_y);
    }

    glamor_make_current(glamor_priv);

    if (glamor_priv->enable_pbo_uploads) {
        pbo_slot_t *slot = NULL;

        if (pbo_acquire_upload(glamor_priv, img_state, transfer_size, &slot)) {
            GLint old_pbo = 0;

            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &old_pbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->id);

            if (slot->persistent && slot->map) {
                memcpy(slot->map, bits, transfer_size);

                if (!slot->coherent) {
                    glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER,
                                             0, (GLsizeiptr)transfer_size);
                }

                /* Ensure CPU writes are visible to GL. */
                glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);

                glamor_upload_region(drawable, &region, x, y,
                                     (const uint8_t *)(uintptr_t)0, byte_stride);
            } else {
                /* Orphan/map/unmap path (non-persistent PBO). */
                GLbitfield map_flags = GL_MAP_WRITE_BIT |
                                       GL_MAP_INVALIDATE_BUFFER_BIT |
                                       GL_MAP_UNSYNCHRONIZED_BIT;

                void *map = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0,
                                             (GLsizeiptr)transfer_size,
                                             map_flags);
                if (map) {
                    memcpy(map, bits, transfer_size);
                    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
                    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

                    glamor_upload_region(drawable, &region, x, y,
                                         (const uint8_t *)(uintptr_t)0, byte_stride);
                } else {
                    /* Mapping failed: fall back to direct upload. */
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, old_pbo);
                    glamor_upload_region(drawable, &region, x, y,
                                         (const uint8_t *)bits, byte_stride);
                    goto done;
                }
            }

            /* ASYNC: record a new fence for future reuse, do not wait here. */
            pbo_wait_fence(slot);
            slot->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, old_pbo);
            RegionUninit(&region);
            goto done;
        }
    }

    glamor_upload_region(drawable, &region, x, y, (uint8_t *) bits, byte_stride);

done:
    RegionUninit(&region);
    return TRUE;
bail:
    return FALSE;
}

static void
glamor_put_image_bail(DrawablePtr drawable, GCPtr gc, int depth, int x, int y,
                      int w, int h, int leftPad, int format, char *bits)
{
    if (glamor_prepare_access_box(drawable, GLAMOR_ACCESS_RW, x, y, w, h))
        fbPutImage(drawable, gc, depth, x, y, w, h, leftPad, format, bits);
    glamor_finish_access(drawable);
}

void
glamor_put_image(DrawablePtr drawable, GCPtr gc, int depth, int x, int y,
                 int w, int h, int leftPad, int format, char *bits)
{
    if (glamor_put_image_xybitmap_gl(drawable, gc, x, y, w, h, leftPad, format, bits))
        return;
    else if (glamor_put_image_gl(drawable, gc, depth, x, y, w, h, leftPad, format, bits))
        return;
    else
        glamor_put_image_bail(drawable, gc, depth, x, y, w, h, leftPad, format, bits);
}

static Bool
glamor_get_image_gl(DrawablePtr drawable, int x, int y, int w, int h,
                    unsigned int format, unsigned long plane_mask, char *d)
{
    PixmapPtr pixmap = glamor_get_drawable_pixmap(drawable);
    glamor_pixmap_private *pixmap_priv;
    uint32_t    byte_stride = PixmapBytePad(w, drawable->depth);
    BoxRec      box;
    int         off_x, off_y;

    pixmap_priv = glamor_get_pixmap_private(pixmap);
    if (!GLAMOR_PIXMAP_PRIV_HAS_FBO(pixmap_priv))
        goto bail;

    if (format != ZPixmap)
        goto bail;

    glamor_get_drawable_deltas(drawable, pixmap, &off_x, &off_y);
    box.x1 = x;
    box.x2 = x + w;
    box.y1 = y;
    box.y2 = y + h;
    glamor_download_boxes(drawable, &box, 1,
                          drawable->x + off_x, drawable->y + off_y,
                          -x, -y,
                          (uint8_t *) d, byte_stride);

    if (!glamor_pm_is_solid(glamor_drawable_effective_depth(drawable), plane_mask)) {
        FbStip pm = fbReplicatePixel(plane_mask, drawable->bitsPerPixel);
        FbStip *dst = (void *)d;
        uint32_t dstStride = byte_stride / sizeof(FbStip);

        for (int i = 0; i < dstStride * h; i++)
            dst[i] &= pm;
    }

    return TRUE;
bail:
    return FALSE;
}

static void
glamor_get_image_bail(DrawablePtr drawable, int x, int y, int w, int h,
                      unsigned int format, unsigned long plane_mask, char *d)
{
    if (glamor_prepare_access_box(drawable, GLAMOR_ACCESS_RO, x, y, w, h))
        fbGetImage(drawable, x, y, w, h, format, plane_mask, d);
    glamor_finish_access(drawable);
}

void
glamor_get_image(DrawablePtr drawable, int x, int y, int w, int h,
                 unsigned int format, unsigned long plane_mask, char *d)
{
    if (glamor_get_image_gl(drawable, x, y, w, h, format, plane_mask, d))
        return;
    else
        glamor_get_image_bail(drawable, x, y, w, h, format, plane_mask, d);
}
