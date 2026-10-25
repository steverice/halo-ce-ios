/*
DELTA_LIST.H

The Delta game list (https://halo.milenko.org, p2p_delta.c talks to it): the
list of games it serves, read; and the form that announces this machine's
public game to it. Plain C with no system calls, so that the probe can run
it (tools/delta_check.c).

Delta's list is a public service the project does not run. Browsing it is an
anonymous GET; announcing a game sends the game's invite, name and map to it,
and it sees this machine's address (network.delta_announce).
*/

#ifndef DELTA_LIST_H
#define DELTA_LIST_H

enum
{
	/* the invite's hex digits: the host's key hash and the token */
	DELTA_INVITE_DIGITS = 64,
	DELTA_NAME_SIZE = 32,
	DELTA_MAP_SIZE = 32,
	DELTA_GAMETYPE_SIZE = 24,
	/* the map's family: "xbox" (the game's own maps), "ce" (a Custom Edition
	map), or another (a map pack's, "pack") */
	DELTA_FAMILY_SIZE = 15,
	/* the most games taken from one answer (the whole list was 53 games and
	31 KB on 2026-10-20) */
	DELTA_MAXIMUM_GAMES = 256,
	/* the largest answer read; a larger one is a failure */
	DELTA_MAXIMUM_ANSWER = 262144,
};

/* a game of the list, as listed: text kept printable ASCII, cut to size */
struct delta_game
{
	char invite[DELTA_INVITE_DIGITS + 1];
	char name[DELTA_NAME_SIZE + 1];
	/* its short name, without its path and its @family suffix */
	char map[DELTA_MAP_SIZE + 1];
	/* its family, as the list says (or as the map's @family suffix says, if
	the list does not); empty if neither does */
	char family[DELTA_FAMILY_SIZE + 1];
	char gametype[DELTA_GAMETYPE_SIZE + 1];
	int engine;
	int players, maximum_players;
	int open, in_progress, teams;
	/* the host's network version (0: the list has not reached it) */
	int version;
};

/* the games of /v1/games' answer (json, size bytes) that this build can
offer: those with a valid invite that the list reached lately, are not
locked (a password lets only the in-game browser join them) and are not
co-op. Returns how many were put into games (at most maximum), or -1 if the
text is not the list (malformed, nested too deeply, or not an object with a
"games" array). Games with unreadable fields are left out one by one */
int delta_parse_games(const char *json, int size, struct delta_game *games, int maximum);

/* what an announcement says of the game hosted */
struct delta_announce
{
	/* the invite's 64 hex digits (the invite link without its "halo://join/") */
	char invite[DELTA_INVITE_DIGITS + 1];
	char name[DELTA_NAME_SIZE + 1];
	char map[DELTA_MAP_SIZE + 1];
	int engine;
	int players, maximum_players;
	int open, teams;
	int version;
};

/* the body of POST /v1/announce (application/x-www-form-urlencoded) for it:
the fields the list requires and the game's own, and none of the optional
host fields (platform, architecture, build, capabilities, machines, roster).
Returns its length, or 0 if it does not fit in size */
int delta_announce_form(char *form, int size, const struct delta_announce *announce);
/* the body of POST /v1/withdraw for an invite */
int delta_withdraw_form(char *form, int size, const char *invite);

/* an answer to an announcement: 1 for "ok <seconds>" (with the seconds the
listing lasts, 0 if not given), else 0 */
int delta_announce_answer(const char *text, int *seconds);

/* when announcing goes on: ---------------------------------------- */

enum
{
	/* the list drops a game not announced for 75 seconds: announced again
	well before, and sooner when the game changes (not sooner than the
	minimum) */
	DELTA_ANNOUNCE_INTERVAL = 60000,
	DELTA_ANNOUNCE_MINIMUM = 15000,
	/* after a failure: this long, doubled with each one in a row up to the
	maximum; a refusal (403: the list could not reach the game, 429) waits
	the longer of it and DELTA_REFUSED_WAIT */
	DELTA_RETRY_WAIT = 30000,
	DELTA_RETRY_MAXIMUM = 300000,
	DELTA_REFUSED_WAIT = 120000,
};

enum
{
	DELTA_STEP_NOTHING,
	DELTA_STEP_ANNOUNCE,
	DELTA_STEP_WITHDRAW,
};

struct delta_announce_state
{
	/* the invite listed now, if any */
	int listed;
	char invite[DELTA_INVITE_DIGITS + 1];
	/* the details last announced, and when (milliseconds of a monotonic
	clock) */
	struct delta_announce sent;
	unsigned long sent_time;
	/* failures in a row, and no attempt before this (0: none waited for) */
	int failures;
	unsigned long wait_until;
};

/* whether a game's setup is final enough to list: it has a name, a map (any: Derelict's is "carousel") and a
game type from 1 to 6. Game type 0 is no multiplayer game (the game's `game_engine_index` is 0 for co-op and
for a game not yet set up, which reports its default map), so such a game is not announced; one already
listed is left as it is, not withdrawn, until the game says it is gone */
int delta_announce_ready(const struct delta_announce *announce);
/* what to do now: wanted is the game to be listed (NULL: none, and a game not delta_announce_ready is
none; a game not delta_announce_ready is neither announced nor, if it is the listed one, withdrawn). Withdraw
first the listing of another invite, then announce. The caller reports what
came of it with delta_announce_done / delta_withdraw_done */
int delta_announce_step(const struct delta_announce_state *state, const struct delta_announce *wanted,
	unsigned long now);
/* an announcement was answered: status is the HTTP status (0: no answer) and
ok whether the answer was "ok" */
void delta_announce_done(struct delta_announce_state *state, const struct delta_announce *announce, int status,
	int ok, unsigned long now);
void delta_withdraw_done(struct delta_announce_state *state);

#endif
