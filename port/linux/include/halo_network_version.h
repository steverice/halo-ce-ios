/*
HALO_NETWORK_VERSION.H

The current agreement is 38. Legacy hosts 29–32 use VISR's known settings
layout and additive messages; the receiver converts their 28-byte options.
Intermediate agreements are not enabled without verified codecs and rules.
A host does not know client versions because join requests do not carry one.
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
	return theirs == ours || (ours >= theirs && theirs >= HALO_PORT_NETWORK_OLDEST_JOINABLE &&
		theirs <= HALO_NETWORK_VERSION_AIM_POLICY);
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

/* whether a message stamped tick is taken, given the latest tick taken from its
sender (-1: none yet) and whether it came over the sender's stream */
static inline int halo_network_stamp_accepts(long latest, long tick, int stream)
{
	return stream || latest == -1 || halo_network_stamp_in_window(latest, tick);
}

#endif
