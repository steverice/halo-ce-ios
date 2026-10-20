/*
P2P.H

Internet play (p2p.c): machines that shared an invite reach each other's
system link games as if they were on one LAN. xnet.c routes the game's
traffic for them through here; sdl_platform.c passes invite links in and
out through the clipboard.

Addresses and ports are in network byte order.
*/

#ifndef __HALO_LINUX_P2P_H
#define __HALO_LINUX_P2P_H

/* starts internet play, if network.online is set, when the game starts
its networking; local_address is the address the game's sockets are
reached at (network.address, else 127.0.0.1) */
void p2p_initialize(unsigned long local_address);

/* on the desktop, before anything else: if this process was started with
an invite link (halo://join/...) and another copy of the game is running,
passes the link to it and returns nonzero (this one should quit) */
int p2p_hand_off_invite(void);

/* joins the game an invite link or code leads to; text may hold other
words around it. Returns nonzero if it held an invite */
int p2p_join_invite(const char *text);
/* the same, for internet play about to start (p2p_initialize next, on this
thread): without p2p_join_invite's "internet play is off" line. The join
waits for p2p's thread, as one asked for at any time does */
int p2p_join_invite_starting(const char *text);

/* whether internet play runs (p2p_initialize started its thread). The main
thread's: p2p_initialize sets it there, from WSAStartup or
xnet_start_internet_play */
int p2p_running(void);
/* whether network.online asked for internet play and p2p_initialize could
not start it (its socket, most often). The main thread's, as p2p_running */
int p2p_start_failed(void);

/* (xnet.c) internet play turned on while the game runs: p2p_initialize, which
WSAStartup ran with network.online off, then the ports of the game's
sockets already open, which it would otherwise never learn */
void xnet_start_internet_play(void);

/* how joining the latest invite goes, which Join Game > Direct Link shows:
reaching the brokers, asking the host through them, reaching it directly,
reached; or what went wrong, as soon as it is known (joining goes on
until it gives up, so a stage can still pass) */
enum
{
	P2P_JOIN_NONE,
	P2P_JOIN_STARTING,
	P2P_JOIN_ASKING,
	P2P_JOIN_REACHING,
	P2P_JOIN_CONNECTED,
	/* no broker reached */
	P2P_JOIN_NO_SERVER,
	/* brokers reached, but no answer from the host: its game ended, or it
	quit (the brokers cannot tell which, or whether it was ever there) */
	P2P_JOIN_NOT_FOUND,
	/* the host's listing (a public game's) says its game ended */
	P2P_JOIN_ENDED,
	/* ... holds another invite: this one is old */
	P2P_JOIN_NEW_INVITE,
	/* ... has another network version */
	P2P_JOIN_VERSION,
	/* the host answered, but no packet got through either way */
	P2P_JOIN_NO_DIRECT,
	/* ... and this machine was refused sending to the host's local address
	(on Apple's systems: the Local Network permission, off or not given yet) */
	P2P_JOIN_LOCAL_NETWORK,
};
/* whether the signalling brokers, which invites and the server browser go
through, can be reached (halo_internet.h's HALO_INTERNET_SERVERS_*): none
is contacted before a game is hosted for the internet, an invite is joined
or the server browser is opened */
int p2p_servers_status(void);

/* the stage (P2P_JOIN_*), the seconds since the invite came, whether
joining has given up, and (P2P_JOIN_VERSION) the host's network version */
int p2p_join_status(int *seconds, int *over, int *host_version);

/* this machine's identifier, which its XNADDR carries (6 bytes) */
const unsigned char *p2p_identifier(void);
/* the address the game reaches the machine with this identifier at, if it
is an internet play peer (reached yet or not) */
int p2p_peer_address(const unsigned char *identifier, unsigned long *address);

/* a destination the game sends to or connects to (stream: a TCP socket;
socket: the game's UDP socket connected to it, else -1): 1 if it is a peer's
address, rewritten to the local address standing in for it; -1 if it is a
peer's (or was) but the peer cannot be reached now (the game's traffic must
not go to the address itself); 0 if it is not a peer's */
int p2p_outgoing(int stream, int socket, unsigned long *address, unsigned short *port);
/* a source the game received from, accepted from or is connected to: if
it is one standing in for a peer, the peer's address */
int p2p_incoming(int stream, unsigned long *address, unsigned short *port);
/* the local addresses standing in for this port (a broadcast's) on every
peer; returns their count */
int p2p_broadcast_targets(unsigned short port, unsigned long *addresses, unsigned short *ports, int maximum_count);
/* a datagram the game sends from a socket with the local port source_port
(bound: not 0) to address and port, sealed onto the tunnel at once (no
stand-in carries it; one is made for the answers when they come): 1 if it
is a peer's address (sent, or lost as a datagram may be); -1 if it is a
peer's (or was) but the peer cannot be reached now; 0 if it is not a
peer's */
int p2p_send_datagram(unsigned short source_port, unsigned long address, unsigned short port, const void *data,
	int size);
/* the same for a broadcast to port: to every peer the tunnel reaches;
returns their count */
int p2p_broadcast_datagram(unsigned short source_port, unsigned short port, const void *data, int size);

/* the game's socket has this local port: bound, given one, or listening
(stream and listening: it is hosting). Peers reach only these ports (a
stream's only while it listens), and datagram ports it sent them from */
void p2p_socket_port(int socket, int stream, int listening, unsigned short port);
/* a socket that peers are not to reach (bound to 127.0.0.1 alone) has this
local port: traffic from it is not a peer's whose stand-in had the port */
void p2p_port_taken(int stream, unsigned short port);
/* the game closes a socket, a datagram one of this local port (0: none, or
a stream's) */
void p2p_socket_closed(int socket, unsigned short datagram_port);

/* text for the clipboard (a new invite link), once; NULL if none. Called
from the main thread */
const char *p2p_take_clipboard_text(void);
/* nonzero once after an invite from elsewhere (join_link.txt on Apple
builds and Android, the hand-off on the desktop) was taken. Called from
the main thread */
int p2p_take_invite_taken(void);
/* nonzero once after a join's host never answered (it gave up after 90
seconds). Called from the main thread */
int p2p_take_join_failed(void);

/* whether a game this machine hosts may be joined from the internet (an
invite, Discord): the PC menus' Create Game > Internet (the default), not
> LAN */
void p2p_set_hosting_allowed(int allowed);
/* the invite link of the game this machine hosts (empty, 0: none, or LAN
only) */
int p2p_invite_link(char *link, int size);
/* the join code of the game this machine hosts, as shown ("ABCD-EFGH"):
0 and empty if none (p2p_code.h) */
int p2p_host_code(char *text, int size);
/* joins the game a join code stands for (looked up at the signalling
brokers), or the invite link the text holds: nonzero if the text is either;
p2p_join_code_status then says how the code's lookup went */
int p2p_join_code(const char *text);
enum
{
	P2P_CODE_NONE,
	P2P_CODE_LOOKING,
	P2P_CODE_FOUND,
	P2P_CODE_NOT_FOUND,
	/* found, and the tunnel to its host is up: the game's own search finds
	the game next */
	P2P_CODE_REACHED,
};
int p2p_join_code_status(void);

/* a peer whose app went to the background (an Apple device's: suspended in
seconds, its game held), for the game to show and to wait for: its virtual
address, whether it is the host of the game joined, and the seconds it is
waited for (0: back; -1: dropped, the time up). p2p_take_away_notice gives
each once; p2p_peer_away whether the peer at the address is away now */
struct p2p_away_notice
{
	unsigned long address;
	int is_host;
	int seconds;
};
int p2p_take_away_notice(struct p2p_away_notice *notice);
int p2p_peer_away(unsigned long address);
/* the invite of the game this machine joined last, if it was lost after
this app or that game's host was away (in the background) lately: 1 and the
link, which joins it again (port/linux/game/network_away.c); else 0 */
int p2p_rejoin_invite(char *text, int size);

/* the hosted game's players and the most it takes, which Discord shows
(0, 0: not hosting; until the game says, the machines the tunnel reaches
are shown). The game's server calls it as they change (calling it with the
same counts again costs little) */
void p2p_set_game_player_counts(int count, int maximum);

/* ---- the server browser's public games (p2p_lobby.c) */

/* whether the game this machine hosts for the internet (not LAN) is listed
in everyone's server browser: Create Game's PUBLIC; a private game is
reached only by its invite. Going private makes a new invite, if the old one
was listed */
void p2p_set_hosting_public(int public);
/* the password of the game hosted for the internet (Server Setup's
PASSWORD; NULL or empty: none): its listing lets only those who know it
join from the server browser (its invite link still joins it). Setting or
changing it makes a new invite, if the old one was listed. It takes a few
milliseconds (the password's key) */
void p2p_set_hosting_password(const char *password);
/* the hosted game's details as listed (printable ASCII is kept; NULL leaves
one as it was): the game's server calls it as they change (calling it with
the same again costs little) */
void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int open,
	int in_progress, int has_teams);
/* the server browser: while on, the public games are gathered (asked for
when turned on, and by p2p_lobby_refresh) */
void p2p_lobby_browse(int on);
void p2p_lobby_refresh(void);

enum
{
	P2P_LISTING_NAME_SIZE = 32,
	P2P_LISTING_MAP_SIZE = 32,
	P2P_LISTING_GAMETYPE_SIZE = 24,
	/* an invite link's text (P2P_LINK_SIZE) */
	P2P_LISTING_INVITE_SIZE = 77,
	/* a password's game's: its host's key and its key's hash, and its token
	sealed (P2P_KEY_SIZE, P2P_KEY_HASH_SIZE, P2P_SEALED_TOKEN_SIZE) */
	P2P_LISTING_SIGNING_KEY_SIZE = 32,
	P2P_LISTING_KEY_HASH_SIZE = 16,
	P2P_LISTING_SEALED_TOKEN_SIZE = 56,
};

struct p2p_listing
{
	/* (empty for a locked one until p2p_listing_unlock opens it) */
	char invite[P2P_LISTING_INVITE_SIZE];
	/* the host's (its XNADDR's abEnet once reached) */
	unsigned char identifier[6];
	char name[P2P_LISTING_NAME_SIZE + 1];
	char map[P2P_LISTING_MAP_SIZE + 1];
	char gametype[P2P_LISTING_GAMETYPE_SIZE + 1];
	unsigned char player_count, maximum_player_count, engine_type;
	unsigned char open, in_progress, has_teams;
	/* joining it failed this run (p2p_lobby_mark_failed) */
	unsigned char failed;
	/* milliseconds, -1 if not known */
	short ping;
	/* it has a password: its invite is sealed with it (p2p_listing_unlock) */
	unsigned char locked;
	/* the host's network version, and whether this build can join it
	(halo_network_join.h): a game of another version is listed, with its
	version, and not joined. from_delta: Delta's list gave it, not the
	signalling brokers */
	short network_version;
	unsigned char joinable, from_delta;
	unsigned char key_hash[P2P_LISTING_KEY_HASH_SIZE];
	unsigned char signing_key[P2P_LISTING_SIGNING_KEY_SIZE];
	unsigned char sealed_token[P2P_LISTING_SEALED_TOKEN_SIZE];
};
/* a copy of the public games found, in the order shown (those this build
can join first, then the most players; then those not failed, the open
ones, by name); returns their count */
int p2p_lobby_games(struct p2p_listing *games, int maximum_count);
/* the public games heard while browsing that the list leaves out, since
their network version is not this build's: those of a newer one, and of an
older one (their listings' signatures are not checked: a count to tell the
player with, no more) */
void p2p_lobby_other_versions(int *newer, int *older);
/* a game the browser could not join: kept, marked failed, for this run */
void p2p_lobby_mark_failed(const unsigned char *identifier);
/* a locked game's invite (into its invite), if the password is its own:
FALSE if not; TRUE for a game with none. It takes a few milliseconds */
int p2p_listing_unlock(struct p2p_listing *listing, const char *password);

/* the sizes of a Discord user's id and name as kept (with their end), and
the text kept of either as told: only the characters allowed (digits in an
id; letters, digits, "_", "." and "-" in a name), no longer than that */
enum
{
	P2P_DISCORD_ID_SIZE = 24,
	P2P_DISCORD_NAME_SIZE = 40,
};
void p2p_discord_sanitize(char *destination, int size, const char *source, int name);
/* the Discord user signed in to the client on this machine, as its
READY told (empty if none, or internet play is off) */
void p2p_discord_identity(char *id, int id_size, char *name, int name_size);
/* this machine's hardware id, as hex (empty if none), a host told it when
joining (a keyed hash of what the machine is known by: p2p.c); and the hex
kept of one told: lowercase hex digits only, P2P_HARDWARE_ID_BYTES' worth */
enum
{
	P2P_HARDWARE_ID_BYTES = 16,
	P2P_HARDWARE_ID_SIZE = 2 * P2P_HARDWARE_ID_BYTES + 1,
};
void p2p_hardware_id(char *hex, int size);
void p2p_hardware_id_sanitize(char *destination, int size, const char *source);

/* the real address (network byte order) of the internet play peer of this
virtual address (network byte order): where its packets come from; 0 if
it is no peer's */
unsigned long p2p_peer_endpoint_address(unsigned long virtual_address);

/* ---- map sharing between the players of a game (port/linux/game/map_share.c)

The players of an internet game reach its host alone. To share a map they
reach each other too: a host tells a player the key hash of another, which
answers holders of the invite it joined with as its host does (only while
answering), and the player asks it for a session as it asked the host. */

enum
{
	/* the hash of a machine's public key, which reaches it */
	P2P_PLAYER_KEY_HASH_SIZE = 16,
	/* the port each machine takes the others' map sharing connections on:
	its game listens there without hosting */
	P2P_MAP_SHARE_PORT = 5153,
};

/* the identifier (6 bytes) and key hash of the internet play peer of this
virtual address (network byte order): 0 if it is no peer's */
int p2p_peer_key_hash(unsigned long virtual_address, unsigned char *identifier, unsigned char *key_hash);
/* whether this machine, a player of an internet game (not its host),
answers the game's other players: holders of the invite it joined with */
void p2p_set_answering_players(int on);
/* asks for a session with another player of the internet game this
machine joined, whose key has this hash (as the game's host told), if there
is none: nonzero if there is one, or it is asked for (for some seconds, or
until reached; asked again, it is asked for anew) */
int p2p_reach_player(const unsigned char *key_hash);
/* whether the tunnel reaches the machine with this identifier, and the
virtual address (network byte order) the game reaches it at */
int p2p_peer_reached(const unsigned char *identifier, unsigned long *address);

/* BLAKE2b with a 32-byte digest (Monocypher's): a map's pieces
(map_share.c) */
void p2p_blake2b_256(const void *data, unsigned long size, unsigned char *digest);

#endif
