/*
HALO_INTERNET.H

Internet play's decisions that the menus show (p2p.c, p2p_signal.c,
p2p_lobby.c, menu_functions.c): whether the signalling brokers can be
reached, and what the server browser says under an empty list.
Header-only and pure, so the probe can run it
(port/ios/tests/internet_probe.c).
*/

#ifndef HALO_INTERNET_H
#define HALO_INTERNET_H

/* the signalling brokers, as p2p_servers_status tells them */
enum
{
	/* nothing has needed them yet: none has been contacted */
	HALO_INTERNET_SERVERS_NONE,
	/* contacted, none reached yet */
	HALO_INTERNET_SERVERS_TRYING,
	HALO_INTERNET_SERVERS_REACHED,
	/* none can be reached: invites and the server browser cannot work */
	HALO_INTERNET_SERVERS_UNREACHABLE,
};

/* milliseconds without any broker reached before they are called
unreachable (a broker's connection times out in 10 seconds: p2p_signal.c's
CONNECT_TIMEOUT; p2p_internal.h's P2P_JOIN_BROKER_WAIT is the same wait) */
#define HALO_INTERNET_SERVERS_WAIT 12000UL

/* started: signalling has begun (something needed the brokers); brokers:
how many there are; failed: those tried and not reached since; connected:
whether one is reached now; elapsed: milliseconds since one was last
reached, or since signalling began if none ever was */
static inline int halo_internet_servers(int started, int brokers, int failed, int connected, unsigned long elapsed)
{
	if (!started)
		return HALO_INTERNET_SERVERS_NONE;
	if (connected)
		return HALO_INTERNET_SERVERS_REACHED;
	if (brokers <= 0 || failed >= brokers || elapsed >= HALO_INTERNET_SERVERS_WAIT)
		return HALO_INTERNET_SERVERS_UNREACHABLE;
	return HALO_INTERNET_SERVERS_TRYING;
}

/* the line under the server browser's empty list */
enum
{
	HALO_INTERNET_BROWSER_LOOKING,
	HALO_INTERNET_BROWSER_NONE_FOUND,
	/* no broker can be reached, so no listing can come */
	HALO_INTERNET_BROWSER_NO_SERVERS,
	/* every game heard is of a newer network version: this build is old */
	HALO_INTERNET_BROWSER_NEWER_ONLY,
	/* ... of an older one */
	HALO_INTERNET_BROWSER_OLDER_ONLY,
};

/* servers: HALO_INTERNET_SERVERS_*; looking: the browser opened or was
refreshed too lately to say that nothing is there; newer and older: the
public games heard whose network version is not this build's, which the
list leaves out. Unreachable brokers are said at once (nothing else can
come); the rest waits for the look to end */
static inline int halo_internet_browser_empty(int servers, int looking, int newer, int older)
{
	if (servers == HALO_INTERNET_SERVERS_UNREACHABLE)
		return HALO_INTERNET_BROWSER_NO_SERVERS;
	if (looking)
		return HALO_INTERNET_BROWSER_LOOKING;
	if (newer > 0)
		return HALO_INTERNET_BROWSER_NEWER_ONLY;
	if (older > 0)
		return HALO_INTERNET_BROWSER_OLDER_ONLY;
	return HALO_INTERNET_BROWSER_NONE_FOUND;
}

#endif
