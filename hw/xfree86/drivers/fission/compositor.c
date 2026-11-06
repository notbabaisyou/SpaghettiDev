/*
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * Copyright 2019 NVIDIA CORPORATION
 * Copyright © 2026 dec05eba
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "dix-config.h"
#include "driver.h"
#include <present.h>

/* Present completions belong to the CRTC whose commit displays the frame. */
struct ms_compositor_completion {
    uint64_t id, frame, usec;
    unsigned remaining;
};

struct ms_compositor_event {
    struct ms_compositor_completion *completion;
    Bool primary;
    struct ms_compositor_event *next;
};

void
ms_compositor_damage_crtc(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr dc = crtc->driver_private;
    int i;

    for (i = 0; i < ARRAY_SIZE(dc->compositor.stale); i++)
        RegionReset(&dc->compositor.stale[i], &crtc->bounds);
}

Bool
ms_compositor_queue_present(xf86CrtcPtr target, WindowPtr window,
                             uint64_t event_id)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(target->scrn);
    struct ms_compositor_completion *completion = calloc(1, sizeof(*completion));
    struct ms_compositor_event *events = NULL;
    int c;

    if (!completion)
        return FALSE;
    completion->id = event_id;
    /* Allocate everything before publishing any event to a CRTC. */
    for (c = 0; c < config->num_crtc; c++) {
        xf86CrtcPtr crtc = config->crtc[c];
        drmmode_crtc_private_ptr dc = crtc->driver_private;
        struct ms_compositor_event *event;
        if (!dc->compositor.fb_id[0] || !xf86_crtc_on(crtc))
            continue;
        if (window && crtc != target &&
            RegionContainsRect(&window->borderSize, &crtc->bounds) == rgnOUT)
            continue;
        event = calloc(1, sizeof(*event));
        if (!event) {
            while (events) {
                event = events->next;
                free(events);
                events = event;
            }
            free(completion);
            return FALSE;
        }
        event->completion = completion;
        event->primary = crtc == target;
        event->next = events;
        events = event;
        completion->remaining++;
    }
    if (!events) {
        free(completion);
        return FALSE;
    }
    for (c = config->num_crtc - 1; c >= 0; c--) {
        xf86CrtcPtr crtc = config->crtc[c];
        drmmode_crtc_private_ptr dc = crtc->driver_private;
        struct ms_compositor_event *event;
        if (!dc->compositor.fb_id[0] || !xf86_crtc_on(crtc))
            continue;
        if (window && crtc != target &&
            RegionContainsRect(&window->borderSize, &crtc->bounds) == rgnOUT)
            continue;
        event = events;
        events = event->next;
        if (dc->compositor.events_tail)
            dc->compositor.events_tail->next = event;
        else
            dc->compositor.events = event;
        dc->compositor.events_tail = event;
        event->next = NULL;
        ms_compositor_damage_crtc(crtc);
    }
    return TRUE;
}

void
ms_compositor_flip_submitted(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr dc = crtc->driver_private;
    dc->compositor.submitted_events = dc->compositor.events;
    dc->compositor.events = NULL;
    dc->compositor.events_tail = NULL;
}

static void
ms_compositor_notify(struct ms_compositor_event *events,
                      uint64_t frame, uint64_t usec)
{
    /* Detach the list before callbacks, which can queue further presents. */
    while (events) {
        struct ms_compositor_event *next = events->next;
        struct ms_compositor_completion *completion = events->completion;
        if (events->primary) {
            completion->frame = frame;
            completion->usec = usec;
        }
        free(events);
        if (!--completion->remaining) {
            uint64_t id = completion->id;
            uint64_t ust = completion->usec, msc = completion->frame;
            free(completion);
            present_event_notify(id, ust, msc);
        }
        events = next;
    }
}

Bool
ms_compositor_flip_complete(xf86CrtcPtr crtc, uint64_t frame,
                             uint64_t usec, Bool abort)
{
    drmmode_crtc_private_ptr dc = crtc->driver_private;
    drmmode_compositor_ptr comp = &dc->compositor;
    struct ms_compositor_event *events = comp->submitted_events;
    Bool direct = comp->direct_pending;

    comp->submitted_events = NULL;
    if (abort) {
        if (comp->pending_fb && comp->pending_fb != comp->scanout_fb)
            drmModeRmFB(dc->drmmode->fd, comp->pending_fb);
        if (comp->pending_pixmap)
            dixDestroyPixmap(comp->pending_pixmap, 0);
    } else {
        if (comp->scanout_fb && comp->scanout_fb != comp->pending_fb)
            drmModeRmFB(dc->drmmode->fd, comp->scanout_fb);
        if (comp->scanout_pixmap)
            dixDestroyPixmap(comp->scanout_pixmap, 0);
        comp->scanout_fb = comp->pending_fb;
        comp->scanout_pixmap = comp->pending_pixmap;
    }
    comp->pending_fb = 0;
    comp->pending_pixmap = NULL;
    comp->direct_pending = FALSE;
    ms_compositor_notify(events, frame, usec);
    return !direct;
}

void
ms_compositor_free_crtc(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr dc = crtc->driver_private;
    drmmode_compositor_ptr comp = &dc->compositor;
    struct ms_compositor_event *events = comp->events;

    comp->events = NULL;
    comp->events_tail = NULL;
    ms_compositor_flip_complete(crtc, 0, 0, TRUE);
    ms_compositor_notify(events, 0, 0);
    if (comp->scanout_fb)
        drmModeRmFB(dc->drmmode->fd, comp->scanout_fb);
    if (comp->scanout_pixmap)
        dixDestroyPixmap(comp->scanout_pixmap, 0);
    comp->scanout_fb = 0;
    comp->scanout_pixmap = NULL;
}

void
ms_compositor_prepare_crtc(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr dc = crtc->driver_private;
    drmmode_compositor_ptr comp = &dc->compositor;
    int back = comp->back_idx;

    if (!crtc->enabled || comp->flip_seq || !comp->pixmap[back] ||
        RegionNil(&comp->stale[back]))
        return;

    if (ms_compositor_direct_flip(crtc))
        return;

    if (comp->scanout_fb)
        ms_compositor_damage_crtc(crtc);
    comp->prepared = ms_compositor_paint(crtc, comp->pixmap[back],
                                          &comp->stale[back]);
}

/*
 * Submit the flip after the flush, against the fenced back buffer.
 */
void
ms_compositor_flip_crtc(ScrnInfoPtr scrn, xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_compositor_ptr comp = &drmmode_crtc->compositor;
    int back = comp->back_idx;
    uint32_t seq;

    if (!crtc->enabled)
        return;

    if (comp->flip_seq || !comp->fb_id[back] || !comp->prepared)
        return;

    if (RegionNil(&comp->stale[back]))
        return;

    comp->prepared = FALSE;

    seq = ms_drm_queue_alloc(crtc, crtc,
                             ms_compositor_flip_handler,
                             ms_compositor_flip_abort);
    if (!seq)
        return;

    if (drmmode_crtc_flip(crtc,
                          comp->fb_id[back],
                          0, 0,
                          DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK,
                          (void *)(intptr_t) seq) != 0) {
        ms_drm_abort_seq(scrn, seq);
        return;
    }

    /*
     * Only clear the stale region once the flip is accepted. On any failure
     * above it's left intact so the accumulated damage is retried on the
     * next block handler.
     */
    RegionEmpty(&comp->stale[back]);

    comp->flip_seq = seq;
    ms_compositor_flip_submitted(crtc);
}

void
ms_compositor_flip_handler(uint64_t frame, uint64_t usec, void *data)
{
    xf86CrtcPtr crtc = data;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    /* Back buffer is now being scanned out; swap roles. */
    if (!drmmode_crtc->compositor.direct_pending)
        drmmode_crtc->compositor.back_idx ^= 1;
    drmmode_crtc->compositor.flip_seq = 0;
    ms_compositor_flip_complete(crtc, frame, usec, FALSE);
}

void
ms_compositor_flip_abort(void *data)
{
    xf86CrtcPtr crtc = data;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    drmmode_crtc->compositor.flip_seq = 0;
    ms_compositor_flip_complete(crtc, 0, 0, TRUE);
}
