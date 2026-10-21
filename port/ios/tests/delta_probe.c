/* the Delta game list (delta_list.c, halo_network_join.h): an answer read as /v1/games gives it, what is left out
   of it, malformed and hostile answers, the announcement's form, the answer to it, and when announcing goes on */
#include "delta_list.h"
#include "halo_network_join.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define INVITE_A "567f7f1ccc06b6e1f15ba7477f947f9e71d231a69db193503a02d1684e47269a"
#define INVITE_B "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"

static const char answer[] =
	"{\"games\":[\n"
	"{\"invite\":\"" INVITE_A "\",\"name\":\". SWAT pistols\",\"map\":\"hangemhigh\",\"gametype\":\"SWAT\",\"engine\":2,"
	"\"players\":26,\"maximum_players\":128,\"open\":1,\"in_progress\":1,\"version\":32,\"score_limit\":0,\"teams\":0,"
	"\"roster\":[],\"verified\":true,\"age\":3,\"region\":\"\",\"mode\":\"multiplayer\",\"counted\":true,"
	"\"source\":\"broker\",\"platform\":\"\",\"machines\":{},\"locked\":false},\n"
	"{\"invite\":\"" INVITE_B "\",\"name\":\"Caf\\u00e9 \\\"Slayer\\\"\\n\",\"map\":\"levels\\\\test\\\\bloodgulch\\\\bloodgulch@ce\","
	"\"engine\":1,\"players\":3,\"maximum_players\":16,\"open\":0,\"version\":29,\"teams\":1,"
	"\"roster\":[{\"team\":0,\"name\":\"a\"},{\"team\":1,\"name\":\"b\"}],\"verified\":true,\"locked\":false,"
	"\"machines\":{\"pc_linux\":2},\"future\":[1,2,{\"x\":[[]]}]},\n"
	"{\"invite\":\"\",\"name\":\"locked game\",\"map\":\"m\",\"version\":29,\"locked\":true},\n"
	"{\"invite\":\"" INVITE_A "\",\"name\":\"unverified\",\"map\":\"m\",\"version\":29,\"verified\":false},\n"
	"{\"invite\":\"" INVITE_A "\",\"name\":\"campaign\",\"map\":\"a10\",\"version\":29,\"mode\":\"coop\"},\n"
	"{\"invite\":\"" INVITE_A "0\",\"name\":\"too long\",\"map\":\"m\",\"version\":29},\n"
	"{\"invite\":\"short\",\"name\":\"old invite\",\"map\":\"m\",\"version\":22},\n"
	"{\"invite\":\"" INVITE_A "\",\"name\":\"\",\"map\":\"m\",\"version\":29},\n"
	"{\"invite\":\"" INVITE_A "\",\"name\":\"odd types\",\"map\":7,\"players\":\"many\",\"version\":29,\"open\":true}\n"
	"],\"extra\":\"x\"}";

static int parse(const char *text, struct delta_game *games, int maximum)
{
	return delta_parse_games(text, (int)strlen(text), games, maximum);
}

int main(void)
{
	static struct delta_game games[DELTA_MAXIMUM_GAMES];
	struct delta_announce announce;
	struct delta_announce_state state;
	char form[512];
	char deep[200];
	int count, seconds, index;

	/* the joinable versions: this build's, and the older ones back to the oldest joinable */
	assert(HALO_PORT_NETWORK_VERSION == 32 && HALO_PORT_NETWORK_OLDEST_JOINABLE == 29);
	assert(halo_network_joinable(32) && halo_network_joinable(31) && halo_network_joinable(29));
	assert(!halo_network_joinable(28) && !halo_network_joinable(33) && !halo_network_joinable(0) &&
		!halo_network_joinable(-1) && !halo_network_joinable(11));

	/* the answer: the first, second and last games are offered; locked, unverified, co-op, short-invite and nameless
	ones are not */
	count = parse(answer, games, DELTA_MAXIMUM_GAMES);
	assert(count == 3);
	assert(!strcmp(games[0].invite, INVITE_A) && !strcmp(games[0].name, ". SWAT pistols"));
	assert(!strcmp(games[0].map, "hangemhigh") && !strcmp(games[0].gametype, "SWAT"));
	assert(games[0].engine == 2 && games[0].players == 26 && games[0].maximum_players == 128);
	assert(games[0].open && games[0].in_progress && games[0].version == 32 && !games[0].teams);
	/* the invite in lowercase, the name's accent a "?", its quotes kept, its newline a space-free gap, the map's
	path and family gone */
	assert(!strcmp(games[1].invite, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
	assert(!strcmp(games[1].name, "Caf? \"Slayer\""));
	assert(!strcmp(games[1].map, "bloodgulch") && games[1].teams && !games[1].open && games[1].version == 29);
	assert(games[1].engine == 1 && games[1].players == 3 && games[1].maximum_players == 16);
	/* a field of the wrong type is left at its default, and the game kept */
	assert(!strcmp(games[2].name, "odd types") && games[2].players == 0 && games[2].map[0] == 0 && games[2].open);
	assert(games[2].maximum_players == 0);

	/* no more than asked for */
	assert(parse(answer, games, 2) == 2);
	/* nothing, and the same with whitespace */
	assert(parse("{\"games\":[]}", games, 8) == 0);
	assert(parse(" \n{ \"games\" : [ ] } \n", games, 8) == 0);
	assert(parse("{\"other\":1,\"games\":[]}", games, 8) == 0);

	/* not the list */
	assert(parse("", games, 8) == -1);
	assert(parse("{}", games, 8) == -1);
	assert(parse("{\"other\":[]}", games, 8) == -1);
	assert(parse("[]", games, 8) == -1);
	assert(parse("{\"games\":{}}", games, 8) == -1);
	assert(parse("{\"games\":[}", games, 8) == -1);
	assert(parse("{\"games\":[] trailing", games, 8) == -1);
	assert(parse("{\"games\":[]} x", games, 8) == -1);
	assert(parse("{\"games\":[{\"invite\":\"" INVITE_A "\"", games, 8) == -1);
	assert(parse("{\"games\":[{\"invite\":\"" INVITE_A, games, 8) == -1);
	assert(parse("{\"games\":[1]}", games, 8) == -1);
	assert(parse("{\"games\":[{\"name\":\"\\x\"}]}", games, 8) == -1);
	assert(parse("{\"games\":[{\"name\":\"\\u12\"}]}", games, 8) == -1);
	assert(parse("<html>502 Bad Gateway</html>", games, 8) == -1);
	assert(delta_parse_games(NULL, 0, games, 8) == -1);
	assert(delta_parse_games(answer, 0, games, 8) == -1);
	assert(delta_parse_games(answer, DELTA_MAXIMUM_ANSWER + 1, games, 8) == -1);
	/* cut off anywhere, it is never read as a list (and never read past its end) */
	for (index = 0; index < (int)strlen(answer); index++)
		assert(delta_parse_games(answer, index, games, DELTA_MAXIMUM_GAMES) == -1);

	/* nested too deeply */
	memset(deep, '[', sizeof(deep) - 1);
	deep[sizeof(deep) - 1] = 0;
	{
		char text[400];

		snprintf(text, sizeof(text), "{\"games\":[{\"x\":%s}]}", deep);
		assert(parse(text, games, 8) == -1);
	}

	/* the announcement */
	memset(&announce, 0, sizeof(announce));
	strcpy(announce.invite, INVITE_A);
	strcpy(announce.name, "Fun & games = 100%");
	strcpy(announce.map, "bloodgulch");
	announce.engine = 2;
	announce.players = 3;
	announce.maximum_players = 16;
	announce.open = 1;
	announce.teams = 0;
	announce.version = HALO_PORT_NETWORK_VERSION;
	assert(delta_announce_form(form, sizeof(form), &announce) > 0);
	assert(!strcmp(form, "invite=" INVITE_A "&name=Fun%20%26%20games%20%3D%20100%25&map=bloodgulch&engine=2&players=3"
		"&maximum_players=16&open=1&version=32&teams=0&delta=0"));
	assert(delta_announce_form(form, 20, &announce) == 0);
	assert(delta_announce_form(form, 1, &announce) == 0);
	assert(delta_withdraw_form(form, sizeof(form), INVITE_A) == (int)strlen("invite=" INVITE_A));
	assert(!strcmp(form, "invite=" INVITE_A));
	assert(delta_withdraw_form(form, 10, INVITE_A) == 0);

	/* the answer to it */
	assert(delta_announce_answer("ok 150", &seconds) && seconds == 150);
	assert(delta_announce_answer("ok", &seconds) && seconds == 0);
	assert(delta_announce_answer("ok 90\n", &seconds) && seconds == 90);
	assert(!delta_announce_answer("okay", &seconds) && !delta_announce_answer("403 forbidden", &seconds));
	assert(!delta_announce_answer("", &seconds));

	/* a game still being set up (game type 0, no map) is not listed, and one listed that goes back to it is
	withdrawn */
	announce.engine = 0;
	assert(!delta_announce_ready(&announce));
	memset(&state, 0, sizeof(state));
	assert(delta_announce_step(&state, &announce, 1000) == DELTA_STEP_NOTHING);
	announce.engine = 7;
	assert(!delta_announce_ready(&announce) && delta_announce_step(&state, &announce, 1000) == DELTA_STEP_NOTHING);
	announce.engine = 2;
	announce.map[0] = 0;
	assert(!delta_announce_ready(&announce) && delta_announce_step(&state, &announce, 1000) == DELTA_STEP_NOTHING);
	strcpy(announce.map, "bloodgulch");
	assert(delta_announce_ready(&announce) && delta_announce_step(&state, &announce, 1000) == DELTA_STEP_ANNOUNCE);
	delta_announce_done(&state, &announce, 200, 1, 1000);
	announce.engine = 0;
	assert(delta_announce_step(&state, &announce, 1500) == DELTA_STEP_WITHDRAW);
	announce.engine = 2;

	/* when it goes on: nothing wanted, nothing done */
	memset(&state, 0, sizeof(state));
	assert(delta_announce_step(&state, NULL, 1000) == DELTA_STEP_NOTHING);
	/* a game wanted: announced; a failure waits, a refusal longer, and a second in a row longer still */
	assert(delta_announce_step(&state, &announce, 1000) == DELTA_STEP_ANNOUNCE);
	delta_announce_done(&state, &announce, 0, 0, 1000);
	assert(delta_announce_step(&state, &announce, 1000 + DELTA_RETRY_WAIT - 1) == DELTA_STEP_NOTHING);
	assert(delta_announce_step(&state, &announce, 1000 + DELTA_RETRY_WAIT) == DELTA_STEP_ANNOUNCE);
	delta_announce_done(&state, &announce, 403, 0, 50000);
	assert(state.failures == 2);
	assert(delta_announce_step(&state, &announce, 50000 + DELTA_REFUSED_WAIT - 1) == DELTA_STEP_NOTHING);
	assert(delta_announce_step(&state, &announce, 50000 + DELTA_REFUSED_WAIT) == DELTA_STEP_ANNOUNCE);
	for (index = 0; index < 20; index++)
		delta_announce_done(&state, &announce, 500, 0, 200000);
	assert(delta_announce_step(&state, &announce, 200000 + DELTA_RETRY_MAXIMUM) == DELTA_STEP_ANNOUNCE);
	assert(delta_announce_step(&state, &announce, 200000 + DELTA_RETRY_MAXIMUM - 1) == DELTA_STEP_NOTHING);
	/* listed: announced again at the interval, sooner when the game changed (not before the minimum), never when
	nothing changed */
	delta_announce_done(&state, &announce, 200, 1, 1000000);
	assert(state.listed && state.failures == 0 && !strcmp(state.invite, INVITE_A));
	assert(delta_announce_step(&state, &announce, 1000000 + DELTA_ANNOUNCE_INTERVAL - 1) == DELTA_STEP_NOTHING);
	assert(delta_announce_step(&state, &announce, 1000000 + DELTA_ANNOUNCE_INTERVAL) == DELTA_STEP_ANNOUNCE);
	announce.players = 4;
	assert(delta_announce_step(&state, &announce, 1000000 + DELTA_ANNOUNCE_MINIMUM - 1) == DELTA_STEP_NOTHING);
	assert(delta_announce_step(&state, &announce, 1000000 + DELTA_ANNOUNCE_MINIMUM) == DELTA_STEP_ANNOUNCE);
	/* the clock wrapping round does not stop it */
	delta_announce_done(&state, &announce, 200, 1, 0xFFFFFFFFul - 1000);
	assert(delta_announce_step(&state, &announce, 0xFFFFFFFFul - 1000 + DELTA_ANNOUNCE_INTERVAL) == DELTA_STEP_ANNOUNCE);
	/* the game gone, or on another invite: withdrawn first, whatever the wait; then the new one announced */
	assert(delta_announce_step(&state, NULL, 0xFFFFFFFFul - 1000 + 5) == DELTA_STEP_WITHDRAW);
	strcpy(announce.invite, INVITE_B);
	assert(delta_announce_step(&state, &announce, 0xFFFFFFFFul - 1000 + 5) == DELTA_STEP_WITHDRAW);
	delta_withdraw_done(&state);
	assert(!state.listed && delta_announce_step(&state, &announce, 0xFFFFFFFFul - 1000 + 5) == DELTA_STEP_ANNOUNCE);
	assert(delta_announce_step(&state, NULL, 0xFFFFFFFFul - 1000 + 5) == DELTA_STEP_NOTHING);

	printf("delta probe ok\n");
	return 0;
}
