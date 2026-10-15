/*
HALO_INTERNET.H

Internet play's decisions that the menus show (p2p.c, p2p_signal.c,
p2p_lobby.c, menu_functions.c): whether the signalling brokers can be
reached, what the server browser says under an empty list, and, on Apple
builds, when the player is asked to turn internet play on and what the
question tells them. Header-only and pure, so the probe can run it
(port/ios/tests/internet_probe.c).

Internet play reaches servers this project does not run: the MQTT brokers
of brokers.txt (network.brokers_file) and the STUN servers of
network.stun_servers. Each of them sees the address this machine connects
from, and so does every player it plays with. An Apple build therefore
turns network.online on only after the player has read that and agreed
(HALO_INTERNET_DISCLOSURE), whichever way they came: the Settings switch, a
join link, or the first internet screen they open (network.online_ask).
*/

#ifndef HALO_INTERNET_H
#define HALO_INTERNET_H

/* what the player is told before internet play is turned on: who runs the
servers, and what they and the other players learn */
#define HALO_INTERNET_DISCLOSURE \
	"Internet Play finds other players through public matchmaking servers that VISR's developers don't run. " \
	"Those servers, and the players you connect to, can see your IP address. " \
	"Games on your local network don't use it."
#define HALO_INTERNET_ASK_TITLE "Turn on Internet Play?"
#define HALO_INTERNET_ASK_YES "Turn On"
#define HALO_INTERNET_ASK_NO "Not Now"
/* (a join link's question, which has a game to join) */
#define HALO_INTERNET_LINK_TITLE "Join this game over the internet?"
#define HALO_INTERNET_LINK_MESSAGE "VISR will turn on Internet Play. " HALO_INTERNET_DISCLOSURE

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

/* (Apple builds) whether opening an internet screen (Create Game >
Internet, Join Game > Server Browser or Direct Link) asks the player to turn
internet play on: only while it is off and has never run this time, only
while network.online_ask says the question is still owed, and never in a
network game or its lobby, where a question would stop the game for
everyone. Asked once: either answer writes network.online_ask = false */
static inline int halo_internet_should_ask(int online, int running, int ask, int in_network_game)
{
	return !online && !running && ask && !in_network_game;
}

#endif
