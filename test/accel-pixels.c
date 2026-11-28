/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

enum {
    WIDTH = 8,
    HEIGHT = 4,
    RGB_DEPTH = 24,
    ARGB_DEPTH = 32,
    ALPHA_DEPTH = 8,
    PIXEL_BYTES = 4,
    REDIRECT_X = 11,
    REDIRECT_Y = 13,
};

static const uint32_t background = 0x00000080;
static const uint32_t patch = 0x00402010;
static const uint32_t stripe = 0x00081732;

static void
check(xcb_connection_t *conn, xcb_void_cookie_t cookie, const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);

    if (error) {
        fprintf(stderr, "%s failed with X11 error %u (opcode %u.%u)\n",
                operation, error->error_code, error->major_code,
                error->minor_code);
        free(error);
        exit(EXIT_FAILURE);
    }
}

static xcb_pixmap_t
create_pixmap(xcb_connection_t *conn, xcb_window_t root, uint8_t depth)
{
    xcb_pixmap_t pixmap = xcb_generate_id(conn);

    check(conn,
          xcb_create_pixmap_checked(conn, depth, pixmap, root, WIDTH, HEIGHT),
          "CreatePixmap");
    return pixmap;
}

static xcb_gcontext_t
create_gc(xcb_connection_t *conn, xcb_drawable_t drawable)
{
    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t values[] = {0, 0};

    check(conn,
          xcb_create_gc_checked(conn, gc, drawable,
                                XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES,
                                values),
          "CreateGC");
    return gc;
}

static void
fill(xcb_connection_t *conn, xcb_drawable_t drawable, xcb_gcontext_t gc,
     xcb_rectangle_t rect, uint32_t color)
{
    check(conn, xcb_change_gc_checked(conn, gc, XCB_GC_FOREGROUND, &color),
          "ChangeGC");
    check(conn, xcb_poly_fill_rectangle_checked(conn, drawable, gc, 1, &rect),
          "PolyFillRectangle");
}

static void
put_argb(xcb_connection_t *conn, xcb_pixmap_t pixmap, xcb_gcontext_t gc,
         const uint32_t *pixels)
{
    uint8_t wire[WIDTH * HEIGHT * PIXEL_BYTES];
    bool little_endian =
        xcb_get_setup(conn)->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;

    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        for (size_t byte = 0; byte < PIXEL_BYTES; byte++) {
            size_t shift = little_endian ? byte : PIXEL_BYTES - 1 - byte;

            wire[i * PIXEL_BYTES + byte] = pixels[i] >> (8 * shift);
        }
    }

    check(conn,
          xcb_put_image_checked(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc,
                                WIDTH, HEIGHT, 0, 0, 0, ARGB_DEPTH,
                                sizeof(wire), wire),
          "PutImage");
}

static void
expect_image(xcb_connection_t *conn, xcb_drawable_t drawable, uint8_t depth,
             const uint32_t *expected, const char *stage)
{
    xcb_get_image_cookie_t cookie =
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable, 0, 0, WIDTH,
                      HEIGHT, UINT32_MAX);
    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, cookie, NULL);
    bool little_endian =
        xcb_get_setup(conn)->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
    size_t bytes = depth == ALPHA_DEPTH ? 1 : PIXEL_BYTES;
    uint32_t mask = UINT32_MAX >> (ARGB_DEPTH - depth);

    if (!reply || reply->depth != depth ||
        xcb_get_image_data_length(reply) != WIDTH * HEIGHT * bytes) {
        fprintf(stderr, "%s: unexpected GetImage reply for depth %u\n", stage,
                depth);
        free(reply);
        exit(EXIT_FAILURE);
    }

    const uint8_t *data = xcb_get_image_data(reply);

    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        uint32_t pixel = 0;

        for (size_t byte = 0; byte < bytes; byte++) {
            size_t shift = little_endian ? byte : bytes - 1 - byte;

            pixel |= (uint32_t)data[i * bytes + byte] << (8 * shift);
        }

        if ((pixel & mask) != expected[i]) {
            fprintf(stderr, "%s: pixel (%zu,%zu) got %08x, expected %08x\n",
                    stage, i % WIDTH, i / WIDTH, pixel & mask, expected[i]);
            free(reply);
            exit(EXIT_FAILURE);
        }
    }

    free(reply);
    printf("PASS %s (depth %u)\n", stage, depth);
}

static void
test_copy(xcb_connection_t *conn, xcb_screen_t *screen)
{
    xcb_pixmap_t src = create_pixmap(conn, screen->root, RGB_DEPTH);
    xcb_pixmap_t dst = create_pixmap(conn, screen->root, RGB_DEPTH);
    xcb_gcontext_t gc = create_gc(conn, src);
    xcb_rectangle_t full = {0, 0, WIDTH, HEIGHT};
    xcb_rectangle_t clip[] = {{1, 1, 3, 1}, {5, 2, 2, 1}};
    uint32_t expected[WIDTH * HEIGHT];
    uint32_t clip_count = sizeof(clip) / sizeof(*clip);

    fill(conn, src, gc, full, patch);
    fill(conn, src, gc, (xcb_rectangle_t){1, 0, 3, HEIGHT}, stripe);
    fill(conn, dst, gc, full, background);
    check(conn,
          xcb_set_clip_rectangles_checked(conn, XCB_CLIP_ORDERING_UNSORTED,
                                          gc, 0, 0, clip_count, clip),
          "SetClipRectangles");
    xcb_void_cookie_t clipped =
        xcb_copy_area_checked(conn, src, dst, gc, -2, 0, 0, 0, WIDTH, HEIGHT);

    check(conn, clipped, "clipped CopyArea");

    for (size_t y = 0; y < HEIGHT; y++) {
        for (size_t x = 0; x < WIDTH; x++) {
            bool first = y == 1 && x >= 2 && x < 4;
            bool second = y == 2 && x >= 5 && x < 7;
            uint32_t color = background;

            if (first || second)
                color = x >= 3 && x < 6 ? stripe : patch;
            expected[y * WIDTH + x] = color;
        }
    }

    expect_image(conn, dst, RGB_DEPTH, expected, "clipped CopyArea");

    uint32_t no_clip = XCB_NONE;

    check(conn, xcb_change_gc_checked(conn, gc, XCB_GC_CLIP_MASK, &no_clip),
          "clear GC clip");
    fill(conn, dst, gc, (xcb_rectangle_t){0, 0, 1, 1}, patch);
    expected[0] = patch;
    xcb_void_cookie_t copy =
        xcb_copy_area_checked(conn, dst, src, gc, 0, 0, 0, 0, WIDTH, HEIGHT);

    check(conn, copy, "copy after software write");
    expect_image(conn, src, RGB_DEPTH, expected, "write and copy round trip");

    xcb_void_cookie_t overlap = xcb_copy_area_checked(
        conn, src, src, gc, 0, 0, 1, 1, WIDTH - 1, HEIGHT - 1);

    check(conn, overlap, "overlapping CopyArea");

    for (size_t y = HEIGHT - 1; y > 0; y--) {
        for (size_t x = WIDTH - 1; x > 0; x--)
            expected[y * WIDTH + x] = expected[(y - 1) * WIDTH + x - 1];
    }

    expect_image(conn, src, RGB_DEPTH, expected, "overlapping CopyArea");
}

static xcb_render_pictformat_t
find_format(const xcb_render_query_pict_formats_reply_t *reply, uint8_t depth)
{
    xcb_render_pictforminfo_iterator_t it =
        xcb_render_query_pict_formats_formats_iterator(reply);

    for (; it.rem; xcb_render_pictforminfo_next(&it)) {
        const xcb_render_pictforminfo_t *f = it.data;
        const xcb_render_directformat_t *d = &f->direct;

        if (f->type != XCB_RENDER_PICT_TYPE_DIRECT || f->depth != depth)
            continue;
        if (depth == ALPHA_DEPTH && !d->red_mask && !d->green_mask &&
            !d->blue_mask) {
            if (d->alpha_shift == 0 && d->alpha_mask == 255)
                return f->id;
            continue;
        }
        if (d->red_shift != 16 || d->green_shift != 8 || d->blue_shift != 0)
            continue;
        if (d->red_mask != 255 || d->green_mask != 255 || d->blue_mask != 255)
            continue;
        if (depth == ARGB_DEPTH && d->alpha_shift == 24 &&
            d->alpha_mask == 255)
            return f->id;
        if (depth == RGB_DEPTH && d->alpha_mask == 0)
            return f->id;
    }

    fprintf(stderr, "Required Render format missing for depth %u\n", depth);
    exit(EXIT_FAILURE);
}

static xcb_render_picture_t
picture(xcb_connection_t *conn, xcb_drawable_t drawable,
        xcb_render_pictformat_t format)
{
    xcb_render_picture_t id = xcb_generate_id(conn);

    check(conn,
          xcb_render_create_picture_checked(conn, id, drawable, format, 0,
                                            NULL),
          "CreatePicture");
    return id;
}

static void
test_icon(xcb_connection_t *conn, xcb_screen_t *screen)
{
    xcb_render_query_pict_formats_reply_t *reply =
        xcb_render_query_pict_formats_reply(
            conn, xcb_render_query_pict_formats(conn), NULL);

    if (!reply) {
        fputs("Render QueryPictFormats failed\n", stderr);
        exit(EXIT_FAILURE);
    }

    xcb_render_pictformat_t argb = find_format(reply, ARGB_DEPTH);
    xcb_render_pictformat_t rgb = find_format(reply, RGB_DEPTH);
    xcb_render_pictformat_t a8 = find_format(reply, ALPHA_DEPTH);
    xcb_pixmap_t icon = create_pixmap(conn, screen->root, ARGB_DEPTH);
    xcb_pixmap_t mask = create_pixmap(conn, screen->root, ALPHA_DEPTH);
    xcb_pixmap_t target = create_pixmap(conn, screen->root, RGB_DEPTH);
    xcb_window_t window = xcb_generate_id(conn);
    xcb_pixmap_t redirected = xcb_generate_id(conn);
    uint32_t icon_pixels[WIDTH * HEIGHT];
    uint32_t mask_pixels[WIDTH * HEIGHT];
    uint32_t expected[WIDTH * HEIGHT];
    const uint32_t icon_row[WIDTH] = {
        0,          0xffff0000, 0x80800000, 0x80800000,
        0xffff0000, 0xffff0000, 0xff00ff00, 0xff00ff00,
    };
    const uint32_t mask_row[WIDTH] = {255, 255, 255, 128, 0, 128, 255, 0};
    const uint32_t result_row[WIDTH] = {
        0x000080, 0xff0000, 0x800040, 0x400060,
        0x000080, 0x800040, 0x00ff00, 0x000080,
    };

    free(reply);
    printf("Render formats ARGB32=%#x A8=%#x RGB24=%#x\n", argb, a8, rgb);

    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        icon_pixels[i] = icon_row[i % WIDTH];
        mask_pixels[i] = mask_row[i % WIDTH];
        expected[i] = result_row[i % WIDTH];
    }

    put_argb(conn, icon, create_gc(conn, icon), icon_pixels);
    expect_image(conn, icon, ARGB_DEPTH, icon_pixels, "uploaded app icon");

    xcb_gcontext_t mask_gc = create_gc(conn, mask);

    for (int x = 0; x < WIDTH; x++)
        fill(conn, mask, mask_gc, (xcb_rectangle_t){x, 0, 1, HEIGHT},
             mask_row[x]);

    expect_image(conn, mask, ALPHA_DEPTH, mask_pixels, "icon alpha mask");

    uint32_t override_redirect = 1;

    check(conn,
          xcb_create_window_checked(
              conn, RGB_DEPTH, window, screen->root, REDIRECT_X, REDIRECT_Y,
              WIDTH, HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
              screen->root_visual, XCB_CW_OVERRIDE_REDIRECT,
              &override_redirect),
          "CreateWindow");
    check(conn,
          xcb_composite_redirect_window_checked(
              conn, window, XCB_COMPOSITE_REDIRECT_MANUAL),
          "RedirectWindow");
    check(conn, xcb_map_window_checked(conn, window), "MapWindow");
    check(conn,
          xcb_composite_name_window_pixmap_checked(conn, window, redirected),
          "NameWindowPixmap");

    xcb_render_picture_t src = picture(conn, icon, argb);
    xcb_render_picture_t alpha = picture(conn, mask, a8);
    xcb_drawable_t outputs[] = {target, window};
    xcb_drawable_t readback[] = {target, redirected};
    const char *stages[] = {"icon on pixmap", "icon on redirected window"};

    for (size_t i = 0; i < sizeof(outputs) / sizeof(*outputs); i++) {
        xcb_drawable_t draw = outputs[i];
        xcb_render_picture_t dst = picture(conn, draw, rgb);
        xcb_rectangle_t full = {0, 0, WIDTH, HEIGHT};

        fill(conn, draw, create_gc(conn, draw), full, background);
        check(conn,
              xcb_render_composite_checked(conn, XCB_RENDER_PICT_OP_OVER, src,
                                           alpha, dst, 0, 0, 0, 0, 0, 0,
                                           WIDTH, HEIGHT),
              "RenderComposite");
        expect_image(conn, readback[i], RGB_DEPTH, expected, stages[i]);
    }
}

int
main(void)
{
    int screen_number = 0;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_number);

    if (xcb_connection_has_error(conn)) {
        fputs("Could not connect to DISPLAY\n", stderr);
        xcb_disconnect(conn);
        return EXIT_FAILURE;
    }

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);

    for (int i = 0; i < screen_number && screens.rem; i++)
        xcb_screen_next(&screens);

    xcb_screen_t *screen = screens.data;

    if (!screens.rem || screen->root_depth != RGB_DEPTH) {
        fputs("accel-pixels requires a depth-24 screen\n", stderr);
        xcb_disconnect(conn);
        return EXIT_FAILURE;
    }

    printf("Screen %d root depth %u\n", screen_number, screen->root_depth);
    test_copy(conn, screen);
    test_icon(conn, screen);
    xcb_disconnect(conn);
    return EXIT_SUCCESS;
}
