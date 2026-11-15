/* SPDX-License-Identifier: MIT */

#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#define SCREEN_COUNT 2

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    xcb_screen_iterator_t screens;
    unsigned int count = 0;

    assert(connection && !xcb_connection_has_error(connection));
    screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (; screens.rem; xcb_screen_next(&screens)) {
        xcb_screen_t *screen = screens.data;
        xcb_randr_get_screen_resources_current_reply_t *resources;
        xcb_randr_get_crtc_info_reply_t *crtc;
        xcb_randr_crtc_t *crtcs;

        resources = xcb_randr_get_screen_resources_current_reply(
            connection,
            xcb_randr_get_screen_resources_current(connection, screen->root),
            NULL);
        assert(resources && resources->num_crtcs == 1);
        crtcs = xcb_randr_get_screen_resources_current_crtcs(resources);
        crtc = xcb_randr_get_crtc_info_reply(
            connection,
            xcb_randr_get_crtc_info(connection, crtcs[0], resources->config_timestamp),
            NULL);
        assert(crtc && crtc->mode);
        assert(crtc->width == screen->width_in_pixels);
        assert(crtc->height == screen->height_in_pixels);
        free(crtc);
        free(resources);
        count++;
    }
    assert(count == SCREEN_COUNT);
    xcb_disconnect(connection);
    return 0;
}
