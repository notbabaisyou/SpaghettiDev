/* SPDX-License-Identifier: MIT */
#include "dix-config.h"
#include <assert.h>
#include "../hw/xfree86/drivers/fission/present.c"

enum {
    TEST_FRAMEBUFFER = 1,
    TEST_EVENT,
    TEST_MSC,
};

int xf86CrtcConfigPrivateIndex;
static ScrnInfoRec test_scrn;
static ScreenPtr test_screen;
static unsigned screen_flips, window_flips;

ScrnInfoPtr
xf86ScreenToScrn(ScreenPtr screen)
{
    assert(screen == test_screen);
    return &test_scrn;
}

Bool
xf86_crtc_on(xf86CrtcPtr crtc)
{
    return crtc->enabled;
}

Bool
drmmode_is_format_supported(ScrnInfoPtr scrn, uint32_t format,
                             uint64_t modifier, Bool async_flip)
{
    return TRUE;
}

void
ms_window_update_async_flip(WindowPtr window, Bool async_flip)
{
    assert(window);
}

Bool
ms_window_has_async_flip_modifiers(WindowPtr window)
{
    assert(window);
    return FALSE;
}

Bool
ms_window_has_variable_refresh(modesettingPtr ms, WindowPtr window)
{
    assert(window);
    return FALSE;
}

void
drmmode_crtc_set_vrr(xf86CrtcPtr crtc, Bool enabled)
{
}

void
present_event_notify(uint64_t event_id, uint64_t ust, uint64_t msc)
{
}

Bool
ms_compositor_queue_present(xf86CrtcPtr crtc, WindowPtr window, uint64_t event_id)
{
    assert(window);
    window_flips++;
    return TRUE;
}

Bool
ms_do_pageflip(ScreenPtr screen, PixmapPtr pixmap, void *event,
                int ref_pipe, Bool async, ms_pageflip_handler_proc handler,
                ms_pageflip_abort_proc abort, const char *log_prefix)
{
    screen_flips++;
    free(event);
    return FALSE;
}

#ifdef HAVE_GBM
static struct gbm_bo *
test_bo_from_pixmap(ScreenPtr screen, PixmapPtr pixmap)
{
    assert(pixmap);
    return NULL;
}
#endif

int
main(void)
{
    modesettingRec ms = { 0 };
    ScreenRec screen = { 0 };
    WindowRec window = { 0 };
    PixmapRec pixmap = { 0 };
    xf86CrtcRec crtc = { 0 };
    xf86CrtcPtr crtcs[] = { &crtc };
    xf86CrtcConfigRec config = { .num_crtc = ARRAY_SIZE(crtcs), .crtc = crtcs };
    DevUnion privates[] = { { .ptr = &config } };
    drmmode_crtc_private_rec dc = { 0 };
    RRCrtcRec rr_crtc = { 0 };

    test_screen = &screen;
    test_scrn.driverPrivate = &ms;
    test_scrn.privates = privates;
    test_scrn.vtSema = TRUE;
    crtc.scrn = &test_scrn;
    crtc.driver_private = &dc;
    crtc.enabled = TRUE;
    dc.compositor.fb_id[0] = TEST_FRAMEBUFFER;
    rr_crtc.devPrivate = &crtc;
    rr_crtc.pScreen = &screen;
    window.drawable.pScreen = &screen;
    ms.drmmode.compositing = TRUE;
    ms.drmmode.pageflip = TRUE;
#ifdef HAVE_GBM
    ms.drmmode.glamor = TRUE;
    ms.glamor.gbm_bo_from_pixmap = test_bo_from_pixmap;
#endif

    /* Compositor flip checks leave the legacy screen-flip context unset. */
    assert(ms_present_check_flip(&rr_crtc, &window, &pixmap, TRUE, NULL));
    assert(!ms.flip_window);
    assert(!ms_present_flip(&rr_crtc, TEST_EVENT, TEST_MSC, &pixmap, TRUE));
    assert(!screen_flips);
    assert(ms_present_window_flip(&rr_crtc, TEST_EVENT, &window, &pixmap, TRUE));
    assert(window_flips == 1);

    /* Invalid requests are rejected before dereferencing their window. */
    assert(!ms_present_check_flip(&rr_crtc, NULL, &pixmap, TRUE, NULL));
    assert(!ms_present_check_flip(&rr_crtc, &window, NULL, TRUE, NULL));

    /* A cached legacy window cannot enable a screen flip while compositing. */
    ms.flip_window = &window;
    assert(!ms_present_flip(&rr_crtc, TEST_EVENT, TEST_MSC, &pixmap, TRUE));
    assert(!screen_flips);

    /* Shared-framebuffer presentation retains its legacy callback. */
    ms.drmmode.compositing = FALSE;
    ms.flip_window = NULL;
    assert(!ms_present_flip(&rr_crtc, TEST_EVENT, TEST_MSC, &pixmap, TRUE));
    assert(ms_present_check_flip(&rr_crtc, &window, &pixmap, TRUE, NULL));
    assert(ms.flip_window == &window);
    assert(!ms_present_flip(&rr_crtc, TEST_EVENT, TEST_MSC, &pixmap, TRUE));
    assert(screen_flips == 1);
    return 0;
}
