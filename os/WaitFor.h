/**
 * Spaghetti Display Server
 * Copyright (C) 2026  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#ifdef WIN32
#include <X11/Xwinsock.h>
#endif
#include <X11/Xos.h>            /* for strings, fcntl, time */
#include <errno.h>
#include <stdio.h>
#include <X11/X.h>

#include "os/busfault.h"

#include "misc.h"

#include "osdep.h"
#include "dixstruct.h"
#include "opaque.h"
#ifdef DPMSExtension
#include "dpmsproc.h"
#endif

#ifdef WIN32
/* Error codes from windows sockets differ from fileio error codes  */
#undef EINTR
#define EINTR WSAEINTR
#undef EINVAL
#define EINVAL WSAEINVAL
#undef EBADF
#define EBADF WSAENOTSOCK
/* Windows select does not set errno. Use GetErrno as wrapper for
   WSAGetLastError */
#define GetErrno WSAGetLastError
#else
/* This is just a fallback to errno to hide the differences between unix and
   Windows in the code */
#define GetErrno() errno
#endif

#ifdef DPMSExtension
#include <X11/extensions/dpmsconst.h>
#endif

struct _OsTimerRec {
    struct xorg_list list;
    CARD32 expires;
    CARD32 delta;
    OsTimerCallback callback;
    void *arg;
};

static OsTimerPtr *heap      = NULL;
static int         heap_size = 0;
static int         heap_cap  = 0;

static inline int
timer_heap_index(OsTimerPtr t)
{
    return (int)(intptr_t)t->list.next;
}

static inline void
timer_set_heap_index(OsTimerPtr t, int i)
{
    t->list.next = (struct xorg_list *)(intptr_t)i;
}

static inline Bool
timer_pending(OsTimerPtr t)
{
    return timer_heap_index(t) >= 0;
}

static inline void
heap_set(int i, OsTimerPtr t)
{
    heap[i] = t;
    timer_set_heap_index(t, i);
}

static void
sift_up(int i)
{
    while (i > 0) {
        int parent = (i - 1) / 2;
        if ((int)(heap[parent]->expires - heap[i]->expires) <= 0)
            break;
        OsTimerPtr tmp = heap[parent];
        heap_set(parent, heap[i]);
        heap_set(i, tmp);
        i = parent;
    }
}

static void
sift_down(int i)
{
    for (;;) {
        int smallest = i;
        int l = 2 * i + 1, r = 2 * i + 2;
        if (l < heap_size &&
            (int)(heap[l]->expires - heap[smallest]->expires) < 0)
            smallest = l;
        if (r < heap_size &&
            (int)(heap[r]->expires - heap[smallest]->expires) < 0)
            smallest = r;
        if (smallest == i)
            break;
        OsTimerPtr tmp = heap[i];
        heap_set(i, heap[smallest]);
        heap_set(smallest, tmp);
        i = smallest;
    }
}

static void
heap_remove_at(int i)
{
    timer_set_heap_index(heap[i], -1);
    heap_size--;
    if (i == heap_size)
        return;
    heap_set(i, heap[heap_size]);
    sift_up(i);
    sift_down(i);
}

static Bool
heap_insert(OsTimerPtr timer)
{
    if (heap_size == heap_cap) {
        int newcap = heap_cap ? heap_cap * 2 : 16;
        OsTimerPtr *tmp = reallocarray(heap, newcap, sizeof(*heap));
        if (!tmp)
            return FALSE;
        heap = tmp;
        heap_cap = newcap;
    }
    heap_set(heap_size++, timer);
    sift_up(timer_heap_index(timer));
    return TRUE;
}

static inline OsTimerPtr
first_timer(void)
{
    return heap_size ? heap[0] : NULL;
}