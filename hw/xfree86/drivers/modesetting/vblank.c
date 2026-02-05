/*
 * Copyright © 2013 Keith Packard
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

/** @file vblank.c
 *
 * Support for tracking the DRM's vblank events.
 */

#include "dix-config.h"

#include <errno.h>
#include <unistd.h>

#include <xf86.h>
#include <xf86Crtc.h>

#include <sys/timerfd.h>

#include "driver.h"
#include "drmmode_display.h"

typedef struct _VblankTimerRec {
    struct _VblankTimerRec *next, *last;
    struct timespec timespec;
    uint64_t msc;
    uint32_t seq;
} VblankTimerRec, *VblankTimerPtr;

/**
 * List of software vblank timers used to schedule copying.  Vblank
 * events are sent after the blanking period on some drivers, so they
 * cannot be reliably used to schedule copying.  In fact, a more
 * reliable method is to just ask for the time with drmWaitVBlank, and
 * to schedule a software timer for that time.
 */
static VblankTimerRec vblank_timers;

/** timerfd descriptor used for this.  */
static int vblank_timer_fd;

/**
 * Tracking for outstanding events queued to the kernel.
 *
 * Each list entry is a struct ms_drm_queue, which has a uint32_t
 * value generated from drm_seq that identifies the event and a
 * reference back to the crtc/screen associated with the event.  It's
 * done this way rather than in the screen because we want to be able
 * to drain the list of event handlers that should be called at server
 * regen time, even though we don't close the drm fd and have no way
 * to actually drain the kernel events.
 */
static struct xorg_list ms_drm_queue;
static uint32_t ms_drm_seq;

static void box_intersect(BoxPtr dest, BoxPtr a, BoxPtr b)
{
    dest->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    dest->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    if (dest->x1 >= dest->x2) {
        dest->x1 = dest->x2 = dest->y1 = dest->y2 = 0;
        return;
    }

    dest->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    dest->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    if (dest->y1 >= dest->y2)
        dest->x1 = dest->x2 = dest->y1 = dest->y2 = 0;
}

static void rr_crtc_box(RRCrtcPtr crtc, BoxPtr crtc_box)
{
    if (crtc->mode) {
        crtc_box->x1 = crtc->x;
        crtc_box->y1 = crtc->y;
        switch (crtc->rotation) {
            case RR_Rotate_0:
            case RR_Rotate_180:
            default:
                crtc_box->x2 = crtc->x + crtc->mode->mode.width;
                crtc_box->y2 = crtc->y + crtc->mode->mode.height;
                break;
            case RR_Rotate_90:
            case RR_Rotate_270:
                crtc_box->x2 = crtc->x + crtc->mode->mode.height;
                crtc_box->y2 = crtc->y + crtc->mode->mode.width;
                break;
        }
    } else
        crtc_box->x1 = crtc_box->x2 = crtc_box->y1 = crtc_box->y2 = 0;
}

static int box_area(BoxPtr box)
{
    return (int)(box->x2 - box->x1) * (int)(box->y2 - box->y1);
}

static Bool rr_crtc_on(RRCrtcPtr crtc, Bool crtc_is_xf86_hint)
{
    if (!crtc) {
        return FALSE;
    }
    if (crtc_is_xf86_hint && crtc->devPrivate) {
         return xf86_crtc_on(crtc->devPrivate);
    } else {
        return !!crtc->mode;
    }
}

Bool
xf86_crtc_on(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return crtc->enabled && drmmode_crtc->dpms_mode == DPMSModeOn;
}


/*
 * Return the crtc covering 'box'. If two crtcs cover a portion of
 * 'box', then prefer the crtc with greater coverage.
 */
static RRCrtcPtr
rr_crtc_covering_box(ScreenPtr pScreen, BoxPtr box, Bool screen_is_xf86_hint)
{
    rrScrPrivPtr pScrPriv;
    RROutputPtr primary_output;
    RRCrtcPtr crtc, best_crtc, primary_crtc;
    int coverage, best_coverage;
    int c;
    BoxRec crtc_box, cover_box;

    best_crtc = NULL;
    best_coverage = 0;

    if (!dixPrivateKeyRegistered(rrPrivKey))
        return NULL;

    pScrPriv = rrGetScrPriv(pScreen);

    if (!pScrPriv)
        return NULL;

    primary_crtc = NULL;
    primary_output = RRFirstOutput(pScreen);
    if (primary_output)
        primary_crtc = primary_output->crtc;

    for (c = 0; c < pScrPriv->numCrtcs; c++) {
        crtc = pScrPriv->crtcs[c];

        /* If the CRTC is off, treat it as not covering */
        if (!rr_crtc_on(crtc, screen_is_xf86_hint))
            continue;

        rr_crtc_box(crtc, &crtc_box);
        box_intersect(&cover_box, &crtc_box, box);
        coverage = box_area(&cover_box);
        if ((coverage > best_coverage) ||
            (coverage == best_coverage && crtc == primary_crtc)) {
            best_crtc = crtc;
            best_coverage = coverage;
        }
    }

    return best_crtc;
}

static RRCrtcPtr
rr_crtc_covering_box_on_secondary(ScreenPtr pScreen, BoxPtr box)
{
    if (!pScreen->isGPU) {
        ScreenPtr secondary;
        RRCrtcPtr crtc = NULL;

        xorg_list_for_each_entry(secondary, &pScreen->secondary_list, secondary_head) {
            if (!secondary->is_output_secondary)
                continue;

            crtc = rr_crtc_covering_box(secondary, box, FALSE);
            if (crtc)
                return crtc;
        }
    }

    return NULL;
}

xf86CrtcPtr
ms_dri2_crtc_covering_drawable(DrawablePtr pDraw)
{
    ScreenPtr pScreen = pDraw->pScreen;
    RRCrtcPtr crtc = NULL;
    BoxRec box;

    box.x1 = pDraw->x;
    box.y1 = pDraw->y;
    box.x2 = box.x1 + pDraw->width;
    box.y2 = box.y1 + pDraw->height;

    crtc = rr_crtc_covering_box(pScreen, &box, TRUE);
    if (crtc) {
        return crtc->devPrivate;
    }
    return NULL;
}

RRCrtcPtr
ms_randr_crtc_covering_drawable(DrawablePtr pDraw)
{
    ScreenPtr pScreen = pDraw->pScreen;
    RRCrtcPtr crtc = NULL;
    BoxRec box;

    box.x1 = pDraw->x;
    box.y1 = pDraw->y;
    box.x2 = box.x1 + pDraw->width;
    box.y2 = box.y1 + pDraw->height;

    crtc = rr_crtc_covering_box(pScreen, &box, TRUE);
    if (!crtc) {
        crtc = rr_crtc_covering_box_on_secondary(pScreen, &box);
    }
    return crtc;
}

static Bool
ms_get_kernel_ust_msc(xf86CrtcPtr crtc,
                      uint64_t *msc, uint64_t *ust)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmVBlank vbl;
    int ret;

    if (ms->has_queue_sequence || !ms->tried_queue_sequence) {
        uint64_t ns;
        ms->tried_queue_sequence = TRUE;

        ret = drmCrtcGetSequence(ms->fd, drmmode_crtc->mode_crtc->crtc_id,
                                 msc, &ns);
        if (ret != -1 || (errno != ENOTTY && errno != EINVAL)) {
            ms->has_queue_sequence = TRUE;
            if (ret == 0)
                *ust = ns / 1000;
            return ret == 0;
        }
    }
    /* Get current count */
    vbl.request.type = DRM_VBLANK_RELATIVE | drmmode_crtc->vblank_pipe;
    vbl.request.sequence = 0;
    vbl.request.signal = 0;
    ret = drmWaitVBlank(ms->fd, &vbl);
    if (ret) {
        *msc = 0;
        *ust = 0;
        return FALSE;
    } else {
        *msc = vbl.reply.sequence;
        *ust = (CARD64) vbl.reply.tval_sec * 1000000 + vbl.reply.tval_usec;
        return TRUE;
    }
}

#if 0
static void
ms_drm_set_seq_msc(uint32_t seq, uint64_t msc)
{
    struct ms_drm_queue *q;

    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->seq == seq) {
            q->msc = msc;
            break;
        }
    }
}

static Bool
ms_queue_coalesce(xf86CrtcPtr crtc, uint32_t seq, uint64_t msc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    /* If the next MSC is too late, then this event can't be coalesced */
    if (msc < drmmode_crtc->next_msc)
        return FALSE;

    /* Set the target MSC on this sequence number */
    ms_drm_set_seq_msc(seq, msc);
    return TRUE;
}
#endif

/**
 * Convert a 32-bit or 64-bit kernel MSC sequence number to a 64-bit local
 * sequence number, adding in the high 32 bits, and dealing with 32-bit
 * wrapping if needed.
 */
uint64_t
ms_kernel_msc_to_crtc_msc(xf86CrtcPtr crtc, uint64_t sequence, Bool is64bit)
{
    drmmode_crtc_private_rec *drmmode_crtc = crtc->driver_private;

    if (!is64bit) {
        /* sequence is provided as a 32 bit value from one of the 32 bit apis,
         * e.g., drmWaitVBlank(), classic vblank events, or pageflip events.
         *
         * Track and handle 32-Bit wrapping, somewhat robust against occasional
         * out-of-order not always monotonically increasing sequence values.
         */
        if ((int64_t) sequence < ((int64_t) drmmode_crtc->msc_prev - 0x40000000))
            drmmode_crtc->msc_high += 0x100000000L;

        if ((int64_t) sequence > ((int64_t) drmmode_crtc->msc_prev + 0x40000000))
            drmmode_crtc->msc_high -= 0x100000000L;

        drmmode_crtc->msc_prev = sequence;

        return drmmode_crtc->msc_high + sequence;
    }

    /* True 64-Bit sequence from Linux 4.15+ 64-Bit drmCrtcGetSequence /
     * drmCrtcQueueSequence apis and events. Pass through sequence unmodified,
     * but update the 32-bit tracking variables with reliable ground truth.
     *
     * With 64-Bit api in use, the only !is64bit input is from pageflip events,
     * and any pageflip event is usually preceded by some is64bit input from
     * swap scheduling, so this should provide reliable mapping for pageflip
     * events based on true 64-bit input as baseline as well.
     */
    drmmode_crtc->msc_prev = sequence;
    drmmode_crtc->msc_high = sequence & 0xffffffff00000000;

    return sequence;
}

int
ms_get_crtc_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    uint64_t kernel_msc;

    if (!ms_get_kernel_ust_msc(crtc, &kernel_msc, ust))
        return BadMatch;
    *msc = ms_kernel_msc_to_crtc_msc(crtc, kernel_msc, ms->has_queue_sequence);

    return Success;
}

/**
 * Check for pending DRM events and process them.
 */
static void
ms_drm_socket_handler(int fd, int ready, void *data)
{
    ScreenPtr screen = data;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);

    if (data == NULL)
        return;

    drmHandleEvent(fd, &ms->event_context);
}

/*
 * Enqueue a potential drm response; when the associated response
 * appears, we've got data to pass to the handler from here
 */
uint32_t
ms_drm_queue_alloc(xf86CrtcPtr crtc,
                   void *data,
                   ms_drm_handler_proc handler,
                   ms_drm_abort_proc abort)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct ms_drm_queue *q;

    q = calloc(1, sizeof(struct ms_drm_queue));

    if (!q)
        return 0;
    if (!ms_drm_seq)
        ++ms_drm_seq;
    q->seq = ms_drm_seq++;
    q->msc = UINT64_MAX;
    q->scrn = scrn;
    q->crtc = crtc;
    q->data = data;
    q->handler = handler;
    q->abort = abort;

    /* Keep the list formatted in ascending order of sequence number */
    xorg_list_append(&q->list, &ms_drm_queue);

    return q->seq;
}

static void
AbortVblankTimer(uint32_t seq)
{
    VblankTimerPtr last, next;

    next = vblank_timers.next;
    while (next != &vblank_timers)
    {
        last = next;
        next = last->next;

        if (last->seq == seq)
        {
            last->next->last = last->last;
            last->last->next = last->next;
            free(last);
            return;
        }
    }
}

/**
 * Abort one queued DRM entry, removing it
 * from the list, calling the abort function and
 * freeing the memory
 */
static void
ms_drm_abort_one(struct ms_drm_queue *q)
{
    if (q->aborted)
        return;

    /* Abort any pending vblank timer for the same sequence.  */
    AbortVblankTimer (q->seq);

    xorg_list_del(&q->list);
    q->abort(q->data);
    free(q);
}

/**
 * Abort all queued entries on a specific scrn, used
 * when resetting the X server
 */
static void
ms_drm_abort_scrn(ScrnInfoPtr scrn)
{
    struct ms_drm_queue *q, *tmp;

    xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
        if (q->scrn == scrn)
            ms_drm_abort_one(q);
    }
}

/**
 * Abort by drm queue sequence number.
 */
void
ms_drm_abort_seq(ScrnInfoPtr scrn, uint32_t seq)
{
    struct ms_drm_queue *q, *tmp;

    xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
        if (q->seq == seq) {
            ms_drm_abort_one(q);
            break;
        }
    }
}

/*
 * Externally usable abort function that uses a callback to match a single
 * queued entry to abort
 */
void
ms_drm_abort(ScrnInfoPtr scrn, Bool (*match)(void *data, void *match_data),
             void *match_data)
{
    struct ms_drm_queue *q;

    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (match(q->data, match_data)) {
            ms_drm_abort_one(q);
            break;
        }
    }
}

/*
 * General DRM kernel handler. Looks for the matching sequence number in the
 * drm event queue and calls the handler for it.
 */
static void ms_drm_sequence_handler(int, uint64_t, uint64_t, Bool, uint64_t);

static void
RunVblankTimer(VblankTimerPtr timer, struct timespec time)
{
    timer->last->next = timer->next;
    timer->next->last = timer->last;
    ms_drm_sequence_handler(-1, timer->msc,
                            (time.tv_sec * 1000000000 + time.tv_nsec),
                            xFalse, timer->seq);
    free(timer);
}

static void
CheckVblank(void)
{
    VblankTimerPtr current, last;
    struct timespec now, prime;
    struct itimerspec new_value;
    static Bool inside;

    if (inside)
        return;

#ifdef CLOCK_MONOTONIC_COARSE
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
#else
    clock_gettime(CLOCK_MONOTONIC, &now);
#endif

    prime.tv_sec = 0;
    prime.tv_nsec = 0;

    current = vblank_timers.next;

    /* This function is reentrant after this point! */
    inside = xTrue;

    while (current != &vblank_timers)
    {
        last = current;
        current = last->next;

        if (last->timespec.tv_sec < now.tv_sec ||
            (last->timespec.tv_sec == now.tv_sec && last->timespec.tv_nsec <= now.tv_nsec)) {
            RunVblankTimer(last, now);
        } else {
            /* Now, find the earliest vblank and prime the timerfd for
             * that.  */
            if ((!prime.tv_sec && !prime.tv_nsec) ||
                (last->timespec.tv_sec < prime.tv_sec) ||
                (last->timespec.tv_sec == prime.tv_sec && last->timespec.tv_nsec < prime.tv_nsec))
                prime = last->timespec;
        }
    }

    new_value.it_interval.tv_sec = 0;
    new_value.it_interval.tv_nsec = 0;
    new_value.it_value = prime;

    timerfd_settime(vblank_timer_fd, TFD_TIMER_ABSTIME,
                    &new_value, NULL);
    inside = xFalse;
}

static void
HandleVblankTimerReadable(int fd, int ready, void *data)
{
    uint64_t nexpired;

    if (read(fd, &nexpired, sizeof(nexpired)) < sizeof(nexpired))
        return;

    /* nexpired is ignored from here onwards.  */
    CheckVblank();
}

static Bool
AddVblankTimer(xf86CrtcPtr crtc, drmVBlankReply *reply,
               uint32_t seq, uint32_t msc)
{
    VblankTimerPtr timer;
    drmmode_crtc_private_ptr msCrtc;
    drmModeModeInfoPtr mode_info;
    int64_t value, vblank_duration_ns, refresh_duration_ns;

    /* Compute the vertical blanking period.  */
    msCrtc = crtc->driver_private;
    mode_info = &msCrtc->mode_crtc->mode;

    if (mode_info->htotal <= 0 || mode_info->vtotal <= 0)
        return xFalse;

    timer = malloc(sizeof(VblankTimerRec));

    if (!timer)
        return xFalse;

    timer->timespec.tv_sec = reply->tval_sec;
    timer->timespec.tv_nsec = reply->tval_usec * 1000;

    /* Get to the specified frame by repeatedly adding the refresh
     * period until the desired sequence is reached.  */
    value = mode_info->vtotal * mode_info->htotal;

    if (mode_info->flags & DRM_MODE_FLAG_DBLSCAN)
        value += value;

    value = (value * 1000 + mode_info->clock - 1) / mode_info->clock;
    refresh_duration_ns = value * 1000 * min(1, (msc - reply->sequence));

    timer->timespec.tv_nsec += refresh_duration_ns;

    while (timer->timespec.tv_nsec >= 1000000000)
    {
        timer->timespec.tv_sec++;
        timer->timespec.tv_nsec -= 1000000000;
    }

    /* Now, subtract the vblank period.  */
    value = mode_info->vsync_end - mode_info->vsync_start;
    value *= mode_info->htotal;

    if (mode_info->flags & DRM_MODE_FLAG_DBLSCAN)
        value += value;

    /* A grace period of about 2 ms is added to account for scheduling
     * delays and the amount of time it takes for the copy to
     * happen.  */
    value = ((value * 1000 + mode_info->clock - 1) / mode_info->clock + 2000);

    vblank_duration_ns = value * 1000;

    if (vblank_duration_ns > timer->timespec.tv_nsec)
    {
        vblank_duration_ns -= timer->timespec.tv_nsec;
        timer->timespec.tv_nsec = 0;

        while (vblank_duration_ns >= 1000000000)
        {
            timer->timespec.tv_sec--;
            vblank_duration_ns -= 1000000000;
        }

        if (vblank_duration_ns > 0)
        {
            timer->timespec.tv_sec -= 1;
            timer->timespec.tv_nsec = 1000000000 - vblank_duration_ns;
        }
    }
    else
    {
        timer->timespec.tv_nsec -= vblank_duration_ns;
    }

    timer->msc = msc;
    timer->seq = seq;

    /* Link this to the end of the list.  This function can be called
     * within CheckVblank.  */
    timer->next = &vblank_timers;
    timer->last = vblank_timers.last;
    vblank_timers.last->next = timer;
    vblank_timers.last = timer;

    CheckVblank();
    return xTrue;
}

static void
ms_drm_sequence_handler(int fd, uint64_t frame, uint64_t ns, Bool is64bit, uint64_t user_data)
{
    struct ms_drm_queue *q, *tmp;
    uint32_t seq = (uint32_t) user_data;
    xf86CrtcPtr crtc = NULL;
    drmmode_crtc_private_ptr drmmode_crtc;
    uint64_t msc, next_msc = UINT64_MAX;

    /* Handle the seq for this event first in order to get the CRTC */
    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->seq == seq) {
            crtc = q->crtc;
            msc = ms_kernel_msc_to_crtc_msc(crtc, frame, is64bit);

            /* Write the current MSC to this event to ensure its handler runs in
             * the loop below. This is done because we don't want to run the
             * handler right now, since we need to ensure all events are handled
             * in FIFO order with respect to one another. Otherwise, if this
             * event were handled first just because it was queued to the
             * kernel, it could run before older events expiring at this MSC.
             */
            q->msc = msc;
            break;
        }
    }

    if (!crtc)
        return;

    /* Now run all of the vblank events for this CRTC with an expired MSC */
    xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
        if (q->crtc == crtc && q->msc <= msc) {
            xorg_list_del(&q->list);
            if (!q->aborted)
                q->handler(msc, ns / 1000, q->data);
            free(q);
        }
    }

    /* Find this CRTC's next queued MSC and next non-queued MSC to be handled */
    msc = UINT64_MAX;
    xorg_list_for_each_entry(q, &ms_drm_queue, list) {
        if (q->crtc == crtc) {
            if (q->msc < msc) {
                msc = q->msc;
                seq = q->seq;
            }
        }
    }

    /* Queue an event if the next queued MSC isn't soon enough */
    drmmode_crtc = crtc->driver_private;
    drmmode_crtc->next_msc = next_msc;
    if (msc < next_msc && !ms_queue_vblank(crtc, MS_QUEUE_ABSOLUTE, msc, NULL, seq)) {
        xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
                   "failed to queue next vblank event, aborting lost events\n");
        xorg_list_for_each_entry_safe(q, tmp, &ms_drm_queue, list) {
            if (q->crtc == crtc && q->msc < next_msc)
                ms_drm_abort_one(q);
        }
    }
}

static void
ms_drm_sequence_handler_64bit(int fd, uint64_t frame, uint64_t ns, uint64_t user_data)
{
    /* frame is true 64 bit wrapped into 64 bit */
    ms_drm_sequence_handler(fd, frame, ns, TRUE, user_data);
}

static void
ms_drm_handler(int fd, uint32_t frame, uint32_t sec, uint32_t usec,
               void *user_ptr)
{
    /* frame is 32 bit wrapped into 64 bit */
    ms_drm_sequence_handler(fd, frame, ((uint64_t) sec * 1000000 + usec) * 1000,
                            FALSE, (uint32_t) (uintptr_t) user_ptr);
}

Bool
ms_drm_queue_is_empty(void)
{
    return xorg_list_is_empty(&ms_drm_queue);
}

Bool
ms_queue_vblank(xf86CrtcPtr crtc, ms_queue_flag flags,
                uint64_t msc, uint64_t *msc_queued, uint32_t seq)
{
    ScreenPtr screen = crtc->randr_crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmVBlank vbl;
    int ret;

#if 0
    /* Try coalescing this event into another to avoid event queue exhaustion */
    if (flags == MS_QUEUE_ABSOLUTE && ms_queue_coalesce(crtc, seq, msc))
        return TRUE;
#endif
    
    vbl.request.type = drmmode_crtc->vblank_pipe;
    vbl.request.sequence = 0;
    vbl.request.signal = 0;

    vbl.request.type |= DRM_VBLANK_RELATIVE;

    ret = drmWaitVBlank(ms->fd, &vbl);
    if (ret || (!vbl.reply.tval_sec && !vbl.reply.tval_usec))
    {
        if (msc_queued)
            *msc_queued = 0;
        return FALSE;
    }
    else
    {
        if (msc_queued)
            *msc_queued = msc;
        return AddVblankTimer(crtc, &vbl.reply, seq, msc);
    }
}

Bool
ms_vblank_screen_init(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    modesettingEntPtr ms_ent = ms_ent_priv(scrn);
    xorg_list_init(&ms_drm_queue);

    if (!vblank_timers.next)
    {
        vblank_timers.next = &vblank_timers;
        vblank_timers.last = &vblank_timers;

        /* No CLOCK_MONOTONIC_COARSE equivalent... */
        vblank_timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (vblank_timer_fd < 0)
        {
            xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                       "failed to create timer fd for vblank tracking\n");
            return xFalse;
        }

        SetNotifyFd(vblank_timer_fd, HandleVblankTimerReadable,
                    X_NOTIFY_READ, NULL);
    }

    ms->event_context.version = 4;
    ms->event_context.vblank_handler = ms_drm_handler;
    ms->event_context.page_flip_handler = ms_drm_handler;
    ms->event_context.sequence_handler = ms_drm_sequence_handler_64bit;

    /* We need to re-register the DRM fd for the synchronisation
     * feedback on every server generation, so perform the
     * registration within ScreenInit and not PreInit.
     */
    if (ms_ent->fd_wakeup_registered != serverGeneration) {
        SetNotifyFd(ms->fd, ms_drm_socket_handler, X_NOTIFY_READ, screen);
        ms_ent->fd_wakeup_registered = serverGeneration;
        ms_ent->fd_wakeup_ref = 1;
    } else
        ms_ent->fd_wakeup_ref++;

    return TRUE;
}

void
ms_vblank_close_screen(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    modesettingEntPtr ms_ent = ms_ent_priv(scrn);

    ms_drm_abort_scrn(scrn);

    if (vblank_timer_fd) {
        close(vblank_timer_fd);
        vblank_timer_fd = 0;
    }

    if (ms_ent->fd_wakeup_registered == serverGeneration &&
        !--ms_ent->fd_wakeup_ref) {
        RemoveNotifyFd(ms->fd);
    }
}
