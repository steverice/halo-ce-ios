/*
HALO_INTERFACE_RANK.H

Which network interface an iPhone, iPad or Vision Pro takes its system link
address from (posix_net.c, spec section 6): Wi-Fi (en0), then the network it
shares its connection on (bridge100, the Personal Hotspot), then any other
with broadcasts (a USB Ethernet adapter, a Developer Strap). Cellular
(pdp_ip) and VPNs (utun) are point-to-point without broadcasts, and never
taken, nor are loopback or interfaces that are down. Lower is better; -1 is
never. Header-only and pure (port/ios/tests/interface_rank_probe.c).
*/

#ifndef HALO_INTERFACE_RANK_H
#define HALO_INTERFACE_RANK_H

#include <net/if.h>
#include <string.h>

static inline int halo_interface_rank(const char *name, unsigned int flags)
{
	if ((flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) || (flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) ||
		!(flags & IFF_BROADCAST))
	{
		return -1;
	}
	if (!strcmp(name, "en0"))
		return 0;
	if (!strcmp(name, "bridge100"))
		return 1;
	return 2;
}

#endif
