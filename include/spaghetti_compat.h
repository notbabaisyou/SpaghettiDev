/**
 * Spaghetti Display Server
 * Copyright (C) 2025  SpaghettiFork
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
#ifndef _SPAGHETTI_COMPAT_NV_
#define _SPAGHETTI_COMPAT_NV_

#define ASSIGN_MEMBER(dest, src, member) \
    (dest).member = (src).member

#include <scrnintstr.h>

typedef struct _NVScreen {
    int myNum;                  /* index of this instance in Screens[] */
    ATOM id;
    short x, y, width, height;
    short mmWidth, mmHeight;
    short numDepths;
    unsigned char rootDepth;
    DepthPtr allowedDepths;
    unsigned long rootVisual;
    unsigned long defColormap;
    short minInstalledCmaps, maxInstalledCmaps;
    char backingStoreSupport, saveUnderSupport;
    unsigned long whitePixel, blackPixel;
    GCPtr GCperDepth[MAXFORMATS + 1];
    /* next field is a stipple to use as default in a GC.  we don't build
     * default tiles of all depths because they are likely to be of a color
     * different from the default fg pixel, so we don't win anything by
     * building a standard one.
     */
    PixmapPtr defaultStipple;
    void *devPrivate;
    short numVisuals;
    VisualPtr visuals;
    WindowPtr root;
    ScreenSaverStuffRec screensaver;

    DevPrivateSetRec    screenSpecificPrivates[PRIVATE_LAST];

    /* Random screen procedures */

    CloseScreenProcPtr CloseScreen;
    QueryBestSizeProcPtr QueryBestSize;
    SaveScreenProcPtr SaveScreen;
    GetImageProcPtr GetImage;
    GetSpansProcPtr GetSpans;
    SourceValidateProcPtr SourceValidate;

    /* Window Procedures */

    CreateWindowProcPtr CreateWindow;
    DestroyWindowProcPtr DestroyWindow;
    PositionWindowProcPtr PositionWindow;
    ChangeWindowAttributesProcPtr ChangeWindowAttributes;
    RealizeWindowProcPtr RealizeWindow;
    UnrealizeWindowProcPtr UnrealizeWindow;
    ValidateTreeProcPtr ValidateTree;
    PostValidateTreeProcPtr PostValidateTree;
    WindowExposuresProcPtr WindowExposures;
    CopyWindowProcPtr CopyWindow;
    ClearToBackgroundProcPtr ClearToBackground;
    ClipNotifyProcPtr ClipNotify;
    RestackWindowProcPtr RestackWindow;
    PaintWindowProcPtr PaintWindow;

    /* Pixmap procedures */

    CreatePixmapProcPtr CreatePixmap;
    DestroyPixmapProcPtr DestroyPixmap;

    /* Font procedures */

    RealizeFontProcPtr RealizeFont;
    UnrealizeFontProcPtr UnrealizeFont;

    /* Cursor Procedures */

    ConstrainCursorProcPtr ConstrainCursor;
    ConstrainCursorHarderProcPtr ConstrainCursorHarder;
    CursorLimitsProcPtr CursorLimits;
    DisplayCursorProcPtr DisplayCursor;
    RealizeCursorProcPtr RealizeCursor;
    UnrealizeCursorProcPtr UnrealizeCursor;
    RecolorCursorProcPtr RecolorCursor;
    SetCursorPositionProcPtr SetCursorPosition;
    CursorWarpedToProcPtr CursorWarpedTo;
    CurserConfinedToProcPtr CursorConfinedTo;

    /* GC procedures */

    CreateGCProcPtr CreateGC;

    /* Colormap procedures */

    CreateColormapProcPtr CreateColormap;
    DestroyColormapProcPtr DestroyColormap;
    InstallColormapProcPtr InstallColormap;
    UninstallColormapProcPtr UninstallColormap;
    ListInstalledColormapsProcPtr ListInstalledColormaps;
    StoreColorsProcPtr StoreColors;
    ResolveColorProcPtr ResolveColor;

    /* Region procedures */

    BitmapToRegionProcPtr BitmapToRegion;

    /* os layer procedures */

    ScreenBlockHandlerProcPtr BlockHandler;
    ScreenWakeupHandlerProcPtr WakeupHandler;

    /* anybody can get a piece of this array */
    PrivateRec *devPrivates;

    CreateScreenResourcesProcPtr CreateScreenResources;
    ModifyPixmapHeaderProcPtr ModifyPixmapHeader;

    GetWindowPixmapProcPtr GetWindowPixmap;
    SetWindowPixmapProcPtr SetWindowPixmap;
    GetScreenPixmapProcPtr GetScreenPixmap;
    SetScreenPixmapProcPtr SetScreenPixmap;
    NameWindowPixmapProcPtr NameWindowPixmap;

	_X_UNUSED void* __NVIDIA_WANTS_THIS_SPACE__;

    unsigned int totalPixmapSize;

    MarkWindowProcPtr MarkWindow;
    MarkOverlappedWindowsProcPtr MarkOverlappedWindows;
    ConfigNotifyProcPtr ConfigNotify;
    MoveWindowProcPtr MoveWindow;
    ResizeWindowProcPtr ResizeWindow;
    GetLayerWindowProcPtr GetLayerWindow;
    HandleExposuresProcPtr HandleExposures;
    ReparentWindowProcPtr ReparentWindow;

    SetShapeProcPtr SetShape;

    ChangeBorderWidthProcPtr ChangeBorderWidth;
    MarkUnrealizedWindowProcPtr MarkUnrealizedWindow;

    /* Device cursor procedures */
    DeviceCursorInitializeProcPtr DeviceCursorInitialize;
    DeviceCursorCleanupProcPtr DeviceCursorCleanup;

    /* set it in driver side if X server can copy the framebuffer content.
     * Meant to be used together with '-background none' option, avoiding
     * malicious users to steal framebuffer's content if that would be the
     * default */
    Bool canDoBGNoneRoot;

    Bool isGPU;

    /* Info on this screen's secondarys (if any) */
    struct xorg_list secondary_list;
    struct xorg_list secondary_head;
    int output_secondarys;
    /* Info for when this screen is a secondary */
    ScreenPtr current_primary;
    Bool is_output_secondary;
    Bool is_offload_secondary;

    SharePixmapBackingProcPtr SharePixmapBacking;
    SetSharedPixmapBackingProcPtr SetSharedPixmapBacking;

    StartPixmapTrackingProcPtr StartPixmapTracking;
    StopPixmapTrackingProcPtr StopPixmapTracking;
    SyncSharedPixmapProcPtr SyncSharedPixmap;

    SharedPixmapNotifyDamageProcPtr SharedPixmapNotifyDamage;
    RequestSharedPixmapNotifyDamageProcPtr RequestSharedPixmapNotifyDamage;
    PresentSharedPixmapProcPtr PresentSharedPixmap;
    StopFlippingPixmapTrackingProcPtr StopFlippingPixmapTracking;

    struct xorg_list pixmap_dirty_list;

    ReplaceScanoutPixmapProcPtr ReplaceScanoutPixmap;
    XYToWindowProcPtr XYToWindow;
    DPMSProcPtr DPMS;
} NVScreenRec;

/**
 * This is a really ugly ABI compatibility hack
 * that I will personally go to the deepest layer
 * of hell for.
 * 
 * God help me.
 */

static Bool
NVScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    NVScreenRec* compatScreen;

    compatScreen = calloc(1, sizeof(NVScreenRec));
    if (!compatScreen)
        return FALSE;

    if (!pScrn->ScreenInit (compatScreen, argc, argv))
        return FALSE;

    ASSIGN_MEMBER(*pScreen, *compatScreen, myNum);
    ASSIGN_MEMBER(*pScreen, *compatScreen, id);
    ASSIGN_MEMBER(*pScreen, *compatScreen, x);
    ASSIGN_MEMBER(*pScreen, *compatScreen, y);
    ASSIGN_MEMBER(*pScreen, *compatScreen, width);
    ASSIGN_MEMBER(*pScreen, *compatScreen, height);
    ASSIGN_MEMBER(*pScreen, *compatScreen, mmWidth);
    ASSIGN_MEMBER(*pScreen, *compatScreen, mmHeight);
    ASSIGN_MEMBER(*pScreen, *compatScreen, numDepths);
    ASSIGN_MEMBER(*pScreen, *compatScreen, rootDepth);
    ASSIGN_MEMBER(*pScreen, *compatScreen, allowedDepths);
    ASSIGN_MEMBER(*pScreen, *compatScreen, rootVisual);
    ASSIGN_MEMBER(*pScreen, *compatScreen, defColormap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, minInstalledCmaps);
    ASSIGN_MEMBER(*pScreen, *compatScreen, maxInstalledCmaps);
    ASSIGN_MEMBER(*pScreen, *compatScreen, backingStoreSupport);
    ASSIGN_MEMBER(*pScreen, *compatScreen, saveUnderSupport);
    ASSIGN_MEMBER(*pScreen, *compatScreen, whitePixel);
    ASSIGN_MEMBER(*pScreen, *compatScreen, blackPixel);

    memcpy(pScreen->GCperDepth, compatScreen->GCperDepth, sizeof(compatScreen->GCperDepth));

    ASSIGN_MEMBER(*pScreen, *compatScreen, defaultStipple);
    ASSIGN_MEMBER(*pScreen, *compatScreen, devPrivate);
    ASSIGN_MEMBER(*pScreen, *compatScreen, numVisuals);
    ASSIGN_MEMBER(*pScreen, *compatScreen, visuals);
    ASSIGN_MEMBER(*pScreen, *compatScreen, root);
    ASSIGN_MEMBER(*pScreen, *compatScreen, screensaver);

    memcpy(pScreen->screenSpecificPrivates, compatScreen->screenSpecificPrivates, sizeof(compatScreen->screenSpecificPrivates));

    ASSIGN_MEMBER(*pScreen, *compatScreen, CloseScreen);
    ASSIGN_MEMBER(*pScreen, *compatScreen, QueryBestSize);
    ASSIGN_MEMBER(*pScreen, *compatScreen, SaveScreen);
    ASSIGN_MEMBER(*pScreen, *compatScreen, GetImage);
    ASSIGN_MEMBER(*pScreen, *compatScreen, GetSpans);
    ASSIGN_MEMBER(*pScreen, *compatScreen, SourceValidate);

    ASSIGN_MEMBER(*pScreen, *compatScreen, CreateWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, DestroyWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, PositionWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ChangeWindowAttributes);
    ASSIGN_MEMBER(*pScreen, *compatScreen, RealizeWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, UnrealizeWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ValidateTree);
    ASSIGN_MEMBER(*pScreen, *compatScreen, PostValidateTree);
    ASSIGN_MEMBER(*pScreen, *compatScreen, WindowExposures);
    ASSIGN_MEMBER(*pScreen, *compatScreen, CopyWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ClearToBackground);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ClipNotify);
    ASSIGN_MEMBER(*pScreen, *compatScreen, RestackWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, PaintWindow);

    ASSIGN_MEMBER(*pScreen, *compatScreen, CreatePixmap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, DestroyPixmap);

    ASSIGN_MEMBER(*pScreen, *compatScreen, RealizeFont);
    ASSIGN_MEMBER(*pScreen, *compatScreen, UnrealizeFont);

    ASSIGN_MEMBER(*pScreen, *compatScreen, ConstrainCursor);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ConstrainCursorHarder);
    ASSIGN_MEMBER(*pScreen, *compatScreen, CursorLimits);
    ASSIGN_MEMBER(*pScreen, *compatScreen, DisplayCursor);
    ASSIGN_MEMBER(*pScreen, *compatScreen, RealizeCursor);
    ASSIGN_MEMBER(*pScreen, *compatScreen, UnrealizeCursor);
    ASSIGN_MEMBER(*pScreen, *compatScreen, RecolorCursor);
    ASSIGN_MEMBER(*pScreen, *compatScreen, SetCursorPosition);
    ASSIGN_MEMBER(*pScreen, *compatScreen, CursorWarpedTo);
    ASSIGN_MEMBER(*pScreen, *compatScreen, CursorConfinedTo);

    ASSIGN_MEMBER(*pScreen, *compatScreen, CreateGC);

    ASSIGN_MEMBER(*pScreen, *compatScreen, CreateColormap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, DestroyColormap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, InstallColormap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, UninstallColormap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ListInstalledColormaps);
    ASSIGN_MEMBER(*pScreen, *compatScreen, StoreColors);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ResolveColor);

    ASSIGN_MEMBER(*pScreen, *compatScreen, BitmapToRegion);

    ASSIGN_MEMBER(*pScreen, *compatScreen, BlockHandler);
    ASSIGN_MEMBER(*pScreen, *compatScreen, WakeupHandler);

    ASSIGN_MEMBER(*pScreen, *compatScreen, devPrivates);

    ASSIGN_MEMBER(*pScreen, *compatScreen, CreateScreenResources);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ModifyPixmapHeader);

    ASSIGN_MEMBER(*pScreen, *compatScreen, GetWindowPixmap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, SetWindowPixmap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, GetScreenPixmap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, SetScreenPixmap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, NameWindowPixmap);

    ASSIGN_MEMBER(*pScreen, *compatScreen, totalPixmapSize);

    ASSIGN_MEMBER(*pScreen, *compatScreen, MarkWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, MarkOverlappedWindows);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ConfigNotify);
    ASSIGN_MEMBER(*pScreen, *compatScreen, MoveWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ResizeWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, GetLayerWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, HandleExposures);
    ASSIGN_MEMBER(*pScreen, *compatScreen, ReparentWindow);

    ASSIGN_MEMBER(*pScreen, *compatScreen, SetShape);

    ASSIGN_MEMBER(*pScreen, *compatScreen, ChangeBorderWidth);
    ASSIGN_MEMBER(*pScreen, *compatScreen, MarkUnrealizedWindow);

    ASSIGN_MEMBER(*pScreen, *compatScreen, DeviceCursorInitialize);
    ASSIGN_MEMBER(*pScreen, *compatScreen, DeviceCursorCleanup);

    ASSIGN_MEMBER(*pScreen, *compatScreen, canDoBGNoneRoot);
    ASSIGN_MEMBER(*pScreen, *compatScreen, isGPU);

    ASSIGN_MEMBER(*pScreen, *compatScreen, secondary_list);
    ASSIGN_MEMBER(*pScreen, *compatScreen, secondary_head);
    ASSIGN_MEMBER(*pScreen, *compatScreen, output_secondarys);
    ASSIGN_MEMBER(*pScreen, *compatScreen, current_primary);
    ASSIGN_MEMBER(*pScreen, *compatScreen, is_output_secondary);
    ASSIGN_MEMBER(*pScreen, *compatScreen, is_offload_secondary);

    ASSIGN_MEMBER(*pScreen, *compatScreen, SharePixmapBacking);
    ASSIGN_MEMBER(*pScreen, *compatScreen, SetSharedPixmapBacking);

    ASSIGN_MEMBER(*pScreen, *compatScreen, StartPixmapTracking);
    ASSIGN_MEMBER(*pScreen, *compatScreen, StopPixmapTracking);
    ASSIGN_MEMBER(*pScreen, *compatScreen, SyncSharedPixmap);

    ASSIGN_MEMBER(*pScreen, *compatScreen, SharedPixmapNotifyDamage);
    ASSIGN_MEMBER(*pScreen, *compatScreen, RequestSharedPixmapNotifyDamage);
    ASSIGN_MEMBER(*pScreen, *compatScreen, PresentSharedPixmap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, StopFlippingPixmapTracking);

    ASSIGN_MEMBER(*pScreen, *compatScreen, pixmap_dirty_list);

    ASSIGN_MEMBER(*pScreen, *compatScreen, ReplaceScanoutPixmap);
    ASSIGN_MEMBER(*pScreen, *compatScreen, XYToWindow);
    ASSIGN_MEMBER(*pScreen, *compatScreen, DPMS);

    return TRUE;
}

#undef ASSIGN_MEMBER
#endif // _SPAGHETTI_COMPAT_NV_