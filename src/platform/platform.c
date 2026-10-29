/**
 * Raven - platform/platform.c
 * Selection of the window system compiled into the library.
 */

#include "platform.h"

const rvn_platform_t* rvn_platform_select(int headless) {
	if (rvn_host_platform_registered()) return rvn_platform_host();
	if (headless) return rvn_platform_null();
#if defined(RAVEN_PLATFORM_WIN32)
	return rvn_platform_win32();
#elif defined(RAVEN_PLATFORM_X11)
	return rvn_platform_x11();
#elif defined(RAVEN_PLATFORM_COCOA)
	return rvn_platform_cocoa();
#else
	/* no window system for this OS yet: the library still works headless */
	return rvn_platform_null();
#endif
}
