#include "halo_interface_rank.h"
#include <assert.h>
#include <net/if.h>
#include <stdio.h>

#define UP (IFF_UP | IFF_RUNNING)

int main(void)
{
	/* Wi-Fi first, then the Personal Hotspot network, then any other with broadcasts */
	assert(halo_interface_rank("en0", UP | IFF_BROADCAST) == 0);
	assert(halo_interface_rank("bridge100", UP | IFF_BROADCAST) == 1);
	assert(halo_interface_rank("en5", UP | IFF_BROADCAST) == 2);
	assert(halo_interface_rank("anpi0", UP | IFF_BROADCAST) == 2);
	/* never: cellular and VPNs (point-to-point), loopback, down, not running */
	assert(halo_interface_rank("pdp_ip0", UP | IFF_POINTOPOINT) == -1);
	assert(halo_interface_rank("utun3", UP | IFF_POINTOPOINT) == -1);
	assert(halo_interface_rank("lo0", UP | IFF_LOOPBACK) == -1);
	assert(halo_interface_rank("en0", IFF_UP | IFF_BROADCAST) == -1);
	assert(halo_interface_rank("en0", IFF_BROADCAST) == -1);
	assert(halo_interface_rank("en0", UP) == -1);
	puts("PASS: interface ranking (Wi-Fi, hotspot, others; never cellular, VPN, loopback)");
	return 0;
}
