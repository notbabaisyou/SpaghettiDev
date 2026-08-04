/* timethread.c -- background thread maintaining the single monotonic clock.
 *
 * The X server keeps one authoritative monotonic time value, updated by a
 * dedicated thread every TIME_TICK_MS milliseconds.  Readers fetch it with an
 * atomic load instead of hitting the clock on every call; this thread is the
 * only writer.
 *
 * Copyright © 2026 SpaghettiFork
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#ifdef WIN32
#include <windows.h>
#endif

#include "misc.h"
#include "osdep.h"

#define TIME_TICK_MS 1

#if TIMETHREAD

/* The tree is built with -std=gnu99, so use the GCC/Clang atomic builtins
 * rather than C11 _Atomic. */
#define TIME_LOAD(v) __atomic_load_n((v), __ATOMIC_RELAXED)
#define TIME_STORE(v, x) __atomic_store_n((v), (x), __ATOMIC_RELAXED)

static CARD32 now_ms;
static Bool time_thread_started;
static Bool time_thread_stop;
static pthread_t time_thread;

static void
tick_sleep(void)
{
#ifdef WIN32
    Sleep(TIME_TICK_MS);
#else
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = TIME_TICK_MS * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void *
time_thread_main(void *arg)
{
#if defined(HAVE_PTHREAD_SETNAME_NP_WITH_TID)
    pthread_setname_np(pthread_self(), "Timekeeping");
#elif defined(HAVE_PTHREAD_SETNAME_NP_WITHOUT_TID)
    pthread_setname_np("Timekeeping");
#endif

    while (!TIME_LOAD(&time_thread_stop)) {
        CARD32 now;
        CARD32 cached;

        tick_sleep();
        now = os_monotonic_millis();
        cached = TIME_LOAD(&now_ms);

        /* never let the exposed time run backwards */
        if ((int) (now - cached) > 0)
            TIME_STORE(&now_ms, now);
    }
    return NULL;
}

CARD32
TimeGetTime(void)
{
    if (TIME_LOAD(&time_thread_started))
        return TIME_LOAD(&now_ms);
    return os_monotonic_millis();
}

void
TimeThreadInit(void)
{
    pthread_attr_t attr;

    if (TIME_LOAD(&time_thread_started))
        return;

    /* Seed the cache so early readers never see a stale zero. */
    TIME_STORE(&now_ms, os_monotonic_millis());

    pthread_attr_init(&attr);
    if (pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) != 0)
        ErrorF("time-thread: error setting thread scope\n");
    if (pthread_create(&time_thread, &attr, &time_thread_main, NULL) != 0) {
        ErrorF("time-thread: failed to create thread\n");
        pthread_attr_destroy(&attr);
        return;
    }
    pthread_attr_destroy(&attr);

    TIME_STORE(&time_thread_started, 1);
}

void
TimeThreadFini(void)
{
    if (!TIME_LOAD(&time_thread_started))
        return;

    TIME_STORE(&time_thread_stop, 1);
    pthread_join(time_thread, NULL);
    TIME_STORE(&time_thread_stop, 0);
    TIME_STORE(&time_thread_started, 0);
}

#else /* TIMETHREAD */

CARD32
TimeGetTime(void)
{
    return os_monotonic_millis();
}

void
TimeThreadInit(void)
{
}

void
TimeThreadFini(void)
{
}

#endif /* TIMETHREAD */
