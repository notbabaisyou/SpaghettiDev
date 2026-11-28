/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xf86drm.h>

enum {
    WIDTH = 8,
    HEIGHT = 4
};

static void
require(bool condition, const char *message)
{
    if (condition)
        return;

    fprintf(stderr, "%s failed\n", message);
    exit(EXIT_FAILURE);
}

#define REQUIRE(condition) require((condition), #condition)

static void
check(xcb_connection_t *conn, xcb_void_cookie_t request)
{
    xcb_generic_error_t *err = xcb_request_check(conn, request);
    if (err) {
        fprintf(stderr, "X error %u (op %u.%u)\n", err->error_code,
                err->major_code, err->minor_code);
        exit(EXIT_FAILURE);
    }
}

static void
check_color(xcb_connection_t *conn, xcb_window_t win, uint32_t expected)
{
    xcb_get_image_cookie_t read =
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, 0, 0, WIDTH,
                      HEIGHT, UINT32_MAX);
    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, read, NULL);
    REQUIRE(reply && xcb_get_image_data_length(reply) == WIDTH * HEIGHT * 4);
    const uint32_t *pixels = (const void *)xcb_get_image_data(reply);
    for (unsigned i = 0; i < WIDTH * HEIGHT; i++) {
        if ((pixels[i] & 0xffffff) != expected) {
            fprintf(stderr, "pixel %u got %06x, expected %06x\n", i,
                    pixels[i] & 0xffffff, expected);
            exit(EXIT_FAILURE);
        }
    }
    free(reply);
}

int
main(int argc, char **argv)
{
    bool unavailable = argc == 2 && !strcmp(argv[1], "--unavailable");

    if (argc > 1 && !unavailable) {
        fprintf(stderr, "usage: %s [--unavailable]\n", argv[0]);
        return EXIT_FAILURE;
    }

    xcb_connection_t *client = xcb_connect(NULL, NULL);
    xcb_connection_t *observer = xcb_connect(NULL, NULL);
    REQUIRE(!xcb_connection_has_error(client));
    REQUIRE(!xcb_connection_has_error(observer));
    xcb_screen_t *screen =
        xcb_setup_roots_iterator(xcb_get_setup(client)).data;
    REQUIRE(screen && screen->root_depth == 24);
    xcb_window_t win = xcb_generate_id(client);
    xcb_pixmap_t pixmap = xcb_generate_id(client);
    xcb_gcontext_t gc = xcb_generate_id(client);
    uint32_t black = 0;
    xcb_rectangle_t rect = {0, 0, WIDTH, HEIGHT};

    xcb_dri3_query_version_reply_t *version = xcb_dri3_query_version_reply(
        client, xcb_dri3_query_version(client, 1, 4), NULL);
    REQUIRE(version && version->major_version == 1);
    xcb_present_query_capabilities_reply_t *caps;
    check(client, xcb_create_window_checked(
                      client, 24, win, screen->root, 0, 0, WIDTH, HEIGHT, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL, &black));
    check(client, xcb_map_window_checked(client, win));
    caps = xcb_present_query_capabilities_reply(
        client, xcb_present_query_capabilities(client, win), NULL);
    REQUIRE(caps);

    if (unavailable) {
        REQUIRE(version->minor_version <= 2);
        REQUIRE(!(caps->capabilities & XCB_PRESENT_CAPABILITY_SYNCOBJ));

        int placeholder = open("/dev/null", O_RDONLY | O_CLOEXEC);

        REQUIRE(placeholder >= 0);
        xcb_generic_error_t *error = xcb_request_check(
            client, xcb_dri3_import_syncobj_checked(
                        client, xcb_generate_id(client), win, placeholder));

        REQUIRE(error && error->error_code == XCB_IMPLEMENTATION);
        free(error);
        free(caps);
        free(version);
        xcb_disconnect(client);
        xcb_disconnect(observer);
        puts("PASS unavailable explicit sync");
        return EXIT_SUCCESS;
    }

    REQUIRE(version->minor_version == 4);
    REQUIRE(caps->capabilities & XCB_PRESENT_CAPABILITY_SYNCOBJ);
    free(caps);
    free(version);
    check(client,
          xcb_create_pixmap_checked(client, 24, pixmap, win, WIDTH, HEIGHT));
    check(client, xcb_create_gc_checked(client, gc, pixmap, 0, NULL));

    xcb_dri3_open_reply_t *render = xcb_dri3_open_reply(
        client, xcb_dri3_open(client, win, XCB_NONE), NULL);

    REQUIRE(render && render->nfd == 1);
    int fd = *xcb_dri3_open_reply_fds(client, render);

    free(render);
    uint32_t handle;
    int timeline_fd;
    REQUIRE(!drmSyncobjCreate(fd, 0, &handle));
    REQUIRE(!drmSyncobjHandleToFD(fd, handle, &timeline_fd));
    uint32_t id = xcb_generate_id(client);
    check(client,
          xcb_dri3_import_syncobj_checked(client, id, win, timeline_fd));
    uint32_t expected = black;
    /* Confirm each new frame waits even after the previous release. */

    for (unsigned frame = 1; frame <= 8; frame++) {
        uint64_t acquire = (UINT64_C(1) << 40) + 2 * frame;
        uint64_t release = acquire + 1;
        uint32_t color = (frame & 1) ? 0xc32542 : 0x19aee1;
        int notify = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        struct pollfd done = {.fd = notify, .events = POLLIN};

        check(client,
              xcb_change_gc_checked(client, gc, XCB_GC_FOREGROUND, &color));
        check(client,
              xcb_poly_fill_rectangle_checked(client, pixmap, gc, 1, &rect));
        REQUIRE(!drmSyncobjEventfd(fd, handle, release, notify, 0));
        check(client,
              xcb_present_pixmap_synced_checked(
                  client, win, pixmap, frame, 0, 0, 0, 0, 0, id, id, acquire,
                  release, XCB_PRESENT_OPTION_COPY, 0, 0, 0, 0, NULL));

        REQUIRE(poll(&done, 1, 40) == 0);
        check_color(observer, win, expected);
        REQUIRE(!drmSyncobjTimelineSignal(fd, &handle, &acquire, 1));
        REQUIRE(poll(&done, 1, 3000) == 1);
        close(notify);
        check(client,
              xcb_change_gc_checked(client, gc, XCB_GC_FOREGROUND, &black));
        check(client,
              xcb_poly_fill_rectangle_checked(client, pixmap, gc, 1, &rect));
        check_color(observer, win, color);
        expected = color;
    }

    /* Destroy a window with an untouched pixmap still awaiting acquire. */
    uint64_t pending = (UINT64_C(1) << 40) + 32;
    int notify = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct pollfd done = {.fd = notify, .events = POLLIN};
    REQUIRE(!drmSyncobjEventfd(fd, handle, pending + 1, notify, 0));
    check(client,
          xcb_present_pixmap_synced_checked(
              client, win, pixmap, 9, 0, 0, 0, 0, 0, id, id, pending,
              pending + 1, XCB_PRESENT_OPTION_COPY, 0, 0, 0, 0, NULL));
    REQUIRE(poll(&done, 1, 40) == 0);
    check(client, xcb_destroy_window_checked(client, win));
    REQUIRE(poll(&done, 1, 3000) == 1);
    close(notify);
    xcb_disconnect(client);
    xcb_disconnect(observer);
    close(fd);
    puts("PASS delayed acquire, independent client, reuse and release");
    return EXIT_SUCCESS;
}
