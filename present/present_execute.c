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
#include <dix-config.h>

#include "present_priv.h"
#include <unistd.h>

/*
 * Called when the wait fence is triggered; just gets the current msc/ust and
 * calls the proper execute again. That will re-check the fence and pend the
 * request again if it's still not actually ready
 */
static void
present_wait_fence_triggered(void *param)
{
    present_vblank_ptr      vblank = param;
    ScreenPtr               screen = vblank->screen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    screen_priv->re_execute(vblank);
}

#ifdef DRI3
enum { PRESENT_ACQUIRE_POLL_MS = 4 };

static void
present_acquire_ready(present_vblank_ptr vblank)
{
    present_vblank_cancel_acquire(vblank);
    present_screen_priv(vblank->screen)->re_execute(vblank);
}

static void
present_acquire_fd_ready(int fd, int xevents, void *data)
{
    present_acquire_ready(data);
}

static Bool
present_watch_acquire(present_vblank_ptr vblank)
{
    struct dri3_syncobj *timeline = vblank->acquire_syncobj;
    uint64_t point = vblank->acquire_point;

    if (!timeline->has_fence(timeline, point))
        return FALSE;

    int fd = timeline->export_fence(timeline, point);

    if (fd < 0)
        return FALSE;
    if (!SetNotifyFd(fd, present_acquire_fd_ready, X_NOTIFY_READ, vblank)) {
        close(fd);
        return FALSE;
    }

    vblank->acquire_fd = fd;
    return TRUE;
}

static CARD32
present_poll_acquire(OsTimerPtr timer, CARD32 now, void *data)
{
    present_vblank_ptr vblank = data;
    struct dri3_syncobj *timeline = vblank->acquire_syncobj;

    if (timeline->is_signaled(timeline, vblank->acquire_point)) {
        present_acquire_ready(vblank);
        return 0;
    }

    if (!present_watch_acquire(vblank))
        return PRESENT_ACQUIRE_POLL_MS;

    vblank->acquire_timer = NULL;
    TimerFree(timer);
    return 0;
}
#endif /* DRI3 */

Bool
present_execute_wait(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

#ifdef DRI3
    if (vblank->acquire_fd >= 0 || vblank->acquire_timer)
        return TRUE;
#endif

    /* We may have to requeue for the next MSC if check_flip_window prevented
     * using a flip.
     */
    if (vblank->exec_msc == crtc_msc + 1 &&
        screen_priv->queue_vblank(screen, window, vblank->crtc, vblank->event_id,
                                  vblank->exec_msc) == Success)
        return TRUE;

    if (vblank->wait_fence) {
        if (!present_fence_check_triggered(vblank->wait_fence)) {
            present_fence_set_callback(vblank->wait_fence, present_wait_fence_triggered, vblank);
            return TRUE;
        }
    }

#ifdef DRI3
    struct dri3_syncobj *timeline = vblank->acquire_syncobj;

    if (timeline && vblank->pixmap &&
        !timeline->is_signaled(timeline, vblank->acquire_point)) {
        /*
         * A sync_file watch can be canceled when the request is dropped.
         * An unsubmitted timeline point has no fence to export yet.
         */
        if (present_watch_acquire(vblank))
            return TRUE;
        vblank->acquire_timer = TimerSet(NULL, 0, PRESENT_ACQUIRE_POLL_MS,
                                         present_poll_acquire, vblank);
        if (vblank->acquire_timer)
            return TRUE;

        /* Skip safely if there is no way to wait without blocking X. */
        present_vblank_scrap(vblank);
    }
#endif /* DRI3 */

    return FALSE;
}

void
present_execute_copy(present_vblank_ptr vblank, PixmapPtr pixmap, WindowPtr window, uint64_t crtc_msc)
{
    ScreenPtr screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    /* If present_flip failed, we may have to requeue for the next MSC */
    if (vblank->exec_msc == crtc_msc + 1 &&
        Success == screen_priv->queue_vblank(screen,
                                             window,
                                             vblank->crtc,
                                             vblank->event_id,
                                             vblank->exec_msc)) {
        vblank->queued = TRUE;
        return;
    }

    Bool copied = present_copy_region(&window->drawable, pixmap,
                                       vblank->update, vblank->x_off,
                                       vblank->y_off);

    if (!copied)
        vblank->mode = PresentCompleteModeSkip;

    /* present_copy_region sticks the region into a scratch GC,
     * which is then freed, freeing the region
     */
    vblank->update = NULL;
#ifdef DRI3
    if (vblank->release_syncobj && screen_priv->flush_fenced) {
        int fence_fd = screen_priv->flush_fenced(window);

        vblank->release_signaled = TRUE;
        vblank->release_syncobj->import_fence(vblank->release_syncobj,
                                              vblank->release_point, fence_fd);
        return;
    }
#endif
    screen_priv->flush(window);
    present_pixmap_idle(vblank);
}

void
present_execute_post(present_vblank_ptr vblank, PixmapPtr pixmap,
                     WindowPtr window, uint64_t ust, uint64_t crtc_msc)
{
    Bool format_fallback =
        vblank->reason == PRESENT_FLIP_REASON_BUFFER_FORMAT;

    if (vblank->mode == PresentCompleteModeCopy && vblank->has_suboptimal &&
        format_fallback)
        vblank->mode = PresentCompleteModeSuboptimalCopy;

    present_vblank_notify(vblank, ust, crtc_msc);
    present_vblank_destroy(vblank);
}
