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

struct flip_buffer {
    xcb_pixmap_t pixmap;
    xcb_dri3_syncobj_t syncobj;
    uint32_t handle;
    int idle;
};

static void
check_completion(xcb_connection_t *conn, xcb_special_event_t *events,
                 uint32_t frame, uint8_t mode)
{
    struct pollfd socket = {.fd = xcb_get_file_descriptor(conn),
                            .events = POLLIN};

    enum { WAIT_SLICE_MS = 50, MAX_SLICES = 60 };

    for (unsigned i = 0; i < MAX_SLICES; i++) {
        xcb_generic_event_t *event = xcb_poll_for_special_event(conn, events);

        if (event) {
            const xcb_present_complete_notify_event_t *completion =
                (const void *)event;

            REQUIRE(completion->serial == frame);
            REQUIRE(completion->mode == mode);
            free(event);
            return;
        }

        int ready = poll(&socket, 1, WAIT_SLICE_MS);

        REQUIRE(ready >= 0);
        if (ready == 0) {
            xcb_get_input_focus_reply_t *reply = xcb_get_input_focus_reply(
                conn, xcb_get_input_focus(conn), NULL);

            REQUIRE(reply);
            free(reply);
        }
        REQUIRE(!xcb_connection_has_error(conn));
    }

    require(false, "Present completion timeout");
}

static void
test_flip(xcb_connection_t *client, xcb_connection_t *observer,
          xcb_window_t win, xcb_rectangle_t rect, int fd)
{
    struct flip_buffer buffers[2];
    xcb_present_event_t eid = xcb_generate_id(client);
    xcb_special_event_t *events =
        xcb_register_for_special_xge(client, &xcb_present_id, eid, NULL);
    xcb_gcontext_t gc = xcb_generate_id(client);
    uint32_t expected = 0;

    REQUIRE(events);
    check(client,
          xcb_present_select_input_checked(
              client, eid, win, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));

    for (unsigned i = 0; i < 2; i++) {
        struct flip_buffer *buffer = &buffers[i];
        int shared;

        buffer->pixmap = xcb_generate_id(client);
        buffer->syncobj = xcb_generate_id(client);
        buffer->idle = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        REQUIRE(buffer->idle >= 0);
        check(client,
              xcb_create_pixmap_checked(client, 24, buffer->pixmap, win,
                                        rect.width, rect.height));
        REQUIRE(!drmSyncobjCreate(fd, 0, &buffer->handle));
        REQUIRE(!drmSyncobjHandleToFD(fd, buffer->handle, &shared));
        check(client, xcb_dri3_import_syncobj_checked(client, buffer->syncobj,
                                                      win, shared));
    }

    check(client,
          xcb_create_gc_checked(client, gc, buffers[0].pixmap, 0, NULL));

    /* Distinct timelines keep a new acquire from advancing an old release. */
    for (unsigned frame = 0; frame < 4; frame++) {
        struct flip_buffer *next = &buffers[frame % 2];
        struct flip_buffer *old = &buffers[(frame + 1) % 2];
        struct pollfd previous = {.fd = old->idle, .events = POLLIN};
        struct pollfd active = {.fd = next->idle, .events = POLLIN};
        uint64_t acquire = (UINT64_C(1) << 40) + 2 * frame + 1;
        uint64_t release = acquire + 1;
        uint32_t color = frame % 2 ? 0x1ac332 : 0xd2184b;

        check(client,
              xcb_change_gc_checked(client, gc, XCB_GC_FOREGROUND, &color));
        check(client, xcb_poly_fill_rectangle_checked(client, next->pixmap,
                                                      gc, 1, &rect));
        REQUIRE(!drmSyncobjEventfd(fd, next->handle, release, next->idle, 0));
        check(client, xcb_present_pixmap_synced_checked(
                          client, win, next->pixmap, frame + 1, 0, 0, 0, 0, 0,
                          next->syncobj, next->syncobj, acquire, release, 0,
                          0, 0, 0, 0, NULL));

        check_color(observer, win, expected);
        REQUIRE(poll(&previous, 1, 40) == 0);
        REQUIRE(!xcb_poll_for_special_event(client, events));
        REQUIRE(!drmSyncobjTimelineSignal(fd, &next->handle, &acquire, 1));
        check_completion(client, events, frame + 1,
                         XCB_PRESENT_COMPLETE_MODE_FLIP);
        REQUIRE(poll(&active, 1, 0) == 0);

        if (frame) {
            eventfd_t value;

            REQUIRE(poll(&previous, 1, 3000) == 1);
            REQUIRE(!eventfd_read(old->idle, &value));
            uint32_t overwrite = 0;
            check(client, xcb_change_gc_checked(client, gc, XCB_GC_FOREGROUND,
                                                &overwrite));
            check(client, xcb_poly_fill_rectangle_checked(client, old->pixmap,
                                                          gc, 1, &rect));
        }

        check_color(observer, win, color);
        expected = color;
    }

    check(client, xcb_destroy_window_checked(client, win));
    struct pollfd retired = {.fd = buffers[1].idle, .events = POLLIN};

    REQUIRE(poll(&retired, 1, 3000) == 1);
    xcb_unregister_for_special_event(client, events);
    for (unsigned i = 0; i < 2; i++)
        close(buffers[i].idle);
}

int
main(int argc, char **argv)
{
    bool unavailable = argc == 2 && !strcmp(argv[1], "--unavailable");
    bool flip = argc == 2 && !strcmp(argv[1], "--flip");
    bool fallback = argc == 2 && !strcmp(argv[1], "--flip-fallback");

    bool fullscreen = flip || fallback;
    bool recognized = unavailable || fullscreen;

    if (argc > 1 && !recognized) {
        fprintf(stderr, "usage: %s [--unavailable|--flip|--flip-fallback]\n",
                argv[0]);
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
    uint32_t values[] = {0, 1};
    uint32_t black = values[0];
    xcb_rectangle_t rect = {
        .width = fullscreen ? screen->width_in_pixels : WIDTH,
        .height = fullscreen ? screen->height_in_pixels : HEIGHT,
    };

    xcb_dri3_query_version_reply_t *version = xcb_dri3_query_version_reply(
        client, xcb_dri3_query_version(client, 1, 4), NULL);
    REQUIRE(version && version->major_version == 1);
    xcb_present_query_capabilities_reply_t *caps;
    check(client,
          xcb_create_window_checked(
              client, 24, win, screen->root, 0, 0, rect.width, rect.height, 0,
              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
              XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values));
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
    xcb_dri3_open_reply_t *render = xcb_dri3_open_reply(
        client, xcb_dri3_open(client, win, XCB_NONE), NULL);

    REQUIRE(render && render->nfd == 1);
    int fd = *xcb_dri3_open_reply_fds(client, render);

    free(render);
    if (flip) {
        test_flip(client, observer, win, rect, fd);
        xcb_disconnect(client);
        xcb_disconnect(observer);
        close(fd);
        puts("PASS delayed flips, replacement retirement and unflip");
        return EXIT_SUCCESS;
    }

    check(client, xcb_create_pixmap_checked(client, 24, pixmap, win,
                                            rect.width, rect.height));
    check(client, xcb_create_gc_checked(client, gc, pixmap, 0, NULL));

    uint32_t options = fallback ? 0 : XCB_PRESENT_OPTION_COPY;
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
        check(client, xcb_present_pixmap_synced_checked(
                          client, win, pixmap, frame, 0, 0, 0, 0, 0, id, id,
                          acquire, release, options, 0, 0, 0, 0, NULL));

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
