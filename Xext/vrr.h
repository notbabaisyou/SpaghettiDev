#ifndef _VRR_H_
#define _VRR_H_
 
#include "window.h"
#include "scrnintstr.h"

#define VRR_EXTENSION_NAME "SPAGHETTI-VRR"
 
/*
 * VRRPropertyChangedProc - callback invoked by the VRR extension when the
 * _VARIABLE_REFRESH property on a window is created, updated, or deleted.
 *
 * @window:         The window whose property changed.
 * @variable_refresh: TRUE if the property was set to a non-zero value;
 *                    FALSE if it was cleared or deleted.
 * @driver_private: The opaque pointer supplied to VrrExtInit().
 */
typedef void (*VRRPropertyChangedProc)(WindowPtr window,
                                       Bool variable_refresh,
                                       void *driver_private);
 
/*
 * VRRExtInit - Register a screen with the VRR extension.
 *
 * Sets up ProcVector wrappers for ChangeProperty / DeleteProperty (once,
 * shared across all registered screens) and ensures the _VARIABLE_REFRESH
 * atom exists.  Call from the driver's ScreenInit.
 *
 * Returns FALSE on allocation failure or if @screen is already registered.
 */
extern _X_EXPORT Bool VRRExtInit(ScreenPtr screen,
                                 VRRPropertyChangedProc callback,
                                 void *driver_private);
 
/*
 * VRRExtFini - Unregister a screen from the VRR extension.
 *
 * When the last registered screen is removed the ProcVector wrappers are
 * torn down.  Call from the driver's CloseScreen / FreeRec path.
 */
extern _X_EXPORT void VRRExtFini(ScreenPtr screen);
 
/*
 * VRRGetAtom - Return the _VARIABLE_REFRESH Atom.
 *
 * Returns None before the first successful VrrExtInit() call.
 */
extern _X_EXPORT Atom VRRGetAtom(void);
 
#endif /* _VRR_H_ */