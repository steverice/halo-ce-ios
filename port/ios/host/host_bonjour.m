/* LAN discovery on iOS, iPadOS, visionOS and the Mac runner (spec section 2). */
#import <Foundation/Foundation.h>
#include "ios_host.h"
#include "host_config.h"
#include "posix.h"
#include <TargetConditionals.h>
#include <string.h>

/* port/ios/CMakeLists.txt: the chosen entitlements file has
com.apple.developer.networking.multicast */
#ifndef HALO_MULTICAST_ENTITLED
#define HALO_MULTICAST_ENTITLED 0
#endif

/* network.lan_broadcast, read once: whether real broadcasts are sent ("auto":
on a Mac, both the iPad app and the Mac Catalyst runner, and on an iOS or
visionOS device only with the multicast entitlement) */
static int broadcast_allowed(void)
{
	static int allowed = -1;

	if (allowed < 0)
	{
		char setting[16];

		host_config_string("network.lan_broadcast", "auto", setting, sizeof(setting));
		if (!strcmp(setting, "on"))
			allowed = 1;
		else if (!strcmp(setting, "off"))
			allowed = 0;
		else
			allowed = (NSProcessInfo.processInfo.isiOSAppOnMac || NSProcessInfo.processInfo.isMacCatalystApp) ? 1 :
				HALO_MULTICAST_ENTITLED;
		host_logf(HOST_LOG_INFO, "LAN: broadcasts %s (network.lan_broadcast = \"%s\")",
			allowed ? "on" : "off", setting);
	}
	return allowed;
}

int posix_lan_status(void)
{
	return broadcast_allowed() ? _posix_lan_broadcast : 0;
}

posix_ulong posix_lan_broadcast_address(void)
{
	return 0xFFFFFFFFu;
}

void posix_lan_discovery(int hosting, int searching, unsigned short version, posix_ulong fork)
{
	(void)hosting;
	(void)searching;
	(void)version;
	(void)fork;
}

int posix_lan_peers(posix_ulong *addresses, int maximum_count)
{
	(void)addresses;
	(void)maximum_count;
	return 0;
}
