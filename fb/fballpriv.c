/*
 * Copyright © 1998 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "fb.h"

static DevPrivateKeyRec fbScreenPrivateKeyRec;

#ifndef FB_ACCESS_WRAPPER
DevPrivateKeyRec fbPixmapAccessKey;
DevPrivateKeyRec fbPixmapCpuAccessKey;

static FbPixmapCpuAccessRec *
fbGetPixmapCpuAccess(ScreenPtr screen)
{
    if (!dixRegisterPrivateKey(&fbPixmapCpuAccessKey, PRIVATE_SCREEN,
                               sizeof(FbPixmapCpuAccessRec)))
        return NULL;

    return dixGetPrivateAddr(&screen->devPrivates, &fbPixmapCpuAccessKey);
}

Bool
fbSetPixmapCpuAccessProc(ScreenPtr screen, FbPixmapCpuAccessProcPtr proc)
{
    FbPixmapCpuAccessRec *access = fbGetPixmapCpuAccess(screen);

    if (!access)
        return FALSE;
    access->access = proc;
    access->checked = NULL;
    return TRUE;
}

Bool
fbSetPixmapCheckedAccessProc(ScreenPtr screen,
                             FbPixmapCheckedAccessProcPtr proc)
{
    FbPixmapCpuAccessRec *access = fbGetPixmapCpuAccess(screen);

    if (!access)
        return FALSE;
    access->checked = proc;
    access->access = NULL;
    return TRUE;
}

uint64_t
fbPixmapAccessFailures(ScreenPtr screen)
{
    if (!dixPrivateKeyRegistered(&fbPixmapCpuAccessKey))
        return 0;

    FbPixmapCpuAccessRec *access =
        dixGetPrivateAddr(&screen->devPrivates, &fbPixmapCpuAccessKey);

    return access->failures;
}

Bool
fbSetPixmapAccessProc(ScreenPtr screen, FbPixmapAccessProcPtr proc)
{
    if (!dixRegisterPrivateKey(&fbPixmapAccessKey, PRIVATE_SCREEN,
                               sizeof(FbPixmapAccessProcPtr)))
        return FALSE;

    FbPixmapAccessProcPtr *access =
        dixGetPrivateAddr(&screen->devPrivates, &fbPixmapAccessKey);

    *access = proc;
    return TRUE;
}
#endif

DevPrivateKey
fbGetScreenPrivateKey(void)
{
    return &fbScreenPrivateKeyRec;
}

#if LEGACY_ABI_COMPAT
DevPrivateKey
fbGetGCPrivateKey(GCPtr pGC)
{
    return &fbGetScreenPrivate((pGC)->pScreen)->gcPrivateKeyRec;
}
#endif

Bool
fbAllocatePrivates(ScreenPtr pScreen)
{
    FbScreenPrivPtr     pScrPriv;

    if (!dixRegisterPrivateKey
        (&fbScreenPrivateKeyRec, PRIVATE_SCREEN, sizeof(FbScreenPrivRec)))
        return FALSE;

    pScrPriv = fbGetScreenPrivate(pScreen);

    if (!dixRegisterScreenSpecificPrivateKey (pScreen, &pScrPriv->gcPrivateKeyRec, PRIVATE_GC, sizeof(FbGCPrivRec)))
        return FALSE;
    if (!dixRegisterScreenSpecificPrivateKey (pScreen, &pScrPriv->winPrivateKeyRec, PRIVATE_WINDOW, 0))
        return FALSE;

    return TRUE;
}

#ifdef FB_ACCESS_WRAPPER
ReadMemoryProcPtr wfbReadMemory;
WriteMemoryProcPtr wfbWriteMemory;
#endif
