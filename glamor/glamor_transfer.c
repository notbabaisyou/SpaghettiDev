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

static const glamor_facet glamor_facet_force_alpha = {
    .name    = "force_alpha",
    .vs_vars =
        "in vec2 position;\n"
        "in vec2 texcoord;\n"
        "out vec2 v_texcoord;\n",
    .vs_exec =
        "gl_Position = vec4(position, 0.0, 1.0);\n"
        "v_texcoord = texcoord;\n",
    .fs_vars =
        "uniform sampler2D tex;\n"
        "in vec2 v_texcoord;\n",
    .fs_exec =
        "frag_color = vec4(texture(tex, v_texcoord).rgb, 1.0);\n",
};

static void
glamor_upload_24bpp_as_32(ScreenPtr screen,
                          glamor_screen_private *glamor_priv,
                          glamor_pixmap_fbo *fbo, BoxPtr box,
                          int dst_x, int dst_y, int w, int h,
                          const struct glamor_format *f,
                          uint8_t *src_line, uint32_t byte_stride,
                          int bytes_per_pixel)
{
    int fbo_w = box->x2 - box->x1;
    int fbo_h = box->y2 - box->y1;
    GLuint staging_tex, vbo;
    glamor_program *prog = &glamor_priv->conv_alpha_program;

    if (!prog->prog) {
        if (!glamor_build_program(screen, prog,
                                  &glamor_facet_force_alpha,
                                  NULL, NULL, NULL)) {
            FatalError("glamor: failed to build alpha_force program\n");
            return;
        }
    }

    glGenTextures(1, &staging_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, staging_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, f->internalformat, w, h, 0,
                 f->format, f->type, NULL);

    if (glamor_priv->has_unpack_subimage) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, byte_stride / bytes_per_pixel);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        f->format, f->type, src_line);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        for (int y = 0; y < h; y++, src_line += byte_stride)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1,
                            f->format, f->type, src_line);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo->fb);
    glViewport(0, 0, fbo_w, fbo_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    glUseProgram(prog->prog);
    glUniform1i(glGetUniformLocation(prog->prog, "tex"), 0);

    GLfloat x1n = 2.0f *  dst_x      / (GLfloat)fbo_w - 1.0f;
    GLfloat x2n = 2.0f * (dst_x + w) / (GLfloat)fbo_w - 1.0f;
    GLfloat y1n = 2.0f *  dst_y      / (GLfloat)fbo_h - 1.0f;
    GLfloat y2n = 2.0f * (dst_y + h) / (GLfloat)fbo_h - 1.0f;

    const GLfloat verts[] = {
        x1n, y1n,  0.0f, 0.0f,
        x2n, y1n,  1.0f, 0.0f,
        x1n, y2n,  0.0f, 1.0f,
        x2n, y2n,  1.0f, 1.0f,
    };

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

    GLint pos_loc = glGetAttribLocation(prog->prog, "position");
    GLint tc_loc  = glGetAttribLocation(prog->prog, "texcoord");

    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), (void *)0);
    glVertexAttribPointer(tc_loc,  2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), (void *)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(pos_loc);
    glEnableVertexAttribArray(tc_loc);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(pos_loc);
    glDisableVertexAttribArray(tc_loc);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vbo);
    glDeleteTextures(1, &staging_tex);
}

/*
 * Write a region of bits into a drawable's backing pixmap
 */
void
glamor_upload_boxes(DrawablePtr drawable, BoxPtr in_boxes, int in_nbox,
                    int dx_src, int dy_src,
                    int dx_dst, int dy_dst,
                    uint8_t *bits, uint32_t byte_stride)
{
    ScreenPtr                   screen = drawable->pScreen;
    glamor_screen_private       *glamor_priv = glamor_get_screen_private(screen);
    PixmapPtr                   pixmap = glamor_get_drawable_pixmap(drawable);
    glamor_pixmap_private       *priv = glamor_get_pixmap_private(pixmap);
    int                         box_index;
    const struct glamor_format *f = glamor_format_for_pixmap(pixmap);
    int                         bytes_per_pixel = PICT_FORMAT_BPP(f->render_format) >> 3;
    Bool                        force_alpha = glamor_drawable_effective_depth(drawable) == 24 &&
                                              pixmap->drawable.depth == 32;

    glamor_make_current(glamor_priv);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (!force_alpha && glamor_priv->has_unpack_subimage)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, byte_stride / bytes_per_pixel);

    glamor_pixmap_loop(priv, box_index) {
        BoxPtr            box = glamor_pixmap_box_at(priv, box_index);
        glamor_pixmap_fbo *fbo = glamor_pixmap_fbo_at(priv, box_index);
        BoxPtr            boxes = in_boxes;
        int               nbox = in_nbox;
        const int         stride_px = byte_stride / bytes_per_pixel;

        if (!force_alpha)
            glamor_bind_texture(glamor_priv, GL_TEXTURE0, fbo, TRUE);

        while (nbox--) {
            int x1 = MAX(boxes->x1 + dx_dst, box->x1);
            int x2 = MIN(boxes->x2 + dx_dst, box->x2);
            int y1 = MAX(boxes->y1 + dy_dst, box->y1);
            int y2 = MIN(boxes->y2 + dy_dst, box->y2);

            size_t ofs = (y1 - dy_dst + dy_src) * byte_stride;
            ofs += (x1 - dx_dst + dx_src) * bytes_per_pixel;

            uint8_t *src_line = bits + ofs;

            boxes++;

            if (x2 <= x1 || y2 <= y1)
                continue;

            if (force_alpha) {
                glamor_upload_24bpp_as_32(screen, glamor_priv, fbo, box,
                                          x1 - box->x1, y1 - box->y1,
                                          x2 - x1, y2 - y1,
                                          f, src_line, byte_stride,
                                          bytes_per_pixel);
            } else if (glamor_priv->has_unpack_subimage ||
                       x2 - x1 == stride_px) {
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                                x1 - box->x1, y1 - box->y1,
                                x2 - x1, y2 - y1,
                                f->format, f->type, src_line);
            } else {
                for (; y1 < y2; y1++, src_line += byte_stride)
                    glTexSubImage2D(GL_TEXTURE_2D, 0,
                                    x1 - box->x1, y1 - box->y1,
                                    x2 - x1, 1,
                                    f->format, f->type, src_line);
            }
        }
    }

    if (!force_alpha && glamor_priv->has_unpack_subimage)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/*
 * Upload a region of data
 */

void
glamor_upload_region(DrawablePtr drawable, RegionPtr region,
                     int region_x, int region_y,
                     uint8_t *bits, uint32_t byte_stride)
{
    glamor_upload_boxes(drawable, RegionRects(region), RegionNumRects(region),
                        -region_x, -region_y,
                        0, 0,
                        bits, byte_stride);
}

/*
 * Read stuff from the drawable's backing pixmap FBOs and write to memory
 */
void
glamor_download_boxes(DrawablePtr drawable, BoxPtr in_boxes, int in_nbox,
                      int dx_src, int dy_src,
                      int dx_dst, int dy_dst,
                      uint8_t *bits, uint32_t byte_stride)
{
    ScreenPtr screen = drawable->pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    PixmapPtr pixmap = glamor_get_drawable_pixmap(drawable);
    glamor_pixmap_private *priv = glamor_get_pixmap_private(pixmap);
    int box_index;
    const struct glamor_format *f = glamor_format_for_pixmap(pixmap);
    int bytes_per_pixel = PICT_FORMAT_BPP(f->render_format) >> 3;

    glamor_make_current(glamor_priv);

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (glamor_priv->has_pack_subimage)
        glPixelStorei(GL_PACK_ROW_LENGTH, byte_stride / bytes_per_pixel);

    glamor_pixmap_loop(priv, box_index) {
        BoxPtr                  box = glamor_pixmap_box_at(priv, box_index);
        glamor_pixmap_fbo       *fbo = glamor_pixmap_fbo_at(priv, box_index);
        BoxPtr                  boxes = in_boxes;
        int                     nbox = in_nbox;

        /* This should not be called on GLAMOR_FBO_NO_FBO-allocated pixmaps. */
        assert(fbo->fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo->fb);

        while (nbox--) {

            /* compute drawable coordinates */
            int                     x1 = MAX(boxes->x1 + dx_src, box->x1);
            int                     x2 = MIN(boxes->x2 + dx_src, box->x2);
            int                     y1 = MAX(boxes->y1 + dy_src, box->y1);
            int                     y2 = MIN(boxes->y2 + dy_src, box->y2);
            size_t ofs = (y1 - dy_src + dy_dst) * byte_stride;
            ofs += (x1 - dx_src + dx_dst) * bytes_per_pixel;

            boxes++;

            if (x2 <= x1 || y2 <= y1)
                continue;

            if (glamor_priv->has_pack_subimage ||
                x2 - x1 == byte_stride / bytes_per_pixel) {
                glReadPixels(x1 - box->x1, y1 - box->y1, x2 - x1, y2 - y1, f->format, f->type, bits + ofs);
            } else {
                for (; y1 < y2; y1++, ofs += byte_stride)
                    glReadPixels(x1 - box->x1, y1 - box->y1, x2 - x1, 1, f->format, f->type, bits + ofs);
            }
        }
    }
    if (glamor_priv->has_pack_subimage)
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}
