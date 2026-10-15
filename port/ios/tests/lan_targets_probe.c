#include "halo_lan_targets.h"
#include "posix.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define A 0x0200000AUL /* 10.0.0.2 */
#define B 0x0300000AUL /* 10.0.0.3 */
#define C 0x0400000AUL /* 10.0.0.4 */
#define SUBNET 0xFF00000AUL /* 10.0.0.255 */
#define ALL HALO_LAN_BROADCAST

static int targets(const unsigned long *list, int list_count, int allowed, unsigned long real,
	const unsigned long *peers, int peer_count, unsigned long *out)
{
	return halo_lan_targets(list, list_count, allowed, real, peers, peer_count, out, 8);
}

int main(void)
{
	unsigned long out[8];
	struct halo_lan_listeners listeners;

	assert(HALO_LAN_STATUS_BROADCAST == _posix_lan_broadcast && HALO_LAN_STATUS_REFUSED == _posix_lan_refused);

	/* Linux and Windows: no list, no peers, broadcasts allowed: the original
	datagram to 255.255.255.255, exactly as before */
	assert(targets(NULL, 0, 1, ALL, NULL, 0, out) == HALO_LAN_SEND_ORIGINAL);
	/* Linux and Windows with a list: the list as it is, 255.255.255.255 too */
	{
		const unsigned long list[] = { B, ALL, A, B };

		assert(targets(list, 4, 1, ALL, NULL, 0, out) == 4);
		assert(out[0] == B && out[1] == ALL && out[2] == A && out[3] == B);
	}
	/* the Mac runner, or an entitled device, with peers: the real broadcast
	stays and the peers come after it */
	{
		const unsigned long peers[] = { A, B };

		assert(targets(NULL, 0, 1, ALL, peers, 2, out) == 3 && out[0] == ALL && out[1] == A && out[2] == B);
		assert(targets(NULL, 0, 1, SUBNET, peers, 2, out) == 3 && out[0] == SUBNET);
	}
	/* an entitled device with no list and no peers: the subnet's broadcast */
	assert(targets(NULL, 0, 1, SUBNET, NULL, 0, out) == 1 && out[0] == SUBNET);
	/* an unentitled device with nothing found: nothing to send */
	assert(targets(NULL, 0, 0, ALL, NULL, 0, out) == 0);
	/* an unentitled device: the list's 255.255.255.255 dropped, the peers
	appended once, a peer already listed not again, the order kept */
	{
		const unsigned long list[] = { B, ALL };
		const unsigned long peers[] = { A, B, A, C };

		assert(targets(list, 2, 0, ALL, peers, 4, out) == 3);
		assert(out[0] == B && out[1] == A && out[2] == C);
	}
	/* an entitled device's list naming 255.255.255.255: the subnet's in its place */
	{
		const unsigned long list[] = { ALL, B };

		assert(targets(list, 2, 1, SUBNET, NULL, 0, out) == 2 && out[0] == SUBNET && out[1] == B);
	}
	/* never more than asked for */
	{
		const unsigned long peers[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

		assert(targets(NULL, 0, 0, ALL, peers, 10, out) == 8 && out[7] == 8);
	}

	/* listening sockets: the hold drops only with the last one, and closing
	a socket that never listened, or closing one twice, changes nothing */
	memset(&listeners, 0, sizeof(listeners));
	assert(halo_lan_listener_add(&listeners, 7) == 1);
	assert(halo_lan_listener_add(&listeners, 9) == 2);
	assert(halo_lan_listener_add(&listeners, 7) == 2);
	assert(halo_lan_listener_remove(&listeners, 5) == 0 && listeners.count == 2);
	assert(halo_lan_listener_remove(&listeners, 7) == 1 && listeners.count == 1);
	assert(halo_lan_listener_remove(&listeners, 7) == 0 && listeners.count == 1);
	assert(halo_lan_listener_remove(&listeners, 9) == 1 && listeners.count == 0);

	/* a subnet's broadcast address, in network byte order */
	assert(halo_lan_subnet_broadcast(0x1400000AUL, 0x00FFFFFFUL) == SUBNET);
	assert(halo_lan_subnet_broadcast(0x0500000AUL, 0x0000FFFFUL) == 0xFFFF000AUL);
	/* the LAN list's empty text: refused first, then Apple devices only */
	assert(halo_lan_empty_text(HALO_LAN_STATUS_BROADCAST) == HALO_LAN_TEXT_LOOKING);
	assert(halo_lan_empty_text(0) == HALO_LAN_TEXT_APPLE_ONLY);
	assert(halo_lan_empty_text(HALO_LAN_STATUS_REFUSED) == HALO_LAN_TEXT_REFUSED);
	assert(halo_lan_empty_text(HALO_LAN_STATUS_BROADCAST | HALO_LAN_STATUS_REFUSED) == HALO_LAN_TEXT_REFUSED);
	puts("PASS: broadcast targets (Linux unchanged, peers added, unentitled, entitled), listeners, subnet broadcast, empty-list text");
	return 0;
}
