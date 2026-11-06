/* SPDX-License-Identifier: MIT */
#include "dix-config.h"
#include <assert.h>
#include "../hw/xfree86/drivers/fission/vblank.c"

enum {
    TEST_MODE_WIDTH = 1920,
    TEST_MODE_HEIGHT = 1080,
    TEST_WINDOW_SIZE = 100,
};

DevPrivateKeyRec rrPrivKeyRec = { .initialized = TRUE };
static RROutputRec primary_output;

RROutputPtr
RRFirstOutput(ScreenPtr screen)
{
    return screen->isGPU ? NULL : &primary_output;
}

int
main(void)
{
    ScreenRec screen = { 0 }, secondary = { .isGPU = TRUE,
                                             .is_output_secondary = TRUE };
    RRModeRec mode = { .mode = { .width = TEST_MODE_WIDTH,
                                 .height = TEST_MODE_HEIGHT } };
    drmmode_crtc_private_rec dc = { .dpms_mode = DPMSModeOn };
    xf86CrtcRec xf86_crtc = { .enabled = TRUE, .driver_private = &dc };
    RRCrtcRec primary = { .mode = &mode, .devPrivate = &xf86_crtc };
    RRCrtcRec prime = { .mode = &mode, .x = TEST_MODE_WIDTH };
    RRCrtcPtr primary_crtcs[] = { &primary }, prime_crtcs[] = { &prime };
    rrScrPrivRec screen_priv = { .numCrtcs = ARRAY_SIZE(primary_crtcs),
                                .crtcs = primary_crtcs };
    rrScrPrivRec secondary_priv = { .numCrtcs = ARRAY_SIZE(prime_crtcs),
                                   .crtcs = prime_crtcs };
    rrScrPrivPtr screen_storage = &screen_priv, secondary_storage = &secondary_priv;
    DrawableRec drawable = { .pScreen = &screen, .width = TEST_WINDOW_SIZE,
                             .height = TEST_WINDOW_SIZE };

    screen.devPrivates = (PrivatePtr) &screen_storage;
    secondary.devPrivates = (PrivatePtr) &secondary_storage;
    primary_output.crtc = &primary;
    xorg_list_init(&screen.secondary_list);
    xorg_list_add(&secondary.secondary_head, &screen.secondary_list);

    assert(ms_randr_crtc_covering_drawable(&drawable) == &primary);

    /* A primary output with no overlap must allow PRIME output selection. */
    drawable.x = TEST_MODE_WIDTH;
    assert(ms_randr_crtc_covering_drawable(&drawable) == &prime);
    assert(!ms_dri2_crtc_covering_drawable(&drawable));

    drawable.x = TEST_MODE_WIDTH * 2;
    assert(!ms_randr_crtc_covering_drawable(&drawable));

    /* Equal positive coverage still prefers the primary output. */
    primary.x = TEST_MODE_WIDTH;
    drawable.x = TEST_MODE_WIDTH;
    assert(ms_randr_crtc_covering_drawable(&drawable) == &primary);

    dc.dpms_mode = DPMSModeOff;
    assert(ms_randr_crtc_covering_drawable(&drawable) == &prime);
    return 0;
}
