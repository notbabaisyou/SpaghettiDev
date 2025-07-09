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

/**
 * Sauce.
 * The interposer between the Display Server and Meatball.
 */

#include <stdarg.h>
#include <xf86.h>
#include <xf86Module.h>

#include <meatball/meatball.h>

#define SAUCE_DEFAULT_VERB	1

static MODULESETUPPROTO(sauce_setup);
static MODULETEARDOWNPROTO(sauce_shutdown);

static XF86ModuleVersionInfo VersRec =
{
	.modname      = "sauce",
	.vendor       = "Spaghetti Fork",
	._modinfo1_   = MODINFOSTRING1,
	._modinfo2_   = MODINFOSTRING2,
	.xf86version  = XORG_VERSION_CURRENT,
	.majorversion = 1,
	.minorversion = 0,
	.patchlevel   = 0,
	.abiclass     = ABI_CLASS_EXTENSION,
	.abiversion   = ABI_EXTENSION_VERSION,
};

_X_EXPORT XF86ModuleData sauceModuleData =
{
	.vers = &VersRec,
	.setup = sauce_setup,
	.teardown = sauce_shutdown
};

static void
RedirectLogToServer(enum MBLogType type, const char* format, va_list fmt)
{
	/**
	 * TODO: We need to pass the scrnIndex
	 * through so we actually log something sane.
	 */
	switch (type)
	{
		case MB_LOG_NONE:
		{
			xf86VDrvMsgVerb(0, X_NONE, SAUCE_DEFAULT_VERB, format, fmt);
			break;
		}

		case MB_LOG_DEBUG:
		{
			xf86VDrvMsgVerb(0, X_DEBUG, SAUCE_DEFAULT_VERB, format, fmt);
			break;
		}

		case MB_LOG_INFO:
		{
			xf86VDrvMsgVerb(0, X_INFO, SAUCE_DEFAULT_VERB, format, fmt);
			break;
		}

		case MB_LOG_WARNING:
		{
			xf86VDrvMsgVerb(0, X_WARNING, SAUCE_DEFAULT_VERB, format, fmt);
			break;
		}

		case MB_LOG_ERROR:
		{
			xf86VDrvMsgVerb(0, X_ERROR, SAUCE_DEFAULT_VERB, format, fmt);
			break;
		}

		default:
		{
			assert(!"Hit unknown MBLogType");
		}
	}
}

static ScrnInfoPtr
GetPrimaryScreen(void)
{
	for (int i = 0; i < screenInfo.numScreens; i++) {
		const ScreenPtr screen = screenInfo.screens[i];

		if (!screen)
		{
			continue;
		}

		/**
		 * Skip any PRIME or offload screens since
		 * they won't be of any use to us.
		 */
		if (screen->is_offload_secondary || screen->is_output_secondary)
		{
			continue;
		}

		return xf86ScreenToScrn(screen);
	}

	return NULL;
}

static void *
sauce_setup(void *module, void *opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (setupDone) {
		/* Allow only one instance of Meatball for now. */
		if (errmaj)
		{
			*errmaj = LDR_ONCEONLY;
		}
		return NULL;
	}

#if defined(SAUCE_ABI_EXPECTED_MAJOR) && defined(SAUCE_ABI_EXPECTED_MINOR)
	unsigned long version;
	meatball_version(&version);

	unsigned long major, minor;
	
	major = GET_MODULE_MAJOR_VERSION(version);
	minor = GET_MODULE_MINOR_VERSION(version);

	if (major != SAUCE_ABI_EXPECTED_MAJOR && minor != SAUCE_ABI_EXPECTED_MINOR)
	{
		/* ABI mismatch, likely something would break. */
		if (errmaj)
		{
			*errmaj = LDR_MISMATCH;
		}
		return NULL;	
	}
#else
#warning "Sauce: ABI check unavailable, you might be in for a surprise!"
#endif

	setupDone = TRUE;

	struct meatball_config config = { 0 };

	/* Software only for now. */
	config.meatball_flags = MB_FORCE_SOFTWARE_RENDERING;

	/* Setup the display server logger here. */
	config.log_to_server = &RedirectLogToServer;

	return meatball_initialize(&config) ? module : NULL;
}

static void
sauce_shutdown(void *teardownData)
{
	meatball_shutdown();
}