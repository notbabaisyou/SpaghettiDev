#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <string.h>
#include <stdlib.h>

#include <X11/Xproto.h>

#include "misc.h"
#include "os.h"
#include "dixstruct.h"
#include "windowstr.h"
#include "globals.h"
#include "extnsionst.h"
#include "extinit.h"
#include "dix.h"

#include "vrr.h"

/* Forward declaration for the ExtensionModule struct below. */
static void VRRExtensionInit(void);

/* Owned here once VRRExtensionInit() runs. */
static Atom vrr_atom = None;

/*
 * ProcVector wrapping.
 *
 * Installed once in VRRExtensionInit() and never removed; the wrappers are
 * harmless when no screens are registered (vrr_notify_window is a no-op).
 *
 * restore_property_vector is set when we detect that another layer has
 * replaced our wrapper in the global ProcVector; per-client vectors are still
 * cleaned up on a best-effort basis.
 */
static int (*saved_change_property)(ClientPtr client);
static int (*saved_delete_property)(ClientPtr client);
static Bool restore_property_vector = FALSE;

typedef struct {
    ScreenPtr              screen;
    VRRPropertyChangedProc callback;
    void                  *driver_private;
} VrrScreenRec;

static VrrScreenRec *vrr_screens     = NULL;
static int           vrr_num_screens = 0;

static VrrScreenRec *
vrr_find_screen(ScreenPtr screen)
{
    int i;

    for (i = 0; i < vrr_num_screens; i++)
        if (vrr_screens[i].screen == screen)
            return &vrr_screens[i];

    return NULL;
}

/*
 * vrr_notify_window - dispatch to the driver owning the window's screen.
 *
 * Screens that have not called VrrExtInit() are silently ignored, making the
 * extension safe on multi-head servers where only some outputs are VRR-capable.
 */
static void
vrr_notify_window(WindowPtr window, Bool variable_refresh)
{
    VrrScreenRec *rec = vrr_find_screen(window->drawable.pScreen);

    if (rec && rec->callback)
        rec->callback(window, variable_refresh, rec->driver_private);
}

/* --------------------------------------------------------------------- */
/* ProcVector wrappers                                                     */
/* --------------------------------------------------------------------- */

/*
 * vrr_change_property - wrapper for DIX ProcChangeProperty.
 *
 * Calls through to the real handler first so the property is committed before
 * we inspect it.  If the changed property is _VARIABLE_REFRESH we fire the
 * per-screen driver callback with the new boolean value.
 */
static int
vrr_change_property(ClientPtr client)
{
    WindowPtr window = NULL;
    int       ret;

    REQUEST(xChangePropertyReq);

    /*
     * Temporarily restore the saved handler on this client's vector so that
     * recursive invocations don't re-enter us.
     */
    client->requestVector[X_ChangeProperty] = saved_change_property;
    ret = saved_change_property(client);

    /* Honour any foreign wrapper that replaced us in the global ProcVector. */
    if (restore_property_vector)
        return ret;

    /* Re-arm for future requests from this client. */
    client->requestVector[X_ChangeProperty] = vrr_change_property;

    if (ret != Success)
        return ret;

    ret = dixLookupWindow(&window, stuff->window, client, DixSetPropAccess);
    if (ret != Success)
        return ret;

    if (stuff->property == vrr_atom &&
        stuff->format == 32 && stuff->nUnits == 1) {
        uint32_t *value = (uint32_t *)(stuff + 1);
        vrr_notify_window(window, *value != 0);
    }

    return ret;
}

/*
 * vrr_delete_property - wrapper for DIX ProcDeleteProperty.
 *
 * Deletion of _VARIABLE_REFRESH is treated as variable_refresh = FALSE.
 */
static int
vrr_delete_property(ClientPtr client)
{
    WindowPtr window;
    int       ret;

    REQUEST(xDeletePropertyReq);

    client->requestVector[X_DeleteProperty] = saved_delete_property;
    ret = saved_delete_property(client);

    if (restore_property_vector)
        return ret;

    client->requestVector[X_DeleteProperty] = vrr_delete_property;

    if (ret != Success)
        return ret;

    ret = dixLookupWindow(&window, stuff->window, client, DixSetPropAccess);
    if (ret != Success)
        return ret;

    if (stuff->property == vrr_atom)
        vrr_notify_window(window, FALSE);

    return ret;
}

/*
 * VRRExtensionInit - called once by the module loader during server startup.
 *
 * Registers the extension name with the server, creates the _VARIABLE_REFRESH
 * atom, and installs ProcVector wrappers.  All three remain for the server
 * lifetime; per-screen DDX registration happens later via VrrExtInit().
 */
static void
VRRExtensionInit(void)
{
    if (!AddExtension(VRR_EXTENSION_NAME,
                      /* NumEvents  = */ 0,
                      /* NumErrors  = */ 0,
                      /* mainProc   = */ NULL,
                      /* swapProc   = */ NULL,
                      /* closeDownProc = */ NULL,
                      /* minorOpcodeProc = */ StandardMinorOpcode)) {
        ErrorF("vrr: AddExtension failed\n");
        return;
    }

    vrr_atom = MakeAtom("_VARIABLE_REFRESH",
                        sizeof("_VARIABLE_REFRESH") - 1, TRUE);
    if (vrr_atom == BAD_RESOURCE) {
        ErrorF("vrr: MakeAtom failed\n");
        return;
    }

    saved_change_property        = ProcVector[X_ChangeProperty];
    ProcVector[X_ChangeProperty] = vrr_change_property;

    saved_delete_property        = ProcVector[X_DeleteProperty];
    ProcVector[X_DeleteProperty] = vrr_delete_property;
}

/**
 * VRRExtInit - Register a DDX screen for _VARIABLE_REFRESH notifications.
 *
 * @screen:         Screen to watch; must not already be registered.
 * @callback:       Invoked on every _VARIABLE_REFRESH property change.
 * @driver_private: Opaque value forwarded to @callback unchanged.
 *
 * Call from the driver's ScreenInit after verifying !noVrrExtension and after
 * dixRegisterPrivateKey for any per-window data the callback will access.
 * The atom and ProcVector wrappers are already in place at this point.
 *
 * Returns TRUE on success, FALSE on duplicate registration or OOM.
 */
Bool
VRRExtInit(ScreenPtr screen, VRRPropertyChangedProc callback,
           void *driver_private)
{
    VrrScreenRec *new_screens;
    VrrScreenRec *rec;

    if (vrr_find_screen(screen)) {
        ErrorF("vrr: VRRExtInit called twice for the same screen\n");
        return FALSE;
    }

    new_screens = realloc(vrr_screens,
                         (vrr_num_screens + 1) * sizeof(VrrScreenRec));
    if (!new_screens)
        return FALSE;

    vrr_screens            = new_screens;
    rec                    = &vrr_screens[vrr_num_screens++];
    rec->screen            = screen;
    rec->callback          = callback;
    rec->driver_private    = driver_private;

    return TRUE;
}

/**
 * VRRExtFini - Unregister a DDX screen from the VRR extension.
 *
 * Removes the per-screen callback entry.  The global ProcVector wrappers
 * installed by VRRExtensionInit() remain active; they are harmless when the
 * registration table is empty.  Safe to call even if VrrExtInit() was never
 * called for @screen.
 */
void
VRRExtFini(ScreenPtr screen)
{
    int i;

    for (i = 0; i < vrr_num_screens; i++) {
        if (vrr_screens[i].screen != screen)
            continue;

        memmove(&vrr_screens[i], &vrr_screens[i + 1],
                (vrr_num_screens - i - 1) * sizeof(VrrScreenRec));
        vrr_num_screens--;
        break;
    }

    if (vrr_num_screens == 0) {
        free(vrr_screens);
        vrr_screens = NULL;
    }
}

/**
 * VRRGetAtom - Return the _VARIABLE_REFRESH Atom, or None.
 */
Atom
VRRGetAtom(void)
{
    return vrr_atom;
}