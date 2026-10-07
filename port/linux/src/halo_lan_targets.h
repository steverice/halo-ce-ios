/*
HALO_LAN_TARGETS.H

Where a system link broadcast goes (xnet.c's broadcast_targets), and the
listening sockets that hold this machine's discovery up while it hosts.
Header-only and pure, so the probe can run it
(port/ios/tests/lan_targets_probe.c). Addresses are IPv4, network byte
order.
*/

#ifndef HALO_LAN_TARGETS_H
#define HALO_LAN_TARGETS_H

#define HALO_LAN_BROADCAST 0xFFFFFFFFUL
/* send the datagram to 255.255.255.255 as given, exactly as before */
#define HALO_LAN_SEND_ORIGINAL (-1)
/* the fork tag discovery advertises: 0, this build's wire is upstream's
(spec section 7) */
#define HALO_LAN_FORK_TAG 0UL
/* the most addresses discovery reports (64 machines, 4 addresses each) */
#define HALO_LAN_MAXIMUM_PEERS 256
/* posix_lan_status's bits (posix.h's _posix_lan_broadcast, _posix_lan_refused) */
#define HALO_LAN_STATUS_BROADCAST 1
#define HALO_LAN_STATUS_REFUSED 2

static inline int halo_lan_has(const unsigned long *targets, int count, unsigned long address)
{
	int index;

	for (index = 0; index < count; index++)
		if (targets[index] == address)
			return 1;
	return 0;
}

/* network.broadcast's list as it is, its 255.255.255.255 entries made the
real broadcast where broadcasts are allowed and left out where they are not;
the real broadcast if the list is empty and broadcasts are allowed; then the
peers discovery found, each once and none already there. At most
maximum_count; HALO_LAN_SEND_ORIGINAL when that is 255.255.255.255 alone
(Linux and Windows with no list, as before) */
static inline int halo_lan_targets(const unsigned long *list, int list_count, int broadcast_allowed,
	unsigned long real_broadcast, const unsigned long *peers, int peer_count, unsigned long *targets, int maximum_count)
{
	int count = 0;
	int index;

	if (!list_count && !peer_count && broadcast_allowed && real_broadcast == HALO_LAN_BROADCAST)
		return HALO_LAN_SEND_ORIGINAL;
	for (index = 0; index < list_count && count < maximum_count; index++)
	{
		if (list[index] != HALO_LAN_BROADCAST)
			targets[count++] = list[index];
		else if (broadcast_allowed)
			targets[count++] = real_broadcast;
	}
	if (!list_count && broadcast_allowed && count < maximum_count)
		targets[count++] = real_broadcast;
	for (index = 0; index < peer_count && count < maximum_count; index++)
		if (!halo_lan_has(targets, count, peers[index]))
			targets[count++] = peers[index];
	return count;
}

/* the game's listening sockets (a host's): discovery stays registered while
there is one */
struct halo_lan_listeners
{
	int sockets[16];
	int count;
};

/* returns the count after */
static inline int halo_lan_listener_add(struct halo_lan_listeners *listeners, int socket)
{
	int index;

	for (index = 0; index < listeners->count; index++)
		if (listeners->sockets[index] == socket)
			return listeners->count;
	if (listeners->count < (int)(sizeof(listeners->sockets) / sizeof(listeners->sockets[0])))
		listeners->sockets[listeners->count++] = socket;
	return listeners->count;
}

/* 1 if the socket was listening */
static inline int halo_lan_listener_remove(struct halo_lan_listeners *listeners, int socket)
{
	int index;

	for (index = 0; index < listeners->count; index++)
	{
		if (listeners->sockets[index] == socket)
		{
			listeners->sockets[index] = listeners->sockets[--listeners->count];
			return 1;
		}
	}
	return 0;
}

static inline unsigned long halo_lan_subnet_broadcast(unsigned long address, unsigned long netmask)
{
	return ((address & netmask) | ~netmask) & HALO_LAN_BROADCAST;
}

#endif
