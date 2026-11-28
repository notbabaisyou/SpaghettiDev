/* SPDX-License-Identifier: MIT */

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/xcb.h>

enum {
    WIDTH = 37,
    HEIGHT = 23,
    DEPTH = 24,
    BPP = 32,
    PIXMAPS = 4,
    STEPS = 512,
    CHECK_INTERVAL = 16,
    PADDING_BYTE = 0xa7,
    LARGE_WIDTH = 16385,
    LARGE_HEIGHT = 3,
    CHAIN_COPIES = 130,
};

static const uint32_t pixel_mask = (1u << DEPTH) - 1;
static const size_t row_bytes = WIDTH * sizeof(uint32_t);

struct pixmap {
    xcb_pixmap_t id;
    uint32_t pixels[HEIGHT][WIDTH];
};

static void
check(xcb_connection_t *conn, xcb_void_cookie_t cookie, const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);

    if (!error)
        return;
    fprintf(stderr, "%s failed with X11 error %u\n", operation,
            error->error_code);
    free(error);
    exit(EXIT_FAILURE);
}

static void
replace_pixmap(xcb_connection_t *conn, xcb_window_t root, xcb_gcontext_t gc,
                struct pixmap *pixmap)
{
    if (pixmap->id)
        xcb_free_pixmap(conn, pixmap->id);
    pixmap->id = xcb_generate_id(conn);
    memset(pixmap->pixels, 0, sizeof(pixmap->pixels));

    const uint32_t color = 0;
    xcb_rectangle_t full = { 0, 0, WIDTH, HEIGHT };

    xcb_create_pixmap(conn, DEPTH, pixmap->id, root, WIDTH, HEIGHT);
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &color);
    xcb_poly_fill_rectangle(conn, pixmap->id, gc, 1, &full);
}

static const xcb_format_t *
pixmap_format(xcb_connection_t *conn, uint8_t depth)
{
    xcb_format_iterator_t format =
        xcb_setup_pixmap_formats_iterator(xcb_get_setup(conn));

    for (; format.rem; xcb_format_next(&format)) {
        if (format.data->depth == depth)
            return format.data;
    }

    return NULL;
}

static uint32_t
read_pixel(const uint8_t *data, size_t bytes, int order)
{
    uint32_t pixel = 0;

    for (size_t i = 0; i < bytes; i++) {
        size_t byte = order == XCB_IMAGE_ORDER_LSB_FIRST ? bytes - 1 - i : i;

        pixel = (pixel << 8) | data[byte];
    }

    return pixel;
}

static void
verify(xcb_connection_t *conn, const struct pixmap *pixmap, unsigned step)
{
    xcb_get_image_cookie_t cookie =
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap->id, 0, 0,
                      WIDTH, HEIGHT, UINT32_MAX);
    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, cookie, NULL);
    const xcb_format_t *format = reply ? pixmap_format(conn, reply->depth) :
                                        NULL;

    if (!format) {
        fprintf(stderr, "No complete GetImage at step %u\n", step);
        free(reply);
        exit(EXIT_FAILURE);
    }

    size_t bytes = format->bits_per_pixel / 8;
    size_t pad = format->scanline_pad / 8;
    size_t stride = (WIDTH * bytes + pad - 1) / pad * pad;
    size_t length = xcb_get_image_data_length(reply);
    uint32_t mask = UINT32_MAX >> (BPP - reply->depth);

    if (length < stride * HEIGHT) {
        fprintf(stderr, "Unexpected GetImage pitch at step %u\n", step);
        free(reply);
        exit(EXIT_FAILURE);
    }

    const uint8_t *data = xcb_get_image_data(reply);
    int order = xcb_get_setup(conn)->image_byte_order;

    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            const uint8_t *pixel_bytes = data + y * stride + x * bytes;
            uint32_t pixel = read_pixel(pixel_bytes, bytes, order) & mask;

            if (pixel == pixmap->pixels[y][x])
                continue;
            fprintf(stderr, "Step %u, pixel (%u, %u) got %#x expected %#x\n",
                    step, x, y, pixel, pixmap->pixels[y][x]);
            free(reply);
            exit(EXIT_FAILURE);
        }
    }

    free(reply);
}

static void
sync_mapping(int fd, uint64_t flags)
{
    struct dma_buf_sync access = { .flags = flags };

    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &access) == 0)
        return;
    perror("DMA_BUF_IOCTL_SYNC");
    exit(EXIT_FAILURE);
}

static void
round_trip(xcb_connection_t *conn, xcb_window_t root, xcb_gcontext_t gc,
            struct pixmap *pixmap, unsigned step)
{
    xcb_dri3_buffer_from_pixmap_cookie_t cookie =
        xcb_dri3_buffer_from_pixmap(conn, pixmap->id);
    xcb_dri3_buffer_from_pixmap_reply_t *reply =
        xcb_dri3_buffer_from_pixmap_reply(conn, cookie, NULL);

    if (!reply || reply->nfd != 1) {
        fprintf(stderr, "BufferFromPixmap failed at step %u\n", step);
        free(reply);
        exit(EXIT_FAILURE);
    }

    int fd = *xcb_dri3_buffer_from_pixmap_reply_fds(conn, reply);
    size_t stride = reply->stride;
    size_t length = reply->size;

    if (stride <= row_bytes || length < stride * HEIGHT) {
        fprintf(stderr, "Expected a padded dma-buf at step %u\n", step);
        exit(EXIT_FAILURE);
    }

    uint8_t *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fd, 0);
    int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);

    if (map == MAP_FAILED || import_fd < 0) {
        perror("Failed to map or duplicate the dma-buf");
        exit(EXIT_FAILURE);
    }

    sync_mapping(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);

    for (unsigned y = 0; y < HEIGHT; y++)
        memset(map + y * stride + row_bytes, PADDING_BYTE,
               stride - row_bytes);

    sync_mapping(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

    xcb_pixmap_t imported = xcb_generate_id(conn);
    xcb_void_cookie_t import = xcb_dri3_pixmap_from_buffer_checked(
        conn, imported, root, length, WIDTH, HEIGHT, stride, DEPTH, BPP,
        import_fd);

    check(conn, import, "PixmapFromBuffer");
    free(reply);

    /* Write through the alias first, then drop the original pixmap. */
    uint32_t color = step & pixel_mask;
    xcb_rectangle_t pixel = { 0, 0, 1, 1 };

    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &color);
    xcb_poly_fill_rectangle(conn, imported, gc, 1, &pixel);
    pixmap->pixels[0][0] = color;
    verify(conn, pixmap, step);

    /* Both framebuffer access scopes refer to the same dma-buf. */
    xcb_copy_area(conn, pixmap->id, imported, gc, 0, 0, 1, 0, 1, 1);
    pixmap->pixels[0][1] = color;
    xcb_free_pixmap(conn, pixmap->id);
    pixmap->id = imported;
    verify(conn, pixmap, step);

    sync_mapping(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);

    for (unsigned y = 0; y < HEIGHT; y++) {
        for (size_t x = row_bytes; x < stride; x++) {
            if (map[y * stride + x] == PADDING_BYTE)
                continue;
            fprintf(stderr, "Changed padding at step %u, row %u\n", step, y);
            exit(EXIT_FAILURE);
        }
    }

    sync_mapping(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    munmap(map, length);
    close(fd);
}

static void
copy_chain(xcb_connection_t *conn, xcb_window_t root, xcb_gcontext_t gc)
{
    xcb_pixmap_t src = xcb_generate_id(conn);
    xcb_pixmap_t dst = xcb_generate_id(conn);
    xcb_rectangle_t full = { 0, 0, LARGE_WIDTH, LARGE_HEIGHT };
    xcb_rectangle_t marker = { LARGE_WIDTH - CHAIN_COPIES - 1, 1, 1, 1 };
    const uint32_t black = 0;
    const uint32_t color = 0x427bf1;

    xcb_create_pixmap(conn, DEPTH, src, root, LARGE_WIDTH, LARGE_HEIGHT);
    xcb_create_pixmap(conn, DEPTH, dst, root, LARGE_WIDTH, LARGE_HEIGHT);
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &black);
    xcb_poly_fill_rectangle(conn, src, gc, 1, &full);
    xcb_poly_fill_rectangle(conn, dst, gc, 1, &full);
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &color);
    xcb_poly_fill_rectangle(conn, src, gc, 1, &marker);

    for (unsigned i = 0; i < CHAIN_COPIES; i++) {
        xcb_pixmap_t previous = src;

        xcb_copy_area(conn, src, dst, gc, 0, 0, 1, 0, LARGE_WIDTH - 1,
                      LARGE_HEIGHT);
        src = dst;
        dst = previous;
    }

    xcb_free_pixmap(conn, dst);
    xcb_get_image_cookie_t cookie =
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, src, LARGE_WIDTH - 1,
                      marker.y, 1, 1, pixel_mask);
    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, cookie, NULL);
    size_t length = reply ? xcb_get_image_data_length(reply) : 0;
    int order = xcb_get_setup(conn)->image_byte_order;

    if (length != sizeof(color) ||
        (read_pixel(xcb_get_image_data(reply), sizeof(color), order) &
         pixel_mask) != color) {
        fputs("Large pixmap copy chain lost the marker pixel\n", stderr);
        free(reply);
        exit(EXIT_FAILURE);
    }

    free(reply);
    xcb_free_pixmap(conn, src);
}

static void
check_depth(xcb_connection_t *conn, xcb_window_t root, uint8_t depth)
{
    const xcb_format_t *format = pixmap_format(conn, depth);

    if (!format)
        return;

    xcb_pixmap_t src = xcb_generate_id(conn);
    struct pixmap dst = { .id = xcb_generate_id(conn) };
    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t mask = UINT32_MAX >> (BPP - depth);
    const uint32_t foreground = 0x3b557aac & mask;
    const uint32_t background = 0x97123216 & mask;
    xcb_rectangle_t full = { 0, 0, WIDTH, HEIGHT };
    xcb_rectangle_t area = { 2, 3, WIDTH - 4, HEIGHT - 6 };

    check(conn, xcb_create_pixmap_checked(conn, depth, src, root, WIDTH,
                                          HEIGHT), "CreatePixmap");
    check(conn, xcb_create_pixmap_checked(conn, depth, dst.id, root, WIDTH,
                                          HEIGHT), "CreatePixmap");
    check(conn, xcb_create_gc_checked(conn, gc, src, XCB_GC_FOREGROUND,
                                      &foreground), "CreateGC");
    xcb_poly_fill_rectangle(conn, src, gc, 1, &full);
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &background);
    xcb_poly_fill_rectangle(conn, dst.id, gc, 1, &full);
    xcb_copy_area(conn, src, dst.id, gc, 0, 0, area.x, area.y, area.width,
                  area.height);

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            bool inside_x = x >= area.x && x < area.x + area.width;
            bool inside_y = y >= area.y && y < area.y + area.height;

            dst.pixels[y][x] = inside_x && inside_y ? foreground : background;
        }
    }

    verify(conn, &dst, STEPS);

    xcb_dri3_buffer_from_pixmap_cookie_t cookie =
        xcb_dri3_buffer_from_pixmap(conn, dst.id);
    xcb_dri3_buffer_from_pixmap_reply_t *reply =
        xcb_dri3_buffer_from_pixmap_reply(conn, cookie, NULL);

    if (!reply || reply->nfd != 1 || reply->bpp != format->bits_per_pixel) {
        fprintf(stderr, "BufferFromPixmap failed for depth %u\n", depth);
        exit(EXIT_FAILURE);
    }

    int fd = *xcb_dri3_buffer_from_pixmap_reply_fds(conn, reply);
    xcb_pixmap_t imported = xcb_generate_id(conn);
    xcb_void_cookie_t import = xcb_dri3_pixmap_from_buffer_checked(
        conn, imported, root, reply->size, WIDTH, HEIGHT, reply->stride,
        depth, reply->bpp, fd);

    check(conn, import, "PixmapFromBuffer");
    free(reply);
    xcb_free_pixmap(conn, dst.id);
    dst.id = imported;
    verify(conn, &dst, STEPS);
    xcb_free_pixmap(conn, imported);
    xcb_free_pixmap(conn, src);
    xcb_free_gc(conn, gc);
    printf("PASS copy and DRI3 round trip at depth %u\n", depth);
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

    if (!screens.rem || screens.data->root_depth != DEPTH ||
        !xcb_get_extension_data(conn, &xcb_dri3_id)->present) {
        fputs("vaccum-pixmap requires a depth-24 screen with DRI3\n", stderr);
        xcb_disconnect(conn);
        return EXIT_FAILURE;
    }

    xcb_window_t root = screens.data->root;
    xcb_gcontext_t gc = xcb_generate_id(conn);
    const uint32_t exposures = 0;
    struct pixmap pixmaps[PIXMAPS] = {0};
    unsigned seed = 74989;

    check(conn, xcb_create_gc_checked(conn, gc, root,
                                      XCB_GC_GRAPHICS_EXPOSURES, &exposures),
          "CreateGC");

    for (unsigned i = 0; i < PIXMAPS; i++)
        replace_pixmap(conn, root, gc, &pixmaps[i]);

    for (unsigned step = 0; step < STEPS; step++) {
        struct pixmap *dst = &pixmaps[rand_r(&seed) % PIXMAPS];
        struct pixmap *src = &pixmaps[rand_r(&seed) % PIXMAPS];
        unsigned x = rand_r(&seed) % WIDTH;
        unsigned y = rand_r(&seed) % HEIGHT;
        unsigned w = 1 + rand_r(&seed) % (WIDTH - x);
        unsigned h = 1 + rand_r(&seed) % (HEIGHT - y);
        unsigned op = rand_r(&seed) % 8;

        switch (op) {
        case 0:
            replace_pixmap(conn, root, gc, dst);
            break;
        case 1:
            round_trip(conn, root, gc, dst, step);
            break;
        case 2:
        case 3: {
            uint32_t color = rand_r(&seed) & pixel_mask;
            xcb_rectangle_t rect = { x, y, w, h };

            xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &color);
            xcb_poly_fill_rectangle(conn, dst->id, gc, 1, &rect);

            for (unsigned row = y; row < y + h; row++)
                for (unsigned col = x; col < x + w; col++)
                    dst->pixels[row][col] = color;

            break;
        }
        default: {
            unsigned sx = rand_r(&seed) % (WIDTH - w + 1);
            unsigned sy = rand_r(&seed) % (HEIGHT - h + 1);
            uint32_t snapshot[HEIGHT][WIDTH];

            memcpy(snapshot, src->pixels, sizeof(snapshot));
            xcb_copy_area(conn, src->id, dst->id, gc, sx, sy, x, y, w, h);

            for (unsigned row = 0; row < h; row++)
                memcpy(&dst->pixels[y + row][x], &snapshot[sy + row][sx],
                       w * sizeof(uint32_t));

            break;
        }
        }

        if (!(step % CHECK_INTERVAL)) {
            for (unsigned i = 0; i < PIXMAPS; i++)
                verify(conn, &pixmaps[i], step);
        }
    }

    for (unsigned i = 0; i < PIXMAPS; i++)
        verify(conn, &pixmaps[i], STEPS);

    copy_chain(conn, root, gc);

    const uint8_t depths[] = { 16, 24, 30, 32 };

    for (size_t i = 0; i < sizeof(depths); i++)
        check_depth(conn, root, depths[i]);

    xcb_disconnect(conn);
    printf("PASS %u randomized pixmap operations and dma-buf padding\n",
           STEPS);
    printf("PASS %u large pixmap copies\n", CHAIN_COPIES);
    return EXIT_SUCCESS;
}
