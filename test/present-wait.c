/* SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "dix-config.h"
#include <assert.h>
#include "../present/present_execute.c"

enum { TEST_CLIENT = 1, TEST_PIXMAP = 1, TEST_NOW = 2000000, TEST_INTERVAL = 1000000, TEST_CLIENT_BITS = 8 };

unsigned int
ResourceClientBits(void)
{
    return TEST_CLIENT_BITS;
}
DevPrivateKeyRec present_screen_private_key = { .initialized = TRUE };
DevPrivateKeyRec present_client_private_key = { .initialized = TRUE, .size = sizeof(uint64_t) };
ClientPtr clients[MAXCLIENTS];
ClientPtr serverClient;
static unsigned copies, idles, rendering_queries, queued, fence_callbacks;
static Bool hidden, fence_signaled = TRUE;

#if FTRACE
void ftrace_print(const char *format, ...) {}
void ftrace_print_begin(unsigned long context, const char *format, ...) {}
void ftrace_print_end(unsigned long context, const char *format, ...) {}
#endif

CARD64
GetTimeInMicros(void)
{
    return TEST_NOW;
}

Bool
CompositeServerWindowHidden(WindowPtr window)
{
    return hidden;
}

void
CompositeServerPresented(WindowPtr window, PixmapPtr pixmap)
{
}

Bool
present_fence_check_triggered(present_fence_ptr fence)
{
    return fence_signaled;
}

void
present_fence_set_callback(present_fence_ptr fence, void (*callback)(void *), void *data)
{
    fence_callbacks++;
}

Bool
SetNotifyFd(int fd, NotifyFdProcPtr notify, int mask, void *data)
{
    return TRUE;
}

void
present_copy_region(DrawablePtr drawable, PixmapPtr pixmap, RegionPtr update,
                    int16_t x, int16_t y)
{
    copies++;
}

void
present_pixmap_idle(present_vblank_ptr vblank)
{
    idles++;
}

static void
flush(WindowPtr window)
{
}

static int
rendering_fd(WindowPtr window, PixmapPtr pixmap)
{
    rendering_queries++;
    return -1;
}

static int
queue_vblank(ScreenPtr screen, WindowPtr window, RRCrtcPtr crtc,
             uint64_t event_id, uint64_t msc)
{
    queued++;
    return Success;
}

int
main(void)
{
    present_screen_info_rec info = {
        .version = PRESENT_SCREEN_INFO_VERSION_WINDOW_FLIP,
        .pixmap_rendering_fd = rendering_fd,
    };
    present_screen_priv_rec priv = {
        .fake_interval = TEST_INTERVAL, .info = &info,
        .flush = flush, .queue_vblank = queue_vblank,
    };
    present_screen_priv_ptr priv_ptr = &priv;
    ScreenRec screen = { .devPrivates = (void *) &priv_ptr };
    WindowRec window = { .drawable = { .pScreen = &screen }, .viewable = TRUE };
    PixmapRec pixmap = { .drawable = { .id = (TEST_CLIENT << CLIENTOFFSET) | TEST_PIXMAP } };
    uint64_t client_times[2] = { 0 };
    ClientRec client = { .devPrivates = (void *) client_times };
    present_vblank_rec vblank = {
        .screen = &screen, .window = &window, .pixmap = &pixmap, .rendering_fd = -1,
    };

    clients[TEST_CLIENT] = &client;
    present_execute_copy(&vblank, &pixmap, &window, 0);
    assert(vblank.executed && copies == 1 && idles == 1);

    /* Completion must bypass both hidden pacing and a reused pixmap's fence. */
    hidden = TRUE;
    assert(!present_execute_wait(&vblank, 0));
    assert(!queued && !rendering_queries);
    hidden = FALSE;
    assert(!present_execute_wait(&vblank, 0));
    assert(!queued && !rendering_queries);

    /* Existing acquire/wait fences still run before the executed shortcut. */
    vblank.wait_fence = (void *) &client;
    fence_signaled = FALSE;
    assert(present_execute_wait(&vblank, 0));
    assert(fence_callbacks == 1 && !queued && !rendering_queries);
    vblank.wait_fence = NULL;
    fence_signaled = TRUE;

    /* A fresh frame must still wait when its window is hidden. */
    vblank.executed = FALSE;
    hidden = TRUE;
    assert(present_execute_wait(&vblank, 0));
    assert(queued == 1 && !rendering_queries);
    return 0;
}
