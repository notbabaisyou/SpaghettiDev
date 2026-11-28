/*
 * Copyright © 2014 Intel Corporation
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

#include "dix-config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#include "os/xserver_poll.h"

#include <xf86.h>
#include <xf86Crtc.h>
#include <xf86drm.h>
#include <xf86str.h>
#include <present.h>

#include "driver.h"
#include "drmmode_display.h"
#include "vrr.h"

#if defined(HAVE_GBM) || defined(FISSION_FB_BACKEND)
#include "drm_fourcc.h"
#endif

#if 0
#define DebugPresent(x) ErrorF x
#else
#define DebugPresent(x)
#endif

struct ms_present_vblank_event {
    uint64_t        event_id;
    Bool            unflip;
};

static RRCrtcPtr
ms_present_get_crtc(WindowPtr window)
{
    return ms_randr_crtc_covering_drawable(&window->drawable);
}

static int
ms_present_get_ust_msc(RRCrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    xf86CrtcPtr xf86_crtc = crtc->devPrivate;

    return ms_get_crtc_ust_msc(xf86_crtc, ust, msc);
}

/*
 * Changes the variable refresh state for every CRTC on the screen.
 */
void
ms_present_set_screen_vrr(ScrnInfoPtr scrn, Bool vrr_enabled)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    xf86CrtcPtr crtc;
    int i;

    for (i = 0; i < config->num_crtc; i++) {
        crtc = config->crtc[i];
        drmmode_crtc_set_vrr(crtc, vrr_enabled);
    }
}

/*
 * Called when the queued vblank event has occurred
 */
static void
ms_present_vblank_handler(uint64_t msc, uint64_t usec, void *data)
{
    struct ms_present_vblank_event *event = data;

    DebugPresent(("\t\tmh %lld msc %llu\n",
                 (long long) event->event_id, (long long) msc));

    present_event_notify(event->event_id, usec, msc);
    free(event);
}

/*
 * Called when the queued vblank is aborted
 */
static void
ms_present_vblank_abort(void *data)
{
    struct ms_present_vblank_event *event = data;

    DebugPresent(("\t\tma %lld\n", (long long) event->event_id));

    free(event);
}

/*
 * Queue an event to report back to the Present extension when the specified
 * MSC has past
 */
static int
ms_present_queue_vblank(RRCrtcPtr crtc,
                        uint64_t event_id,
                        uint64_t msc)
{
    xf86CrtcPtr xf86_crtc = crtc->devPrivate;
    struct ms_present_vblank_event *event;
    uint32_t seq;

    event = calloc(1, sizeof(struct ms_present_vblank_event));
    if (!event)
        return BadAlloc;
    event->event_id = event_id;
    seq = ms_drm_queue_alloc(xf86_crtc, event,
                             ms_present_vblank_handler,
                             ms_present_vblank_abort);
    if (!seq) {
        free(event);
        return BadAlloc;
    }

    if (!ms_queue_vblank(xf86_crtc, MS_QUEUE_ABSOLUTE, msc, NULL, seq))
        return BadAlloc;

    DebugPresent(("\t\tmq %lld seq %u msc %llu\n",
                 (long long) event_id, seq, (long long) msc));
    return Success;
}

static Bool
ms_present_event_match(void *data, void *match_data)
{
    struct ms_present_vblank_event *event = data;
    uint64_t *match = match_data;

    return *match == event->event_id;
}

/*
 * Remove a pending vblank event from the DRM queue so that it is not reported
 * to the extension
 */
static void
ms_present_abort_vblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    ScreenPtr screen = crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);

    ms_drm_abort(scrn, ms_present_event_match, &event_id);
}

/*
 * Flush our batch buffer when requested by the Present extension.
 */
static void
ms_present_flush(WindowPtr window)
{
#if defined(HAVE_GBM) || defined(FISSION_FB_BACKEND)
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);

    if (ms->drmmode.glamor)
        ms->glamor.block_handler(screen);
#endif
}

#if defined(HAVE_GBM) || defined(FISSION_FB_BACKEND)
/**
 * Callback for the DRM event queue when a flip has completed on all pipes
 *
 * Notify the extension code
 */
static void
ms_present_flip_handler(modesettingPtr ms, uint64_t msc,
                        uint64_t ust, void *data)
{
    struct ms_present_vblank_event *event = data;

    DebugPresent(("\t\tms:fc %lld msc %llu ust %llu\n",
                  (long long) event->event_id,
                  (long long) msc, (long long) ust));

    if (event->unflip)
        ms->drmmode.present_flipping = FALSE;

    ms_present_vblank_handler(msc, ust, event);
}

/*
 * Callback for the DRM queue abort code.  A flip has been aborted.
 */
static void
ms_present_flip_abort(modesettingPtr ms, void *data)
{
    struct ms_present_vblank_event *event = data;

    DebugPresent(("\t\tms:fa %lld\n", (long long) event->event_id));

    free(event);
}

/*
 * Test to see if page flipping is possible on the target crtc
 *
 * We ignore sw-cursors when *disabling* flipping, we may very well be
 * returning to scanning out the normal framebuffer *because* we just
 * switched to sw-cursor mode and check_flip just failed because of that.
 */
static Bool
ms_present_check_unflip(RRCrtcPtr crtc,
                        WindowPtr window,
                        PixmapPtr pixmap,
                        Bool sync_flip,
                        PresentFlipReason *reason)
{
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int num_crtcs_on = 0;
    int i;
    struct gbm_bo *gbm = NULL;

    if (!ms->drmmode.pageflip)
        return FALSE;

    if (ms->drmmode.dri2_flipping)
        return FALSE;

    if (!scrn->vtSema)
        return FALSE;

    for (i = 0; i < config->num_crtc; i++) {
        drmmode_crtc_private_ptr drmmode_crtc = config->crtc[i]->driver_private;

        /* Don't do pageflipping if CRTCs are rotated. */
#if defined(HAVE_GBM) || defined(FISSION_FB_BACKEND)
        if (drmmode_crtc->rotate_bo.gbm)
            return FALSE;
#endif

        if (drmmode_crtc->rotate_bo.dumb)
            return FALSE;

        if (xf86_crtc_on(config->crtc[i]))
            num_crtcs_on++;
    }

    /* We can't do pageflipping if all the CRTCs are off. */
    if (num_crtcs_on == 0)
        return FALSE;

#ifdef GBM_BO_WITH_MODIFIERS
    /* Check if buffer format/modifier is supported by all active CRTCs */
    if (ms->drmmode.glamor)
        gbm = ms->glamor.gbm_bo_from_pixmap(screen, pixmap);
#ifdef FISSION_FB_BACKEND
    else if (ms->fb_backend.gbm_bo_from_pixmap)
        gbm = ms->fb_backend.gbm_bo_from_pixmap(screen, pixmap);
#endif

    if (gbm) {
        uint32_t format;
        uint64_t modifier;

        format = gbm_bo_get_format(gbm);
        modifier = gbm_bo_get_modifier(gbm);

        Bool owned = ms->drmmode.glamor;
#ifdef FISSION_FB_BACKEND
        owned |= ms->fb_backend.owns_scanout_reference;
#endif
        if (owned)
            gbm_bo_destroy(gbm);

        if (!drmmode_is_format_supported(scrn, format, modifier, !sync_flip)) {
            if (reason)
                *reason = PRESENT_FLIP_REASON_BUFFER_FORMAT;
            return FALSE;
        }
    }
#endif

    return TRUE;
}

static Bool
ms_tearfree_is_active_on_crtc(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return drmmode_crtc->tearfree.fb_id[0] &&
           crtc->scrn->vtSema &&
           xf86_crtc_on(crtc);
}

static Bool
ms_present_check_flip(RRCrtcPtr crtc,
                      WindowPtr window,
                      PixmapPtr pixmap,
                      Bool sync_flip,
                      PresentFlipReason *reason)
{
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    Bool async_flip = !sync_flip;

    if (ms->drmmode.sprites_visible > 0)
        goto no_flip;

    if (ms->drmmode.pending_modeset)
        goto no_flip;

    if (!ms_present_check_unflip(crtc, window, pixmap, sync_flip, reason)) {
        if (reason && *reason == PRESENT_FLIP_REASON_BUFFER_FORMAT)
            ms_window_update_async_flip(window, async_flip);
        goto no_flip;
    }

    ms_window_update_async_flip(window, async_flip);

    if (reason && async_flip != ms_window_has_async_flip_modifiers(window)) {
        *reason = PRESENT_FLIP_REASON_BUFFER_FORMAT;
    }

    ms->flip_window = window;
    return TRUE;

no_flip:
    if (reason && *reason == PRESENT_FLIP_REASON_UNKNOWN && crtc) {
        xf86CrtcPtr xf86_crtc = crtc->devPrivate;

        if (ms_tearfree_is_active_on_crtc(xf86_crtc)) {
            drmmode_crtc_private_ptr drmmode_crtc = xf86_crtc->driver_private;

            *reason = drmmode_crtc->tearfree.flip_seq ?
                      PRESENT_FLIP_REASON_DRIVER_TEARFREE_FLIPPING :
                      PRESENT_FLIP_REASON_DRIVER_TEARFREE;
        }
    }

    return FALSE;
}

/*
 * Queue a flip on 'crtc' to 'pixmap' at 'target_msc'. If 'sync_flip' is true,
 * then wait for vblank. Otherwise, flip immediately
 */
static Bool
ms_present_flip(RRCrtcPtr crtc,
                uint64_t event_id,
                uint64_t target_msc,
                PixmapPtr pixmap,
                Bool sync_flip)
{
    ScreenPtr screen = crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    xf86CrtcPtr xf86_crtc = crtc->devPrivate;
    drmmode_crtc_private_ptr drmmode_crtc = xf86_crtc->driver_private;
    struct ms_present_vblank_event *event;
    Bool ret;

    if (!ms_present_check_flip(crtc, ms->flip_window, pixmap, sync_flip, NULL))
        return FALSE;

    event = calloc(1, sizeof(struct ms_present_vblank_event));
    if (!event)
        return FALSE;

    DebugPresent(("\t\tms:pf %lld msc %llu\n",
                  (long long) event_id, (long long) target_msc));

    event->event_id = event_id;
    event->unflip = FALSE;

    ret = ms_do_pageflip(screen, pixmap, event, drmmode_crtc->vblank_pipe,
                         !sync_flip, ms->present_atomic_group,
                         ms_present_flip_handler, ms_present_flip_abort,
                         "PRESENT-flip");
    if (ret) {
        ms->drmmode.present_flipping = TRUE;
        vrr_set_flip_window(screen, ms->flip_window);
    }

    return ret;
}

#define FISSION_UNFLIP_RETRY_MS 100

static CARD32
ms_present_retry_unflip(OsTimerPtr timer, CARD32 now, void *data)
{
    ScreenPtr screen = data;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);

    if (!drmmode_restore_scanout(scrn))
        return FISSION_UNFLIP_RETRY_MS;

    present_event_notify(ms->present_unflip_event, 0, 0);
    return 0;
}

/*
 * Queue a flip back to the normal frame buffer.
 */
static void
ms_present_unflip(ScreenPtr screen, uint64_t event_id)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    PixmapPtr pixmap = screen->GetScreenPixmap(screen);

    vrr_set_flip_window(screen, NULL);

    if (!ms->drmmode.pending_modeset &&
        ms_present_check_unflip(NULL, screen->root, pixmap, TRUE, NULL)) {
        struct ms_present_vblank_event *event;

        event = calloc(1, sizeof(struct ms_present_vblank_event));
        if (event) {
            event->event_id = event_id;
            event->unflip = TRUE;

            if (ms_do_pageflip(screen, pixmap, event, -1, FALSE,
                               ms->present_atomic_group,
                               ms_present_flip_handler, ms_present_flip_abort,
                               "PRESENT-unflip"))
                return;
        }
    }

    if (drmmode_restore_scanout(scrn)) {
        present_event_notify(event_id, 0, 0);
        return;
    }

    ms->present_unflip_event = event_id;
    OsTimerPtr timer = ms->present_unflip_timer;

    ms->present_unflip_timer = TimerSet(timer, 0, FISSION_UNFLIP_RETRY_MS,
                                      ms_present_retry_unflip, screen);
}
#endif

static const present_screen_info_rec ms_present_screen_info = {
    .version = SPAGHETTI,

    .get_crtc = ms_present_get_crtc,
    .get_ust_msc = ms_present_get_ust_msc,
    .queue_vblank = ms_present_queue_vblank,
    .abort_vblank = ms_present_abort_vblank,
    .flush = ms_present_flush,

    .capabilities = PresentCapabilityNone,
#if defined(HAVE_GBM) || defined(FISSION_FB_BACKEND)
    .check_flip = NULL,
    .check_flip2 = ms_present_check_flip,

    .flip = ms_present_flip,
    .unflip = ms_present_unflip,
#endif
};

#ifndef DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15
#endif

static inline Bool
ms_present_async_supported(modesettingPtr ms)
{
    uint64_t value;
    int ret;

    ret = drmGetCap(ms->fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &value);
    return ret == 0 && value == 1;
}

Bool
ms_present_screen_init(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    Bool async_supported = ms_present_async_supported(ms);

    ms->present_info = ms_present_screen_info;
    ms->present_atomic_group = FALSE;
    ms->present_retire_failed = FALSE;
#if defined(DRI3) && defined(FISSION_FB_BACKEND)
    if (ms->fb_backend.synchronous_copy &&
        dri3_screen_supports_syncobj(screen))
        ms->present_info.capabilities |= PresentCapabilitySyncobj;
#endif

    if (async_supported && ms->drmmode.glamor) {
        ms->present_info.capabilities |= PresentCapabilityAsync;

        ms->drmmode.can_async_flip = TRUE;
    } else {
        async_supported &= drmmode_is_format_supported(scrn, DRM_FORMAT_XRGB8888,
                                                       DRM_FORMAT_MOD_LINEAR, TRUE);

        if (async_supported) {
            ms->present_info.capabilities |= PresentCapabilityAsync;
            ms->drmmode.can_async_flip = TRUE;
        }
    }

    if (!present_screen_init(screen, &ms->present_info))
        return FALSE;

#if defined(DRI3) && (defined(HAVE_GBM) || defined(FISSION_FB_BACKEND))
    uint64_t crtc_events;

    if ((ms->present_info.capabilities & PresentCapabilitySyncobj) &&
        drmGetCap(ms->fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &crtc_events) == 0 &&
        crtc_events) {
        ms->present_unflip_timer = TimerSet(NULL, 0, 0, NULL, NULL);
        if (ms->present_unflip_timer) {
            ms->present_atomic_group = TRUE;
            present_enable_syncobj_flips(screen);
        }
    }
#endif
    return TRUE;
}
