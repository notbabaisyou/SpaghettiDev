/* SPDX-License-Identifier: MIT */
/* Regression tests for per-CRTC presentation and scanout buffer retirement. */
#include "dix-config.h"

#include <assert.h>
#include "os/ftrace.h"
#include "driver.h"
#include <present.h>

#if FTRACE
void
ftrace_print(const char *format, ...)
{
}

void
ftrace_print_begin(unsigned long context, const char *format, ...)
{
}

void
ftrace_print_end(unsigned long context, const char *format, ...)
{
}
#endif

enum {
    TEST_CRTC_COUNT = 2,
    TEST_MODE_WIDTH = 1920,
    TEST_MODE_HEIGHT = 1080,
    TEST_MAX_NOTIFICATIONS = 32,
    TEST_SEQUENCE = 1,
    TEST_ACTIVE_FB = 10,
    TEST_PENDING_FB,
    TEST_TARGET_MSC = 100,
    TEST_TARGET_UST = 200,
    TEST_SECONDARY_MSC = 50,
    TEST_SECONDARY_UST = 300,
    TEST_CLOCK_START = 1000000,
    TEST_REFRESH_HZ = 100,
    TEST_PERIOD_USEC = 1000000 / TEST_REFRESH_HZ,
    TEST_MARGIN_USEC = 2000,
    TEST_TIMER_EARLY_USEC = 500,
    TEST_MARGIN_STEP_USEC = 1000,

};

enum test_event {
    TEST_SPANNING = 1,
    TEST_LOCAL,
    TEST_FIFO_FIRST,
    TEST_FIFO_SECOND,
    TEST_RETRY,
    TEST_TEARDOWN,
    TEST_BATCH_FIRST,
    TEST_BATCH_SECOND,
    TEST_AFTER_PREPARE,
    TEST_AFTER_EMPTY_PREPARE,
};

int xf86CrtcConfigPrivateIndex;
BoxRec RegionEmptyBox;
RegDataRec RegionEmptyData, RegionBrokenData;
static unsigned notifications, removed_fbs, destroyed_pixmaps;
static unsigned paint_calls, direct_calls, commit_calls;
static uint64_t notification_ids[TEST_MAX_NOTIFICATIONS];
static uint64_t notified_msc, notified_ust;
static Bool direct_scanout, paint_success = TRUE, commit_success = TRUE;
static Bool queue_success = TRUE;
static BoxRec painted;
static xf86CrtcPtr queued_crtc;
static ms_drm_abort_proc queued_abort;

struct _OsTimerRec {
    OsTimerCallback callback;
    void *arg;
    CARD32 delay;
    Bool active;
};

static uint64_t test_now, test_ust;
static Bool clock_success = TRUE, timer_success = TRUE;
static unsigned timers_created, timers_freed;

CARD64
GetTimeInMicros(void)
{
    return test_now;
}

double
xf86ModeVRefresh(const DisplayModeRec *mode)
{
    return mode->VRefresh;
}

int
ms_get_crtc_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    *ust = test_ust;
    *msc = TEST_TARGET_MSC;
    return clock_success ? Success : BadMatch;
}

OsTimerPtr
TimerSet(OsTimerPtr timer, int flags, CARD32 millis,
         OsTimerCallback callback, void *arg)
{
    assert(!flags && millis);
    if (!timer_success)
        return NULL;
    if (!timer) {
        timer = calloc(1, sizeof(*timer));
        assert(timer);
        timers_created++;
    }
    timer->callback = callback;
    timer->arg = arg;
    timer->delay = millis;
    timer->active = TRUE;
    return timer;
}

void
TimerCancel(OsTimerPtr timer)
{
    timer->active = FALSE;
}

void
TimerFree(OsTimerPtr timer)
{
    timers_freed++;
    free(timer);
}

void
AdjustWaitForDelay(void *timeout, int delay)
{
    int *value = timeout;

    if (*value < 0 || delay < *value)
        *value = delay;
}

Bool
xf86_crtc_on(xf86CrtcPtr crtc)
{
    return crtc->enabled;
}

void
present_event_notify(uint64_t id, uint64_t ust, uint64_t msc)
{
    assert(notifications < ARRAY_SIZE(notification_ids));
    notification_ids[notifications++] = id;
    notified_msc = msc;
    notified_ust = ust;
}

int
drmModeRmFB(int fd, uint32_t fb)
{
    assert(fb);
    removed_fbs++;
    return 0;
}

int
dixDestroyPixmap(void *value, XID id)
{
    PixmapPtr pixmap = value;
    assert(pixmap->refcnt > 0);
    pixmap->refcnt--;
    destroyed_pixmaps++;
    return Success;
}

Bool
ms_compositor_direct_flip(xf86CrtcPtr crtc)
{
    direct_calls++;
    return direct_scanout;
}

Bool
ms_compositor_paint(xf86CrtcPtr crtc, PixmapPtr dst, RegionPtr region)
{
    paint_calls++;
    painted = *RegionExtents(region);
    return paint_success;
}

uint32_t
ms_drm_queue_alloc(xf86CrtcPtr crtc, void *data,
                    ms_drm_handler_proc handler, ms_drm_abort_proc abort)
{
    queued_crtc = crtc;
    queued_abort = abort;
    return queue_success ? TEST_SEQUENCE : 0;
}

void
ms_drm_abort_seq(ScrnInfoPtr scrn, uint32_t seq)
{
    assert(seq == TEST_SEQUENCE);
    queued_abort(queued_crtc);
}

int
drmmode_crtc_flip(xf86CrtcPtr crtc, uint32_t fb_id, int x, int y,
                   uint32_t flags, void *data)
{
    commit_calls++;
    assert(fb_id && !x && !y);
    assert(flags == (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK));
    return commit_success ? 0 : -1;
}

static void
test_deadline_pacing(void)
{
    ScrnInfoRec scrn = { .vtSema = TRUE };
    xf86CrtcRec crtcs[TEST_CRTC_COUNT] = { 0 };
    xf86CrtcPtr pointers[] = { &crtcs[0], &crtcs[1] };
    xf86CrtcConfigRec config = { .num_crtc = TEST_CRTC_COUNT, .crtc = pointers };
    DevUnion privates[] = { { .ptr = &config } };
    drmmode_rec drmmode = { 0 };
    drmmode_crtc_private_rec dc[TEST_CRTC_COUNT] = { 0 };
    PixmapRec buffers[TEST_CRTC_COUNT][MS_COMPOSITOR_BUFFER_COUNT] = { 0 };
    drmmode_compositor_ptr comp = &dc[0].compositor;
    WindowRec window = { 0 };
    uint64_t deadline, margin;
    unsigned before, before_paint, before_timers;
    int timeout, i, j;

    scrn.privates = privates;
    for (i = 0; i < TEST_CRTC_COUNT; i++) {
        crtcs[i].scrn = &scrn;
        crtcs[i].driver_private = &dc[i];
        crtcs[i].enabled = i == 0;
        crtcs[i].mode.VRefresh = TEST_REFRESH_HZ * (i + 1);
        crtcs[i].bounds = (BoxRec) {
            i * TEST_MODE_WIDTH, 0, (i + 1) * TEST_MODE_WIDTH, TEST_MODE_HEIGHT
        };
        dc[i].drmmode = &drmmode;
        for (j = 0; j < MS_COMPOSITOR_BUFFER_COUNT; j++) {
            dc[i].compositor.fb_id[j] = i * MS_COMPOSITOR_BUFFER_COUNT + j + 1;
            dc[i].compositor.pixmap[j] = &buffers[i][j];
            RegionNull(&dc[i].compositor.stale[j]);
        }
    }
    RegionInit(&window.borderSize, &crtcs[0].bounds, 1);
    test_now = test_ust = TEST_CLOCK_START;
    before = notifications;
    before_paint = paint_calls;
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_BATCH_FIRST));
    timeout = -1;
    ms_compositor_prepare_crtc(&crtcs[0], &timeout);
    deadline = TEST_CLOCK_START + TEST_PERIOD_USEC - TEST_MARGIN_USEC;
    assert(comp->compose_deadline == deadline);
    assert(comp->compose_timer && comp->compose_timer->active);
    assert(timeout == (TEST_PERIOD_USEC - TEST_MARGIN_USEC) / 1000);
    assert(paint_calls == before_paint && notifications == before);

    /* A second client joins the same deadline instead of chasing it forward. */
    test_now += TEST_TIMER_EARLY_USEC;
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_BATCH_SECOND));
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(comp->compose_deadline == deadline && paint_calls == before_paint);
    test_now = deadline - TEST_TIMER_EARLY_USEC;
    assert(comp->compose_timer->callback(comp->compose_timer, 0,
                                        comp->compose_timer->arg) == 1);
    assert(paint_calls == before_paint);
    test_now = deadline;
    assert(!comp->compose_timer->callback(comp->compose_timer, 0,
                                         comp->compose_timer->arg));
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(paint_calls == before_paint + 1 && comp->prepared);
    assert(!comp->compose_timer->active);

    /* A Present after painting belongs to the following output frame. */
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_AFTER_PREPARE));
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(comp->events && comp->submitted_events);
    assert(notifications == before);
    test_now = test_ust = TEST_CLOCK_START + TEST_PERIOD_USEC;
    ms_compositor_flip_handler(TEST_TARGET_MSC, test_ust, &crtcs[0]);
    assert(notifications == before + 2);
    assert(notification_ids[before] == TEST_BATCH_FIRST);
    assert(notification_ids[before + 1] == TEST_BATCH_SECOND);
    assert(comp->events && comp->repaint && !comp->compose_deadline);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    test_now = comp->compose_deadline;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    test_now = test_ust += TEST_PERIOD_USEC;
    ms_compositor_flip_handler(TEST_TARGET_MSC + 1, test_ust, &crtcs[0]);
    assert(notifications == before + 3);
    assert(notification_ids[before + 2] == TEST_AFTER_PREPARE);

    /* An empty prepared batch must not consume a later Present either. */
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    test_now = comp->compose_deadline;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_AFTER_EMPTY_PREPARE));
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(comp->events && !comp->submitted_events);
    test_now = test_ust += TEST_PERIOD_USEC;
    ms_compositor_flip_handler(TEST_TARGET_MSC + 2, test_ust, &crtcs[0]);
    assert(notifications == before + 3);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    test_now = comp->compose_deadline;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    test_now = test_ust += TEST_PERIOD_USEC;
    ms_compositor_flip_handler(TEST_TARGET_MSC + 3, test_ust, &crtcs[0]);
    assert(notifications == before + 4);
    assert(notification_ids[before + 3] == TEST_AFTER_EMPTY_PREPARE);

    /* A missed refresh increases the lead; successful frames recover slowly. */
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    test_now = comp->compose_deadline;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    test_now = test_ust += 2 * TEST_PERIOD_USEC;
    ms_compositor_flip_handler(TEST_TARGET_MSC + 5, test_ust, &crtcs[0]);
    assert(comp->compose_margin == TEST_MARGIN_USEC + TEST_MARGIN_STEP_USEC);
    margin = comp->compose_margin;
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    test_now = comp->compose_deadline;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    test_now = test_ust += TEST_PERIOD_USEC;
    ms_compositor_flip_handler(TEST_TARGET_MSC + 6, test_ust, &crtcs[0]);
    assert(comp->compose_margin < margin && comp->compose_margin >= TEST_MARGIN_USEC);

    /* Different refresh rates keep independent timers and prepare deadlines. */
    crtcs[1].enabled = TRUE;
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_damage_crtc(&crtcs[1]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_prepare_crtc(&crtcs[1], NULL);
    assert(dc[1].compositor.compose_deadline < comp->compose_deadline);
    assert(dc[1].compositor.compose_timer != comp->compose_timer);
    before_paint = paint_calls;
    test_now = dc[1].compositor.compose_deadline;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(paint_calls == before_paint);
    ms_compositor_prepare_crtc(&crtcs[1], NULL);
    assert(paint_calls == before_paint + 1);

    /* DPMS or VT loss cancels a timer without rendering from its callback. */
    crtcs[0].enabled = FALSE;
    assert(!comp->compose_timer->callback(comp->compose_timer, 0,
                                         comp->compose_timer->arg));
    assert(!comp->compose_deadline && paint_calls == before_paint + 1);
    crtcs[0].enabled = TRUE;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    scrn.vtSema = FALSE;
    assert(!comp->compose_timer->callback(comp->compose_timer, 0,
                                         comp->compose_timer->arg));
    assert(!comp->compose_deadline);
    scrn.vtSema = TRUE;

    /* An active VRR scanout does not wait for a fixed-refresh deadline. */
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(comp->compose_deadline > test_now);
    dc[0].vrr_enabled = TRUE;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(comp->prepared && !comp->compose_deadline && !comp->compose_timer->active);
    dc[0].vrr_enabled = FALSE;
    comp->prepared = FALSE;

    /* Clock or timer failure falls back to preparing immediately. */
    ms_compositor_damage_crtc(&crtcs[0]);
    clock_success = FALSE;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(comp->prepared && !comp->compose_deadline);
    clock_success = TRUE;
    comp->prepared = FALSE;
    TimerFree(comp->compose_timer);
    comp->compose_timer = NULL;
    timer_success = FALSE;
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(comp->prepared && !comp->compose_timer && !comp->compose_deadline);
    timer_success = TRUE;

    /* Modeset teardown frees all per-output timers and pending state. */
    before_timers = timers_freed;
    for (i = 0; i < TEST_CRTC_COUNT; i++) {
        ms_compositor_free_crtc(&crtcs[i]);
        assert(!dc[i].compositor.compose_timer && !dc[i].compositor.compose_deadline);
        assert(!dc[i].compositor.prepared && !dc[i].compositor.prepared_tail);
        for (j = 0; j < MS_COMPOSITOR_BUFFER_COUNT; j++)
            RegionUninit(&dc[i].compositor.stale[j]);
    }
    assert(timers_freed == before_timers + 1);
    assert(timers_created == timers_freed);
    RegionUninit(&window.borderSize);
}

int
main(void)
{
    ScrnInfoRec scrn = { 0 };
    xf86CrtcRec crtcs[TEST_CRTC_COUNT] = { 0 };
    xf86CrtcPtr pointers[TEST_CRTC_COUNT] = { &crtcs[0], &crtcs[1] };
    xf86CrtcConfigRec config = { .num_crtc = TEST_CRTC_COUNT, .crtc = pointers };
    DevUnion privates[] = { { .ptr = &config } };
    drmmode_rec drmmode = { 0 };
    drmmode_crtc_private_rec dc[TEST_CRTC_COUNT] = { 0 };
    PixmapRec buffers[TEST_CRTC_COUNT][MS_COMPOSITOR_BUFFER_COUNT] = { 0 };
    WindowRec window = { 0 };
    BoxRec bounds = { 0, 0, TEST_CRTC_COUNT * TEST_MODE_WIDTH, TEST_MODE_HEIGHT };
    PixmapRec old = { .refcnt = 1 }, next = { .refcnt = 1 };
    unsigned before, before_paint, before_direct, before_commit;
    int i, j;

    scrn.privates = privates;
    RegionInit(&window.borderSize, &bounds, 1);
    for (i = 0; i < ARRAY_SIZE(crtcs); i++) {
        crtcs[i].scrn = &scrn;
        crtcs[i].driver_private = &dc[i];
        crtcs[i].enabled = TRUE;
        crtcs[i].mode.HDisplay = TEST_MODE_WIDTH;
        crtcs[i].mode.VDisplay = TEST_MODE_HEIGHT;
        crtcs[i].bounds = (BoxRec) {
            i * TEST_MODE_WIDTH, 0, (i + 1) * TEST_MODE_WIDTH, TEST_MODE_HEIGHT
        };
        dc[i].drmmode = &drmmode;
        for (j = 0; j < MS_COMPOSITOR_BUFFER_COUNT; j++) {
            dc[i].compositor.fb_id[j] = i * MS_COMPOSITOR_BUFFER_COUNT + j + 1;
            dc[i].compositor.pixmap[j] = &buffers[i][j];
            RegionNull(&dc[i].compositor.stale[j]);
        }
    }

    /* Spanning presentation reports the target's timing after both outputs. */
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_SPANNING));
    for (i = 0; i < ARRAY_SIZE(crtcs); i++) {
        assert(RegionExtents(&dc[i].compositor.stale[0])->x1 == crtcs[i].bounds.x1);
        assert(RegionExtents(&dc[i].compositor.stale[0])->x2 == crtcs[i].bounds.x2);
        ms_compositor_flip_submitted(&crtcs[i]);
    }
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);
    assert(!notifications);
    ms_compositor_flip_handler(TEST_SECONDARY_MSC, TEST_SECONDARY_UST, &crtcs[1]);
    assert(notifications == 1 && notification_ids[0] == TEST_SPANNING);
    assert(notified_msc == TEST_TARGET_MSC && notified_ust == TEST_TARGET_UST);

    /* A monitor-local window completes independently. */
    bounds.x2 = TEST_MODE_WIDTH;
    RegionReset(&window.borderSize, &bounds);
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_LOCAL));
    assert(!dc[1].compositor.events);
    ms_compositor_flip_submitted(&crtcs[0]);
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);
    assert(notification_ids[notifications - 1] == TEST_LOCAL);

    /* Multiple frames in one commit retire in submission order. */
    bounds.x2 = TEST_CRTC_COUNT * TEST_MODE_WIDTH;
    RegionReset(&window.borderSize, &bounds);
    before = notifications;
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_FIFO_FIRST));
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_FIFO_SECOND));
    for (i = 0; i < ARRAY_SIZE(crtcs); i++)
        ms_compositor_flip_submitted(&crtcs[i]);
    ms_compositor_flip_handler(TEST_SECONDARY_MSC, TEST_SECONDARY_UST, &crtcs[1]);
    assert(notifications == before);
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);
    assert(notifications == before + 2);
    assert(notification_ids[before] == TEST_FIFO_FIRST);
    assert(notification_ids[before + 1] == TEST_FIFO_SECOND);

    /* Rotation and scaling retain logical bounds, including negative origins. */
    crtcs[0].bounds = (BoxRec) {
        -TEST_MODE_HEIGHT, TEST_MODE_WIDTH, TEST_MODE_HEIGHT,
        TEST_MODE_WIDTH + TEST_MODE_WIDTH
    };
    ms_compositor_damage_crtc(&crtcs[0]);
    for (i = 0; i < MS_COMPOSITOR_BUFFER_COUNT; i++)
        assert(!memcmp(RegionExtents(&dc[0].compositor.stale[i]),
                       &crtcs[0].bounds, sizeof(BoxRec)));

    /* Direct scanout skips painting. */
    before_paint = paint_calls;
    before_direct = direct_calls;
    direct_scanout = TRUE;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(direct_calls == before_direct + 1 && paint_calls == before_paint);
    assert(!dc[0].compositor.prepared);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(direct_calls == before_direct + 1 && paint_calls == before_paint);
    direct_scanout = FALSE;

    /* A failed paint keeps damage and prevents a commit. */
    before_commit = commit_calls;
    ms_compositor_damage_crtc(&crtcs[0]);
    paint_success = FALSE;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(!dc[0].compositor.prepared && commit_calls == before_commit);
    assert(RegionNotEmpty(&dc[0].compositor.stale[dc[0].compositor.back_idx]));
    paint_success = TRUE;

    /* Returning from direct scanout repaints the complete logical viewport. */
    dc[0].compositor.scanout_fb = TEST_ACTIVE_FB;
    dc[0].compositor.scanout_pixmap = &old;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(dc[0].compositor.prepared);
    assert(!memcmp(&painted, &crtcs[0].bounds, sizeof(BoxRec)));

    /* A rejected commit retains its Present and damage until the retry. */
    before = notifications;
    commit_success = FALSE;
    assert(ms_compositor_queue_present(&crtcs[0], &window, TEST_RETRY));
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(notifications == before && dc[0].compositor.events);
    assert(!dc[0].compositor.flip_seq);
    assert(RegionNotEmpty(&dc[0].compositor.stale[dc[0].compositor.back_idx]));
    commit_success = TRUE;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(dc[0].compositor.flip_seq == TEST_SEQUENCE);
    assert(RegionNil(&dc[0].compositor.stale[dc[0].compositor.back_idx]));
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);
    /* The other CRTC participates because this window spans it. */
    ms_compositor_flip_submitted(&crtcs[1]);
    ms_compositor_flip_handler(TEST_SECONDARY_MSC, TEST_SECONDARY_UST, &crtcs[1]);
    assert(notifications == before + 1 && notification_ids[before] == TEST_RETRY);
    assert(old.refcnt == 0 && removed_fbs == 1 && destroyed_pixmaps == 1);

    /* A stale back buffer alone must not submit another copy of the frame. */
    before_paint = paint_calls;
    before_commit = commit_calls;
    assert(RegionNotEmpty(&dc[0].compositor.stale[dc[0].compositor.back_idx]));
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(paint_calls == before_paint && commit_calls == before_commit);

    /* Fresh damage repairs the accumulated stale region on the next frame. */
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(paint_calls == before_paint + 1);
    assert(!memcmp(&painted, &crtcs[0].bounds, sizeof(BoxRec)));
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);

    /* Queue allocation failure leaves the new frame available for retry. */
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    before_commit = commit_calls;
    queue_success = FALSE;
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(commit_calls == before_commit && !dc[0].compositor.flip_seq);
    queue_success = TRUE;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(commit_calls == before_commit + 1);
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);

    /* Damage arriving after preparation remains pending after submission. */
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    ms_compositor_damage_crtc(&crtcs[0]);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    assert(RegionNotEmpty(&dc[0].compositor.stale[dc[0].compositor.back_idx]));
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);
    before_paint = paint_calls;
    ms_compositor_prepare_crtc(&crtcs[0], NULL);
    assert(paint_calls == before_paint + 1);
    ms_compositor_flip_crtc(&scrn, &crtcs[0]);
    ms_compositor_flip_handler(TEST_TARGET_MSC, TEST_TARGET_UST, &crtcs[0]);

    /* Aborting a direct commit preserves the current scanout. */
    old.refcnt = 1;
    dc[0].compositor.scanout_fb = TEST_ACTIVE_FB;
    dc[0].compositor.scanout_pixmap = &old;
    dc[0].compositor.pending_fb = TEST_PENDING_FB;
    dc[0].compositor.pending_pixmap = &next;
    assert(!ms_compositor_flip_complete(&crtcs[0], 0, 0, TRUE));
    assert(dc[0].compositor.scanout_fb == TEST_ACTIVE_FB && old.refcnt == 1);
    assert(next.refcnt == 0 && removed_fbs == 2 && destroyed_pixmaps == 2);

    /* Reusing the same scanout framebuffer keeps its outstanding reference. */
    old.refcnt++;
    dc[0].compositor.pending_fb = TEST_ACTIVE_FB;
    dc[0].compositor.pending_pixmap = &old;
    assert(!ms_compositor_flip_complete(&crtcs[0], TEST_TARGET_MSC,
                                        TEST_TARGET_UST, FALSE));
    assert(old.refcnt == 1 && removed_fbs == 2);

    /* Teardown completes outstanding groups once and retires scanout buffers. */
    before = notifications;
    assert(ms_compositor_queue_present(&crtcs[0], NULL, TEST_TEARDOWN));
    ms_compositor_free_crtc(&crtcs[0]);
    assert(notifications == before);
    ms_compositor_free_crtc(&crtcs[1]);
    assert(notifications == before + 1);
    assert(notification_ids[before] == TEST_TEARDOWN && old.refcnt == 0);
    for (i = 0; i < ARRAY_SIZE(crtcs); i++)
        for (j = 0; j < MS_COMPOSITOR_BUFFER_COUNT; j++)
            RegionUninit(&dc[i].compositor.stale[j]);
    RegionUninit(&window.borderSize);
    test_deadline_pacing();
    return 0;
}
