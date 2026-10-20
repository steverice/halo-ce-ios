/*
HALO_NETWORK_VERSION.H

Which network versions this build plays with, and what each peer's version
turns on (port/linux/NETCODE.md, "Versions"). A host advertises
HALO_PORT_NETWORK_VERSION. A client joins a host of that version or of any
older one back to HALO_PORT_NETWORK_OLDEST_JOINABLE: versions 30 to 32 only
add (a machine that does not know an addition drops it), so the host's
version says which additions it sends and takes. A host does not know its
clients' versions (a join request does not carry one), so it plays every
client as its own.
*/

#ifndef __HALO_NETWORK_VERSION_H
#define __HALO_NETWORK_VERSION_H

#include "halo_port_limits.h"

/* the oldest version a client joins (29 is the last whose rules the later
ones add to) */
#define HALO_PORT_NETWORK_OLDEST_JOINABLE 29

/* what each version added */
#define HALO_NETWORK_VERSION_ELEVATOR_RIDERS 30
#define HALO_NETWORK_VERSION_STREAM_RULES 31
#define HALO_NETWORK_VERSION_AIM_POLICY 32

/* whether a client of version `ours` joins a host of version `theirs` */
static inline int halo_network_version_joinable(unsigned ours, unsigned theirs)
{
	return theirs == ours || (ours >= theirs && theirs >= HALO_PORT_NETWORK_OLDEST_JOINABLE);
}

/* whether a host of the version sends the aim-assist policy (message 90);
a client of an older host plays with the mouse's magnetism as set */
static inline int halo_network_version_has_aim_policy(unsigned version)
{
	return version >= HALO_NETWORK_VERSION_AIM_POLICY;
}

/* how far a datagram's tick may be from the latest the sender's others
carried, in ticks (15 seconds); over its stream any is taken */
#define HALO_NETWORK_STAMP_WINDOW_TICKS 450

static inline int halo_network_stamp_in_window(long latest, long tick)
{
	long difference = tick - latest;

	return difference <= HALO_NETWORK_STAMP_WINDOW_TICKS && difference >= -HALO_NETWORK_STAMP_WINDOW_TICKS;
}

#endif
