/* The menus' platform attribute (port/linux/src/menu_platform.h): "quit" is for the desktop ports and the Mac app
(the host sets HALO_APP_CAN_QUIT) and not for an iPhone, an iPad, an Apple TV or a Vision Pro; "desktop" and "android"
are as before; anything else is refused. Built once for the desktop, once as the Apple guest (HALO_ILP32), once as
Android; the Apple guest's runs are with the variable and without it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../linux/src/menu_platform.h"

static int failures;

static void check(char const *platform, int expected)
{
	int got = menu_platform_matches(platform);

	printf("%s: \"%s\" is %d, expected %d\n", got == expected ? "ok" : "FAIL", platform, got, expected);
	if (got != expected)
		failures++;
}

int main(int argc, char **argv)
{
	/* argv[1]: "desktop", "apple", "mac" or "android" */
	char const *kind = argc > 1 ? argv[1] : "";
#if defined(HALO_ANDROID)
	int want_quit = 0, want_desktop = 0, want_android = 1;
#elif defined(HALO_ILP32)
	int want_quit = getenv("HALO_APP_CAN_QUIT") != NULL, want_desktop = 0, want_android = 1;
#else
	int want_quit = 1, want_desktop = 1, want_android = 0;
#endif
#if defined(HALO_IOS)
	check("cloud_saves", getenv("HALO_HOST_CLOUD_SAVES") && !strcmp(getenv("HALO_HOST_CLOUD_SAVES"), "1"));
#else
	check("cloud_saves", 0);
#endif
	check("quit", want_quit);
	check("desktop", want_desktop);
	check("android", want_android);
	check("tv", -1);
	check("", -1);
	/* the build and the case are what the caller asked for */
	if ((!strcmp(kind, "mac")) != (getenv("HALO_APP_CAN_QUIT") != NULL))
	{
		printf("FAIL: the environment is not the case's\n");
		failures++;
	}
	printf("%s (%s)\n", failures ? "menu platform probe FAILED" : "menu platform probe passed", kind);
	return failures != 0;
}
