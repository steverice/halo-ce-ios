/*
HALO_NETWORK_JOIN.H

Which network versions (halo_port_limits.h) this build can join: the one
place that says so, for the Delta list (p2p_delta.c, delta_list.c), whose
games announce versions from 11 to 32 and which the server browser shows
whatever they are. A game of a version this build cannot join is shown with
its version and not joined.

Header-only and pure, so the probes can run it.
*/

#ifndef HALO_NETWORK_JOIN_H
#define HALO_NETWORK_JOIN_H

#include "halo_network_version.h"

/* whether a host that announces this network version can be joined: this
build's own, or an older one back to HALO_PORT_NETWORK_OLDEST_JOINABLE */
static inline int halo_network_joinable(int version)
{
	return version > 0 && halo_network_version_joinable((unsigned)HALO_PORT_NETWORK_VERSION, (unsigned)version);
}

#endif
