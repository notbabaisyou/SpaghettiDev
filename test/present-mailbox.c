/* SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "dix-config.h"
#include <assert.h>
#include "../present/present_scmd.c"
#include "../present/present_vblank.c"

enum { TEST_MSC = 100, TEST_TARGET_MSC = 200, TEST_SERIAL = 1 };
DevPrivateKeyRec present_screen_private_key = { .initialized = TRUE };
DevPrivateKeyRec present_window_private_key = { .initialized = TRUE };
static present_window_priv_rec window_private;
static unsigned idles, destroyed, unexpected_calls;

#if FTRACE
void ftrace_print(const char *format, ...) {}
void ftrace_print_begin(unsigned long context, const char *format, ...) {}
void ftrace_print_end(unsigned long context, const char *format, ...) {}
#endif

present_window_priv_ptr
present_get_window_priv(WindowPtr window, Bool create)
{
    return &window_private;
}

Bool
CompositeServerActive(ScreenPtr screen)
{
    return FALSE;
}

int
present_fake_get_ust_msc(ScreenPtr screen, uint64_t *ust, uint64_t *msc)
{
    *ust = TEST_MSC;
    *msc = TEST_MSC;
    return Success;
}

static int
queue_vblank(ScreenPtr screen, WindowPtr window, RRCrtcPtr crtc,
             uint64_t event_id, uint64_t msc)
{
    return Success;
}

void
present_pixmap_idle(present_vblank_ptr vblank)
{
    idles++;
}

int
dixDestroyPixmap(void *value, XID id)
{
    PixmapPtr pixmap = value;
    assert(pixmap->refcnt > 0);
    pixmap->refcnt--;
    destroyed++;
    return Success;
}

void
present_fence_destroy(present_fence_ptr fence)
{
    assert(!fence);
}

int
present_fake_queue_vblank(ScreenPtr screen, uint64_t event_id, uint64_t msc)
{
    return Success;
}

RRCrtcPtr
present_get_crtc(WindowPtr window)
{
    return NULL;
}

Bool
RRCrtcExists(ScreenPtr screen, RRCrtcPtr crtc)
{
    return FALSE;
}

uint64_t
present_get_target_msc(uint64_t target, uint64_t current, uint64_t divisor,
                       uint64_t remainder, uint32_t options)
{
    assert(target == TEST_TARGET_MSC && current == TEST_MSC && !divisor && !remainder);
    return target;
}

present_fence_ptr
present_fence_create(SyncFence *fence)
{
    unexpected_calls++;
    return NULL;
}

void
present_destroy_notifies(present_notify_ptr notifies, int count)
{
    assert(!notifies && !count);
}

Bool
SetNotifyFd(int fd, NotifyFdProcPtr notify, int mask, void *data)
{
    return TRUE;
}

void
present_send_complete_notify(WindowPtr window, CARD8 kind, CARD8 mode,
                             CARD32 serial, uint64_t ust, uint64_t msc)
{
    unexpected_calls++;
}

void
present_copy_region(DrawablePtr drawable, PixmapPtr pixmap, RegionPtr update,
                    int16_t x, int16_t y)
{
    unexpected_calls++;
}

void
present_set_tree_pixmap(WindowPtr window, PixmapPtr expected, PixmapPtr pixmap)
{
    unexpected_calls++;
}

void
DamageDamageRegion(DrawablePtr drawable, const RegionPtr region)
{
    unexpected_calls++;
}

void
CompositeServerPresented(WindowPtr window, PixmapPtr pixmap)
{
    unexpected_calls++;
}

Bool
present_execute_wait(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    unexpected_calls++;
    return TRUE;
}

void
present_execute_copy(present_vblank_ptr vblank, PixmapPtr pixmap, WindowPtr window,
                      uint64_t crtc_msc)
{
    unexpected_calls++;
}

void
present_execute_post(present_vblank_ptr vblank, PixmapPtr pixmap, WindowPtr window,
                      uint64_t ust, uint64_t crtc_msc)
{
    unexpected_calls++;
}

static Bool
reject_flip(RRCrtcPtr crtc, WindowPtr window, PixmapPtr pixmap, Bool sync,
            RegionPtr valid, int16_t x, int16_t y, PresentFlipReason *reason)
{
    return FALSE;
}

int
main(void)
{
    static const struct {
        Bool held, copied;
        uint32_t options;
        present_flip_type type;
    } cases[] = {
        { TRUE, FALSE, 0, PRESENT_TYPE_SYNCHRONOUS },
        { TRUE, FALSE, PresentOptionCopy | PresentOptionAsync, PRESENT_TYPE_ASYNCHRONOUS },
        { TRUE, FALSE, PresentOptionCopy | PresentOptionAsyncMayTear, PRESENT_TYPE_ASYNC_TEARING },
        { TRUE, FALSE, PresentOptionAsync, PRESENT_TYPE_ASYNCHRONOUS },
        { TRUE, FALSE, PresentOptionAsyncMayTear, PRESENT_TYPE_ASYNC_TEARING },
        { FALSE, FALSE, 0, PRESENT_TYPE_SYNCHRONOUS },
        { TRUE, TRUE, 0, PRESENT_TYPE_SYNCHRONOUS },
    };
    present_screen_priv_rec priv = { .queue_vblank = queue_vblank, .check_flip = reject_flip };
    present_screen_priv_ptr priv_ptr = &priv;
    ScreenRec screen = { .devPrivates = (void *) &priv_ptr };
    WindowRec window = { .drawable = { .pScreen = &screen } };
    RRCrtcRec previous_crtc = { 0 };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        PixmapRec old_pixmap = { .refcnt = 1 }, new_pixmap = { .refcnt = 1 };
        present_vblank_rec old = {
            .window = &window, .pixmap = &old_pixmap, .queued = TRUE,
            .throttled = cases[i].held, .crtc = &previous_crtc,
            .target_msc = TEST_MSC, .exec_msc = TEST_MSC, .rendering_fd = -1,
            .executed = cases[i].copied,
            .reason = cases[i].copied ? PRESENT_FLIP_REASON_DRIVER_TEARFREE :
                                       PRESENT_FLIP_REASON_UNKNOWN,
        };
        present_vblank_ptr latest;
        Bool replace = cases[i].held && !cases[i].copied;

        idles = destroyed = 0;
        xorg_list_init(&window_private.vblank);
        xorg_list_init(&window_private.notifies);
        xorg_list_init(&present_exec_queue);
        xorg_list_init(&present_flip_queue);
        window_private.crtc = NULL;
        xorg_list_append(&old.window_list, &window_private.vblank);
        xorg_list_append(&old.event_queue, &present_exec_queue);
        assert(present_scmd_pixmap(&window, &new_pixmap, TEST_SERIAL, NULL, NULL, 0, 0,
                                  NULL, NULL, NULL,
#ifdef DRI3
                                  NULL, NULL, 0, 0,
#endif
                                  cases[i].options, TEST_TARGET_MSC, 0, 0, NULL, 0) == Success);
        if (replace) {
            assert(!old.pixmap && old.mode == PresentCompleteModeSkip);
            assert(!old_pixmap.refcnt && idles == 1 && destroyed == 1);
        } else {
            assert(old.pixmap == &old_pixmap && old_pixmap.refcnt == 1);
            assert(!idles && !destroyed);
        }
        latest = xorg_list_last_entry(&window_private.vblank, present_vblank_rec, window_list);
        assert(latest != &old && latest->pixmap == &new_pixmap);
        assert(latest->mode == PresentCompleteModeCopy && latest->flip_type == cases[i].type);
        present_vblank_destroy(latest);
        assert(new_pixmap.refcnt == 1);
        if (old.pixmap)
            present_vblank_scrap(&old);
        xorg_list_del(&old.window_list);
        xorg_list_del(&old.event_queue);
        assert(xorg_list_is_empty(&window_private.vblank));
        assert(xorg_list_is_empty(&present_exec_queue));
    }
    assert(!unexpected_calls);
    return 0;
}
