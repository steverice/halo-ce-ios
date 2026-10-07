/*
MENU_PLATFORM.H

Whether a menu element's platform attribute (port/assets/menus/README.md) is
for this build: "desktop" on the desktop ports, "android" on Android and on
the Apple devices' game (HALO_ILP32, which also runs as the Mac app), and
"quit" where the app can end itself, so the main menu's Quit is there: the
desktop ports, and the Mac app, which its host says by setting
HALO_APP_CAN_QUIT. An iPhone, an iPad, an Apple TV and a Vision Pro leave to
the system, and a Quit there only asked a question whose OK did nothing.
"mac_app" is the Apple host's Mac app alone, which says so by setting
HALO_HOST_MAC: the one Apple build with a setting the others ignore
(display.frame_rate). "apple" is every Apple host build (HALO_IOS): the
lobby's Y, which shares the invite link. Returns -1 for a value that is none of these. A header
of its own so that tools/ios_test.py can build the three cases.
*/

#ifndef MENU_PLATFORM_H
#define MENU_PLATFORM_H

#include <stdlib.h>
#include <string.h>

static int menu_platform_matches(char const *platform)
{
	if (!strcmp(platform, "quit"))
	{
#if defined(HALO_ANDROID)
		return 0;
#elif defined(HALO_ILP32)
		return getenv("HALO_APP_CAN_QUIT") != NULL;
#else
		return 1;
#endif
	}
	if (!strcmp(platform, "mac_app"))
	{
#if defined(HALO_ILP32) && !defined(HALO_ANDROID)
		char const *mac = getenv("HALO_HOST_MAC");

		return mac && *mac == '1';
#else
		return 0;
#endif
	}
	if (!strcmp(platform, "apple"))
	{
#if defined(HALO_IOS)
		return 1;
#else
		return 0;
#endif
	}
	if (strcmp(platform, "desktop") && strcmp(platform, "android"))
		return -1;
#if defined(HALO_ANDROID) || defined(HALO_ILP32)
	return !strcmp(platform, "android");
#else
	return !strcmp(platform, "desktop");
#endif
}

#endif
