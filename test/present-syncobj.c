/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#ifdef __linux__
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#endif
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/sync.h>
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

struct producer_mapping {
    uint8_t *pixels;
    size_t length;
    unsigned width, height, stride;
    int fd;
};

static struct producer_mapping
map_pixmap(xcb_connection_t *conn, xcb_pixmap_t pixmap)
{
    xcb_dri3_buffer_from_pixmap_cookie_t cookie =
        xcb_dri3_buffer_from_pixmap(conn, pixmap);
    xcb_dri3_buffer_from_pixmap_reply_t *reply =
        xcb_dri3_buffer_from_pixmap_reply(conn, cookie, NULL);

    REQUIRE(reply && reply->nfd == 1 && reply->bpp == 32);
    REQUIRE(reply->stride >= reply->width * sizeof(uint32_t));
    REQUIRE(reply->size >= (size_t)reply->stride * reply->height);

    struct producer_mapping map = {
        .width = reply->width,
        .height = reply->height,
        .stride = reply->stride,
        .length = reply->size,
        .fd = *xcb_dri3_buffer_from_pixmap_reply_fds(conn, reply),
    };

    free(reply);
    map.pixels = mmap(NULL, map.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                      map.fd, 0);
    REQUIRE(map.pixels != MAP_FAILED);
    return map;
}

static void
write_pixmap(const struct producer_mapping *map, uint32_t color)
{
#ifdef __linux__
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START |
                                          DMA_BUF_SYNC_WRITE };

    REQUIRE(ioctl(map->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
#endif

    for (unsigned y = 0; y < map->height; y++) {
        uint32_t *row = (void *)(map->pixels + y * map->stride);

        for (unsigned x = 0; x < map->width; x++)
            row[x] = color;
    }

#ifdef __linux__
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    REQUIRE(ioctl(map->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
#endif
}

static void
unmap_pixmap(const struct producer_mapping *map)
{
    REQUIRE(munmap(map->pixels, map->length) == 0);
    close(map->fd);
}

struct flip_buffer {
    struct producer_mapping map;
    xcb_pixmap_t pixmap;
    xcb_dri3_syncobj_t syncobj;
    uint32_t handle;
    int idle;
};

static xcb_generic_event_t *
next_event(xcb_connection_t *conn, xcb_special_event_t *events)
{
    struct pollfd socket = { .fd = xcb_get_file_descriptor(conn),
                             .events = POLLIN };

    enum { WAIT_SLICE_MS = 50, MAX_SLICES = 60 };

    for (unsigned i = 0; i < MAX_SLICES; i++) {
        xcb_generic_event_t *event = xcb_poll_for_special_event(conn, events);

        if (event)
            return event;

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

    require(false, "Present event timeout");
    return NULL;
}

static uint64_t
check_completion(xcb_connection_t *conn, xcb_special_event_t *events,
                 uint32_t frame, uint8_t mode)
{
    xcb_present_complete_notify_event_t *event =
        (void *)next_event(conn, events);

    REQUIRE(event->event_type == XCB_PRESENT_COMPLETE_NOTIFY);
    REQUIRE(event->serial == frame);
    REQUIRE(event->mode == mode);
    uint64_t msc = event->msc;

    free(event);
    return msc;
}

static void
check_idle(xcb_connection_t *conn, xcb_special_event_t *events,
           uint32_t frame, xcb_pixmap_t pixmap)
{
    xcb_present_idle_notify_event_t *event = (void *)next_event(conn, events);

    REQUIRE(event->event_type == XCB_PRESENT_IDLE_NOTIFY);
    REQUIRE(event->serial == frame);
    REQUIRE(event->pixmap == pixmap);
    free(event);
}

static void
test_legacy_present(xcb_connection_t *client, xcb_connection_t *observer,
                    xcb_window_t win, xcb_rectangle_t rect, bool flip)
{
    struct {
        xcb_pixmap_t pixmap;
        struct producer_mapping map;
    } buffers[2];

    xcb_present_event_t complete_id = xcb_generate_id(observer);
    xcb_present_event_t idle_id = xcb_generate_id(client);
    xcb_special_event_t *complete = xcb_register_for_special_xge(
        observer, &xcb_present_id, complete_id, NULL);
    xcb_special_event_t *idle = xcb_register_for_special_xge(
        client, &xcb_present_id, idle_id, NULL);
    uint32_t options = flip ? 0 : XCB_PRESENT_OPTION_COPY;
    uint8_t mode = flip ? XCB_PRESENT_COMPLETE_MODE_FLIP :
                          XCB_PRESENT_COMPLETE_MODE_COPY;
    uint32_t expected = 0;

    REQUIRE(complete && idle);
    check(observer, xcb_present_select_input_checked(
                        observer, complete_id, win,
                        XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
    check(client, xcb_present_select_input_checked(
                      client, idle_id, win,
                      XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY));

    for (unsigned i = 0; i < 2; i++) {
        buffers[i].pixmap = xcb_generate_id(client);
        check(client, xcb_create_pixmap_checked(client, 24, buffers[i].pixmap,
                                                win, rect.width,
                                                rect.height));
        buffers[i].map = map_pixmap(client, buffers[i].pixmap);
    }

    for (uint32_t frame = 1; frame <= 3; frame++) {
        unsigned next = (frame - 1) % 2;
        unsigned old = 1 - next;
        uint32_t color = next ? 0x127a4e : 0x8a37c2;
        xcb_sync_fence_t fence = xcb_generate_id(client);

        write_pixmap(&buffers[next].map, color);
        check(client, xcb_sync_create_fence_checked(client, win, fence, 0));
        check(client, xcb_present_pixmap_checked(
                          client, win, buffers[next].pixmap, frame, 0, 0,
                          0, 0, 0, fence, 0, options, 0, 0, 0, 0, NULL));

        check_color(observer, win, expected);
        REQUIRE(!xcb_poll_for_special_event(observer, complete));
        check(client, xcb_sync_trigger_fence_checked(client, fence));
        check_completion(observer, complete, frame, mode);

        if (!flip) {
            check_idle(client, idle, frame, buffers[next].pixmap);
            write_pixmap(&buffers[next].map, 0);
        } else if (frame > 1) {
            check_idle(client, idle, frame - 1, buffers[old].pixmap);
            write_pixmap(&buffers[old].map, 0);
        }

        check_color(observer, win, color);
        expected = color;
        check(client, xcb_sync_destroy_fence_checked(client, fence));
    }

    check(client, xcb_destroy_window_checked(client, win));
    xcb_unregister_for_special_event(observer, complete);
    xcb_unregister_for_special_event(client, idle);

    for (unsigned i = 0; i < 2; i++)
        unmap_pixmap(&buffers[i].map);
}

static void
test_access_failure(xcb_connection_t *client, xcb_connection_t *observer,
                    xcb_window_t win, xcb_pixmap_t pixmap, int fd)
{
    xcb_present_event_t id = xcb_generate_id(observer);
    xcb_special_event_t *complete = xcb_register_for_special_xge(
        observer, &xcb_present_id, id, NULL);
    struct producer_mapping map = map_pixmap(client, pixmap);
    xcb_dri3_syncobj_t timeline = xcb_generate_id(client);
    uint32_t handle = 0;
    int timeline_fd = -1;
    uint64_t acquire = UINT64_C(1) << 40;
    uint64_t release = acquire + 1;
    int idle = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct pollfd done = { .fd = idle, .events = POLLIN };

    REQUIRE(complete && idle >= 0);
    write_pixmap(&map, 0xc41758);
    check(observer, xcb_present_select_input_checked(
                        observer, id, win,
                        XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
    REQUIRE(!drmSyncobjCreate(fd, 0, &handle));
    REQUIRE(!drmSyncobjHandleToFD(fd, handle, &timeline_fd));
    check(client, xcb_dri3_import_syncobj_checked(client, timeline, win,
                                                  timeline_fd));
    REQUIRE(!drmSyncobjEventfd(fd, handle, release, idle, 0));
    check(client, xcb_present_pixmap_synced_checked(
                      client, win, pixmap, 1, 0, 0, 0, 0, 0, timeline,
                      timeline, acquire, release, XCB_PRESENT_OPTION_COPY,
                      0, 0, 0, 0, NULL));
    REQUIRE(!drmSyncobjTimelineSignal(fd, &handle, &acquire, 1));
    check_completion(observer, complete, 1, XCB_PRESENT_COMPLETE_MODE_SKIP);
    REQUIRE(poll(&done, 1, 3000) == 1);
    check_color(observer, win, 0);
    check_color(client, pixmap, 0);

    xcb_generic_error_t *error = NULL;
    xcb_dri3_buffer_from_pixmap_reply_t *reply =
        xcb_dri3_buffer_from_pixmap_reply(
            client, xcb_dri3_buffer_from_pixmap(client, pixmap), &error);

    REQUIRE(!reply && error && error->error_code == XCB_PIXMAP);
    free(error);
    xcb_unregister_for_special_event(observer, complete);
    REQUIRE(!drmSyncobjDestroy(fd, handle));
    unmap_pixmap(&map);
    close(idle);
}

static xcb_randr_get_screen_resources_current_reply_t *
get_outputs(xcb_connection_t *conn, xcb_window_t root)
{
    xcb_randr_get_screen_resources_current_cookie_t cookie =
        xcb_randr_get_screen_resources_current(conn, root);
    xcb_randr_get_screen_resources_current_reply_t *resources =
        xcb_randr_get_screen_resources_current_reply(conn, cookie, NULL);

    REQUIRE(resources);
    return resources;
}

static void
test_output_loss(xcb_connection_t *client, xcb_connection_t *observer,
                 xcb_window_t root, xcb_window_t win, xcb_pixmap_t pixmap,
                 int fd)
{
    enum { OUTPUT_LOSS_TIMEOUT_MS = 60000 };

    xcb_randr_get_screen_resources_current_reply_t *resources =
        get_outputs(observer, root);
    xcb_randr_output_t *outputs =
        xcb_randr_get_screen_resources_current_outputs(resources);
    xcb_randr_output_t output = XCB_NONE;
    xcb_randr_get_output_info_reply_t *initial = NULL;

    for (unsigned i = 0; i < resources->num_outputs; i++) {
        xcb_randr_get_output_info_cookie_t cookie = xcb_randr_get_output_info(
            observer, outputs[i], resources->config_timestamp);

        initial = xcb_randr_get_output_info_reply(observer, cookie, NULL);
        if (initial && initial->crtc &&
            initial->connection == XCB_RANDR_CONNECTION_CONNECTED) {
            output = outputs[i];
            break;
        }
        free(initial);
        initial = NULL;
    }

    free(resources);
    require(initial != NULL, "connected output with active scanout");

    xcb_present_event_t id = xcb_generate_id(observer);
    xcb_special_event_t *complete = xcb_register_for_special_xge(
        observer, &xcb_present_id, id, NULL);
    struct producer_mapping map = map_pixmap(client, pixmap);
    xcb_dri3_syncobj_t timeline = xcb_generate_id(client);
    uint32_t handle = 0;
    int timeline_fd = -1;
    uint64_t acquire = UINT64_C(1) << 40;
    uint64_t release = acquire + 1;
    int idle = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct pollfd done = { .fd = idle, .events = POLLIN };
    const uint32_t color = 0x2a7ea1;

    REQUIRE(complete && idle >= 0);
    check(observer, xcb_present_select_input_checked(
                        observer, id, win,
                        XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
    REQUIRE(!drmSyncobjCreate(fd, 0, &handle));
    REQUIRE(!drmSyncobjHandleToFD(fd, handle, &timeline_fd));
    check(client, xcb_dri3_import_syncobj_checked(client, timeline, win,
                                                  timeline_fd));
    REQUIRE(!drmSyncobjEventfd(fd, handle, release, idle, 0));
    write_pixmap(&map, color);
    check(client, xcb_present_pixmap_synced_checked(
                      client, win, pixmap, 1, 0, 0, 0, 0, initial->crtc,
                      timeline, timeline, acquire, release, 0, 0, 0, 0,
                      0, NULL));
    REQUIRE(!drmSyncobjTimelineSignal(fd, &handle, &acquire, 1));
    check_completion(observer, complete, 1, XCB_PRESENT_COMPLETE_MODE_FLIP);
    check_color(observer, win, color);
    REQUIRE(poll(&done, 1, 0) == 0);

    printf("Disconnect output %.*s now\n",
           xcb_randr_get_output_info_name_length(initial),
           xcb_randr_get_output_info_name(initial));
    fflush(stdout);
    free(initial);
    REQUIRE(poll(&done, 1, OUTPUT_LOSS_TIMEOUT_MS) == 1);

    resources = get_outputs(observer, root);
    xcb_randr_get_output_info_cookie_t cookie = xcb_randr_get_output_info(
        observer, output, resources->config_timestamp);
    xcb_randr_get_output_info_reply_t *after =
        xcb_randr_get_output_info_reply(observer, cookie, NULL);

    REQUIRE(!xcb_connection_has_error(observer));
    REQUIRE(!after || after->connection == XCB_RANDR_CONNECTION_DISCONNECTED);
    free(after);
    free(resources);
    xcb_unregister_for_special_event(observer, complete);
    REQUIRE(!drmSyncobjDestroy(fd, handle));
    unmap_pixmap(&map);
    close(idle);
}

static void
test_flip(xcb_connection_t *client, xcb_connection_t *observer,
          xcb_window_t win, xcb_rectangle_t rect, int fd)
{
    struct flip_buffer buffers[2];
    xcb_present_event_t eid = xcb_generate_id(client);
    xcb_special_event_t *events =
        xcb_register_for_special_xge(client, &xcb_present_id, eid, NULL);
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
        buffer->map = map_pixmap(client, buffer->pixmap);
        REQUIRE(!drmSyncobjCreate(fd, 0, &buffer->handle));
        REQUIRE(!drmSyncobjHandleToFD(fd, buffer->handle, &shared));
        check(client, xcb_dri3_import_syncobj_checked(client, buffer->syncobj,
                                                      win, shared));
    }

    /* Distinct timelines keep a new acquire from advancing an old release. */
    for (unsigned frame = 0; frame < 4; frame++) {
        struct flip_buffer *next = &buffers[frame % 2];
        struct flip_buffer *old = &buffers[(frame + 1) % 2];
        struct pollfd previous = {.fd = old->idle, .events = POLLIN};
        struct pollfd active = {.fd = next->idle, .events = POLLIN};
        uint64_t acquire = (UINT64_C(1) << 40) + 2 * frame + 1;
        uint64_t release = acquire + 1;
        uint32_t color = frame % 2 ? 0x1ac332 : 0xd2184b;

        write_pixmap(&next->map, color);
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
            write_pixmap(&old->map, 0);
        }

        check_color(observer, win, color);
        expected = color;
    }

    check(client, xcb_destroy_window_checked(client, win));
    struct pollfd retired = {.fd = buffers[1].idle, .events = POLLIN};

    REQUIRE(poll(&retired, 1, 3000) == 1);
    xcb_unregister_for_special_event(client, events);
    for (unsigned i = 0; i < 2; i++) {
        unmap_pixmap(&buffers[i].map);
        close(buffers[i].idle);
    }
}

static void
test_skipped(xcb_connection_t *client, xcb_connection_t *observer,
             xcb_window_t win, xcb_pixmap_t pixmap, int fd, uint32_t handle,
             xcb_dri3_syncobj_t syncobj, uint32_t previous_color)
{
    xcb_present_event_t eid = xcb_generate_id(client);
    xcb_special_event_t *events =
        xcb_register_for_special_xge(client, &xcb_present_id, eid, NULL);
    int first = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int second = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct pollfd skipped = { .fd = first, .events = POLLIN };
    struct pollfd copied = { .fd = second, .events = POLLIN };
    uint64_t acquire = (UINT64_C(1) << 40) + 20;

    enum {
        QUERY_SERIAL = 10,
        SKIP_SERIAL = 11,
        COPY_SERIAL = 12,
        QUEUE_AHEAD = 8,
    };

    REQUIRE(events && first >= 0 && second >= 0);
    check(client, xcb_present_select_input_checked(
                      client, eid, win, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
    check(client, xcb_present_notify_msc_checked(
                      client, win, QUERY_SERIAL, 0, 0, 0));

    uint64_t msc = check_completion(client, events, QUERY_SERIAL,
                                    XCB_PRESENT_COMPLETE_MODE_COPY);
    uint64_t target = msc + QUEUE_AHEAD;
    uint64_t replacement = acquire + 2;

    REQUIRE(!drmSyncobjEventfd(fd, handle, acquire + 1, first, 0));
    REQUIRE(!drmSyncobjEventfd(fd, handle, replacement + 1, second, 0));
    xcb_void_cookie_t old = xcb_present_pixmap_synced_checked(
        client, win, pixmap, SKIP_SERIAL, 0, 0, 0, 0, 0, syncobj, syncobj,
        acquire, acquire + 1, XCB_PRESENT_OPTION_COPY, target, 0, 0, 0, NULL);
    xcb_void_cookie_t new = xcb_present_pixmap_synced_checked(
        client, win, pixmap, COPY_SERIAL, 0, 0, 0, 0, 0, syncobj, syncobj,
        replacement, replacement + 1, XCB_PRESENT_OPTION_COPY, target,
        0, 0, 0, NULL);

    check(client, old);
    check(client, new);
    REQUIRE(poll(&skipped, 1, 3000) == 1);
    check_completion(client, events, SKIP_SERIAL, XCB_PRESENT_COMPLETE_MODE_SKIP);
    REQUIRE(poll(&copied, 1, 0) == 0);
    check_color(observer, win, previous_color);
    REQUIRE(!drmSyncobjTimelineSignal(fd, &handle, &replacement, 1));
    REQUIRE(poll(&copied, 1, 3000) == 1);
    check_completion(client, events, COPY_SERIAL, XCB_PRESENT_COMPLETE_MODE_COPY);
    check_color(observer, win, 0);

    close(first);
    close(second);
    xcb_unregister_for_special_event(client, events);
}

static void
test_disconnect(xcb_connection_t *client, xcb_connection_t *observer,
                xcb_rectangle_t rect, int fd, bool active_flip)
{
    xcb_screen_t *screen =
        xcb_setup_roots_iterator(xcb_get_setup(observer)).data;
    xcb_window_t win = xcb_generate_id(observer);
    xcb_pixmap_t pixmap = xcb_generate_id(client);
    xcb_gcontext_t gc = xcb_generate_id(client);
    xcb_dri3_syncobj_t id = xcb_generate_id(client);
    xcb_present_event_t eid = xcb_generate_id(observer);
    xcb_special_event_t *events =
        xcb_register_for_special_xge(observer, &xcb_present_id, eid, NULL);
    uint32_t bg_override[] = {0, 1};
    const uint32_t color = 0xd46132;
    uint32_t handle = 0;
    int shared = -1;
    uint64_t acquire = (UINT64_C(1) << 41) + 1;
    uint64_t release = acquire + 1;
    int idle = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct pollfd done = { .fd = idle, .events = POLLIN };

    REQUIRE(idle >= 0 && events);
    check(observer, xcb_create_window_checked(
                        observer, 24, win, screen->root, 0, 0, rect.width,
                        rect.height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        screen->root_visual,
                        XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT,
                        bg_override));
    check(observer, xcb_map_window_checked(observer, win));
    check(observer,
          xcb_present_select_input_checked(
              observer, eid, win, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
    check(client, xcb_create_pixmap_checked(client, 24, pixmap, win,
                                            rect.width, rect.height));
    check(client, xcb_create_gc_checked(client, gc, pixmap,
                                        XCB_GC_FOREGROUND, &color));
    check(client, xcb_poly_fill_rectangle_checked(client, pixmap, gc,
                                                  1, &rect));
    REQUIRE(!drmSyncobjCreate(fd, 0, &handle));
    REQUIRE(!drmSyncobjHandleToFD(fd, handle, &shared));
    check(client, xcb_dri3_import_syncobj_checked(client, id, win, shared));
    REQUIRE(!drmSyncobjEventfd(fd, handle, release, idle, 0));

    if (!active_flip)
        check(client, xcb_set_close_down_mode_checked(
                          client, XCB_CLOSE_DOWN_RETAIN_TEMPORARY));

    uint32_t options = active_flip ? 0 : XCB_PRESENT_OPTION_COPY;

    check(client, xcb_present_pixmap_synced_checked(
                      client, win, pixmap, 1, 0, 0, 0, 0, 0, id, id,
                      acquire, release, options, 0, 0, 0, 0, NULL));
    if (active_flip) {
        REQUIRE(!drmSyncobjTimelineSignal(fd, &handle, &acquire, 1));
        check_completion(observer, events, 1, XCB_PRESENT_COMPLETE_MODE_FLIP);
    }

    uint32_t expected = active_flip ? color : bg_override[0];

    check_color(observer, win, expected);
    REQUIRE(poll(&done, 1, 40) == 0);
    xcb_disconnect(client);
    REQUIRE(poll(&done, 1, 3000) == 1);
    check_color(observer, win, expected);
    xcb_unregister_for_special_event(observer, events);
    check(observer, xcb_destroy_window_checked(observer, win));
    REQUIRE(!drmSyncobjDestroy(fd, handle));
    close(idle);
}

int
main(int argc, char **argv)
{
    bool legacy_flip = argc == 2 && !strcmp(argv[1], "--unavailable-flip");
    bool unavailable = legacy_flip || (argc == 2 &&
                                       !strcmp(argv[1], "--unavailable"));
    bool flip = argc == 2 && !strcmp(argv[1], "--flip");
    bool fallback = argc == 2 && !strcmp(argv[1], "--flip-fallback");
    bool access_failure = argc == 2 && !strcmp(argv[1], "--access-failure");
    bool output_loss = argc == 2 && !strcmp(argv[1], "--output-loss");

    bool fullscreen = flip || fallback || legacy_flip || output_loss;
    bool recognized = unavailable || fullscreen || access_failure;

    if (argc > 1 && !recognized) {
        fprintf(stderr, "usage: %s [--unavailable|--unavailable-flip|"
                        "--flip|--flip-fallback|--access-failure|"
                        "--output-loss]\n",
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
        test_legacy_present(client, observer, win, rect, legacy_flip);
        xcb_disconnect(client);
        xcb_disconnect(observer);
        printf("PASS legacy Present %s with explicit sync unavailable\n",
               legacy_flip ? "flip" : "copy");
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
        test_disconnect(client, observer, rect, fd, true);
        xcb_disconnect(observer);
        close(fd);
        puts("PASS delayed flips, replacement retirement and unflip");
        puts("PASS producer disconnect with an active flip");
        return EXIT_SUCCESS;
    }

    check(client, xcb_create_pixmap_checked(client, 24, pixmap, win,
                                            rect.width, rect.height));

    if (output_loss) {
        test_output_loss(client, observer, screen->root, win, pixmap, fd);
        xcb_disconnect(client);
        xcb_disconnect(observer);
        close(fd);
        puts("PASS active Present flip retired after output loss");
        return EXIT_SUCCESS;
    }

    if (access_failure) {
        test_access_failure(client, observer, win, pixmap, fd);
        xcb_disconnect(client);
        xcb_disconnect(observer);
        close(fd);
        puts("PASS dma-buf access failure skipped and released the Present");
        return EXIT_SUCCESS;
    }

    struct producer_mapping map = map_pixmap(client, pixmap);

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

        write_pixmap(&map, color);
        REQUIRE(!drmSyncobjEventfd(fd, handle, release, notify, 0));
        check(client, xcb_present_pixmap_synced_checked(
                          client, win, pixmap, frame, 0, 0, 0, 0, 0, id, id,
                          acquire, release, options, 0, 0, 0, 0, NULL));

        REQUIRE(poll(&done, 1, 40) == 0);
        check_color(observer, win, expected);
        REQUIRE(!drmSyncobjTimelineSignal(fd, &handle, &acquire, 1));
        REQUIRE(poll(&done, 1, 3000) == 1);
        close(notify);
        write_pixmap(&map, black);
        check_color(observer, win, color);
        expected = color;
    }

    test_skipped(client, observer, win, pixmap, fd, handle, id, expected);

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
    unmap_pixmap(&map);
    test_disconnect(client, observer, rect, fd, false);
    xcb_disconnect(observer);
    close(fd);
    puts("PASS delayed acquire, independent client, reuse and release");
    puts("PASS superseded Present and producer disconnect");
    return EXIT_SUCCESS;
}
