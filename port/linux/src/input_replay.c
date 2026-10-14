/*
INPUT_REPLAY.C

Recording and replaying controller 1 (port 0), and a benchmark that times
the frames of a replay, so the same stretch of the game plays the same way
on every build (debug.input_record, debug.input_replay, debug.benchmark).

A recording is the controller's state each time it changes, stamped with the
game time: ticks since the map loaded, plus the fraction into the next tick.
A replay at 45 frames a second and one at 90 then press the same buttons at
the same moments of the game. Only reads while a level's game time runs are
recorded; before that (menus, including the main menu's own map, and
loading) the real controller drives the game. The game time goes back when
the game reverts to a checkpoint (skipping a cinematic does, as does dying)
or loads the next level; the recorded time carries on from where it was
instead, the same way on recording and replay, so a recording can span
them. During a
replay the real controller is ignored while the game time runs. The look
stick turns the view by its deflection times each frame's length, so aim
can differ slightly between replays at different frame rates.

The file is text: the header line "halo input recording 1", then one line
per change: game time in ticks, the digital buttons (hex), the 8 analog
buttons and the 4 stick axes. A name without a slash is a file in the
current directory (the app's Documents on iOS), with ".input" added.

The controller alone doesn't replay a route exactly: the game turns the look
stick into facing frame by frame, and frames fall differently on every run.
So each tick's action for player 1 (what the simulation itself takes: facing,
movement, buttons, trigger, weapon, grenade and zoom) is recorded too, bit
for bit, to NAME.actions, and a replay hands the recorded action to each
tick in place of the one built from the controller (update_client_dequeue,
source/game/player_queues_new.c). The controller replay still drives what
reads the controller directly, such as skipping a cinematic. Actions are
keyed by segment and game tick: a segment ends whenever the game time goes
back (a checkpoint revert, the next level), so a replay whose revert comes a
frame later than the recording's still lines up. Its header is "halo action
recording 1", plus " stereo=head" when display.stereo wasn't off (head, screen
or side_by_side; an older header reads as off), then one line per tick:
segment, tick and the action's eight 32-bit words in hex. A replay should run
with display.direct_camera off: the live camera follows the real stick, not
the recorded facing.

With debug.benchmark, a replay's frames are timed from the first recorded
state to the last; then the report goes to benchmark-<name>-<time>.txt next
to the recording, a summary to the log, and the game quits.
*/

#include "platform.h"
#include "port_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* source/game/game_time.c */
extern unsigned char game_time_initialized(void);
extern long game_time_get(void);
extern float game_time_get_tick_fraction(void);
extern unsigned char game_time_get_paused(void);
/* port/linux/game/input_replay_scenario.c */
extern unsigned char input_replay_main_menu_loaded(void);
extern unsigned char input_replay_skip_cinematic(void);
extern void input_replay_level(char *map, unsigned int size, long *difficulty, long *players, unsigned long *seed);
/* the game's global random seed; set to *seed first when seed is given */
extern unsigned long input_replay_random_seed(const unsigned long *seed);
/* both random sequences' seeds, global then local: read, or set */
extern void input_replay_random_seeds(unsigned long seeds[2], unsigned char set);
/* xbox_kernel.c */
double halo_frame_trace_milliseconds(void);

#define RECORDING_HEADER "halo input recording 1"
/* how long after the last recorded state a replay ends, in ticks */
#define REPLAY_TAIL_TICKS 30.0

#define ACTION_HEADER "halo action recording 1"
/* a cinematic the replay skipped whose revert has not shown as a step back in this many ticks did not step back:
the segment begins anyway */
#define SKIP_REVERT_TICKS 90
/* struct player_action (source/game/players.h) is 0x20 bytes */
#define ACTION_WORDS 8

struct recorded_action
{
	long segment, tick;
	unsigned int words[ACTION_WORDS];
	/* from the recording's .seeds file, when it has one: the two random seeds as this tick began */
	unsigned long seeds[2];
	BOOL has_seeds;
};

/* A recording's third file, NAME.seeds, beside NAME.input and NAME.actions (which keep their form, so
recordings made before it read as they did): the line "halo seed recording 1", then for each tick an
action was recorded for, in the same order, its segment, its tick and the game's two random seeds as the
tick began, global then local, in hex. What enemies choose depends on them, and frames drawn between
ticks draw from them too, so two runs with the same seed at a take's first tick part once the frame
rates differ; a replay that sets both before each tick's action keeps the recording's. */
#define SEEDS_HEADER "halo seed recording 1"

struct recorded_state
{
	double ticks;
	XINPUT_GAMEPAD pad;
};

static struct
{
	BOOL configured;
	char record_path[512], replay_path[512], replay_name[256];
	FILE *record_file;
	BOOL record_ended;
	/* the game time last seen, and what's added to it to make the recorded
	time, which never goes back */
	double last_game_ticks, timeline_offset;
	XINPUT_GAMEPAD last_pad;
	struct recorded_state *states;
	long state_count, next_state;
	XINPUT_GAMEPAD replay_pad;
	BOOL replay_finished;
	BOOL benchmark;
	/* the device benchmark's suite (benchmark_suite.c) owns the replays: several in one launch, started with
	input_replay_begin; it times the frames itself, and the real controller is ignored throughout */
	BOOL suite;
	/* a replay begun while another level still plays waits for the next level: until the game time is seen to go
	back, nothing is replayed and no segment is counted */
	BOOL await_level;
	double await_ticks;
	double *frame_milliseconds;
	long frame_count, frame_capacity;
	double previous_frame;
	double started;
	/* each tick's action: the file being written, or the recording being
	replayed; the segment and the game tick of the last action seen */
	char record_actions_path[512], replay_actions_path[512];
	FILE *actions_file, *seeds_file;
	struct recorded_action *actions;
	long action_count, next_action;
	long segment, last_action_tick;
	/* tick actions written to the take being recorded */
	long actions_written;
	/* a replayed take's .route: the seed each of its segments began with, and whether the take began after the
	level's opening was skipped (its first tick is past 0), which the replay must do first */
	struct { long segment, tick; unsigned long seed; BOOL applied; } segment_seeds[32];
	int segment_seed_count;
	BOOL preroll, preroll_timeline;
	/* the replay itself skipped a cinematic and the revert that follows has not been seen yet. The revert is a
	segment's beginning even when the game time does not go back by it: a cinematic skipped on the level's first
	tick reverts to that same tick (a fast load, or debug.fixed_timestep), which the tick alone cannot show */
	BOOL skip_pending;
	long skip_tick;
	/* the game time's jumps back seen frame by frame (the ticks' own
	segment count can miss one: a cinematic can stop the player updates
	right after its checkpoint), when the last came, and when the game
	time last moved */
	long jumps;
	double jumped, moved;
} replay;

/* NAME.input's actions file, NAME.actions */
static void actions_path(const char *input_path, char *path, size_t size)
{
	size_t length = strlen(input_path);

	if (length > 6 && !strcmp(input_path + length - 6, ".input"))
		length -= 6;
	snprintf(path, size, "%.*s.actions", (int)length, input_path);
}

static void actions_load(void)
{
	FILE *file = fopen(replay.replay_actions_path, "r");
	char line[256];
	long capacity = 0;

	if (!file)
	{
		platform_log("debug.input_replay: no %s; the controller alone drives the replay", replay.replay_actions_path);
		return;
	}
	if (!fgets(line, sizeof(line), file) || strncmp(line, ACTION_HEADER, strlen(ACTION_HEADER)))
	{
		platform_log("debug.input_replay: %s isn't an action recording", replay.replay_actions_path);
		fclose(file);
		return;
	}
	{
		/* the stereo mode the recording was made in; an older header has none, which reads as off */
		char recorded[32] = "off";
		const char *tag = strstr(line, " stereo=");
		const char *run = config_string("display.stereo");

		if (tag)
			sscanf(tag + strlen(" stereo="), "%31s", recorded);
		if (!run || !*run)
			run = "off";
		/* a10's script flow differs only when one mode skips its look-inversion test (head, side_by_side) and the other doesn't */
		if ((!strcmp(recorded, "head") || !strcmp(recorded, "side_by_side")) !=
			(!strcmp(run, "head") || !strcmp(run, "side_by_side")))
			platform_log("debug.input_replay: warning: %s was recorded with display.stereo \"%s\" and this run uses \"%s\"; a10's script flow differs (its look-inversion test runs only when the mode isn't head or side_by_side)",
				replay.replay_actions_path, recorded, run);
	}
	while (fgets(line, sizeof(line), file))
	{
		struct recorded_action action = { 0 };
		unsigned int *w = action.words;

		if (sscanf(line, "%ld %ld %x %x %x %x %x %x %x %x", &action.segment, &action.tick,
			&w[0], &w[1], &w[2], &w[3], &w[4], &w[5], &w[6], &w[7]) != 2 + ACTION_WORDS)
			continue;
		if (replay.action_count == capacity)
		{
			capacity = capacity ? capacity * 2 : 4096;
			replay.actions = realloc(replay.actions, (size_t)capacity * sizeof(*replay.actions));
		}
		replay.actions[replay.action_count++] = action;
	}
	fclose(file);
	platform_log("debug.input_replay: %ld tick actions from %s", replay.action_count, replay.replay_actions_path);
}

static int compare_action_keys(const struct recorded_action *action, long segment, long tick);

/* the recording's .seeds, when it has one: each line's seeds go to the action of the same segment and tick
(the two files are written together, so in step; a line that matches no action is skipped) */
static void seeds_load(void)
{
	char path[512], line[128];
	size_t length = strlen(replay.replay_path);
	FILE *file;
	long index = 0, matched = 0, segment, tick;
	unsigned long global, local;

	if (!replay.action_count || length < 6 || strcmp(replay.replay_path + length - 6, ".input"))
		return;
	snprintf(path, sizeof(path), "%.*s.seeds", (int)(length - 6), replay.replay_path);
	if (!(file = fopen(path, "r")))
		return;
	if (fgets(line, sizeof(line), file) && !strncmp(line, SEEDS_HEADER, strlen(SEEDS_HEADER)))
	{
		while (fgets(line, sizeof(line), file) && index < replay.action_count)
		{
			if (sscanf(line, "%ld %ld %lx %lx", &segment, &tick, &global, &local) != 4)
				continue;
			while (index < replay.action_count && compare_action_keys(&replay.actions[index], segment, tick) < 0)
				index++;
			if (index < replay.action_count && !compare_action_keys(&replay.actions[index], segment, tick))
			{
				replay.actions[index].seeds[0] = global;
				replay.actions[index].seeds[1] = local;
				replay.actions[index].has_seeds = TRUE;
				matched++;
			}
		}
	}
	fclose(file);
	platform_log("debug.input_replay: %ld of %ld ticks have the recording's random seeds (%s): set before each tick's action",
		matched, replay.action_count, path);
}

/* A replayed take's .route, when it has one (an automatic take): the seed each segment began with, and
whether the take's first tick is past the level's own. Such a take was opened after the player skipped
the level's opening cinematic, which reverts the game to its first tick: the replay starts one segment
early, with no actions, so that it skips the cinematic as soon as it can (input_replay_tick_action) and
the take's first segment begins at that revert, with the take's seed. */
static void route_load(void)
{
	char path[512], line[256];
	size_t length = strlen(replay.replay_path);
	FILE *file;
	long first_tick = 0;
	unsigned long seed;
	long segment, tick;

	if (length < 6 || strcmp(replay.replay_path + length - 6, ".input"))
		return;
	snprintf(path, sizeof(path), "%.*s.route", (int)(length - 6), replay.replay_path);
	if (!(file = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), file))
	{
		if (sscanf(line, "first_tick = %ld", &tick) == 1)
			first_tick = tick;
		else if (sscanf(line, "random_seed = %lx", &seed) == 1 && replay.segment_seed_count < 32)
		{
			/* (the take's own: noted at its first tick, which a first_tick line before or after this one gives) */
			replay.segment_seeds[replay.segment_seed_count].segment = 0;
			replay.segment_seeds[replay.segment_seed_count].tick = -1;
			replay.segment_seeds[replay.segment_seed_count++].seed = seed;
		}
		else if (sscanf(line, "segment_seed = %ld %ld %lx", &segment, &tick, &seed) == 3 && replay.segment_seed_count < 32)
		{
			replay.segment_seeds[replay.segment_seed_count].segment = segment;
			replay.segment_seeds[replay.segment_seed_count].tick = tick;
			replay.segment_seeds[replay.segment_seed_count++].seed = seed;
		}
	}
	fclose(file);
	{
		int index;

		for (index = 0; index < replay.segment_seed_count; index++)
		{
			if (replay.segment_seeds[index].tick < 0)
				replay.segment_seeds[index].tick = first_tick;
		}
	}
	if (first_tick > 0)
	{
		replay.preroll = replay.preroll_timeline = TRUE;
		replay.segment = -1;
	}
	platform_log("debug.input_replay: %s: %d segment seed%s%s", path, replay.segment_seed_count,
		replay.segment_seed_count == 1 ? "" : "s", replay.preroll ? "; the take begins after the level's opening was "
		"skipped (first_tick past 0), so the replay skips it first" : "");
}

/* A replay's tick, before its action: when the recording noted the random seed at this segment and
tick (a take's first tick, and each segment's first after a revert), the game continues from that seed.
At the tick itself and not at the revert: a revert can land a tick before the one the recording noted
(a take that began at tick 1 replayed from a revert to tick 0), and that tick's own random numbers
would leave the seed elsewhere. */
static void replay_seed(long tick)
{
	int index;

	for (index = 0; index < replay.segment_seed_count; index++)
	{
		if (!replay.segment_seeds[index].applied && replay.segment_seeds[index].segment == replay.segment &&
			tick >= replay.segment_seeds[index].tick)
		{
			replay.segment_seeds[index].applied = TRUE;
			input_replay_random_seed(&replay.segment_seeds[index].seed);
			platform_log("debug.input_replay: segment %ld, tick %ld: the recording's seed %08lx (noted at tick %ld)",
				replay.segment, tick, replay.segment_seeds[index].seed, replay.segment_seeds[index].tick);
		}
	}
}

/* a replay's segment begins (the game time went back) */
static void replay_segment_began(long tick)
{
	(void)tick;
	if (!replay.action_count)
		return;
	if (replay.preroll && replay.segment == 0)
	{
		/* the take's own time begins here: its controller states are timed from this revert (game_ticks
		starts the timeline over when it sees the same revert) */
		replay.preroll = FALSE;
		replay.next_state = 0;
	}
}

static void recording_path(const char *name, char *path, size_t size)
{
	if (strchr(name, '/'))
		snprintf(path, size, "%s", name);
	else
		snprintf(path, size, "%s.input", name);
}

static void replay_load(void)
{
	FILE *file = fopen(replay.replay_path, "r");
	char line[256];
	long capacity = 0;

	if (!file)
	{
		platform_log("debug.input_replay: cannot open %s", replay.replay_path);
		replay.replay_finished = TRUE;
		return;
	}
	if (!fgets(line, sizeof(line), file) || strncmp(line, RECORDING_HEADER, strlen(RECORDING_HEADER)))
	{
		platform_log("debug.input_replay: %s isn't an input recording", replay.replay_path);
		fclose(file);
		replay.replay_finished = TRUE;
		return;
	}
	while (fgets(line, sizeof(line), file))
	{
		struct recorded_state state;
		unsigned int buttons, analog[8];
		int sticks[4];
		int index;

		memset(&state, 0, sizeof(state));
		if (sscanf(line, "%lf %x %u %u %u %u %u %u %u %u %d %d %d %d", &state.ticks, &buttons,
			&analog[0], &analog[1], &analog[2], &analog[3], &analog[4], &analog[5], &analog[6], &analog[7],
			&sticks[0], &sticks[1], &sticks[2], &sticks[3]) != 14)
			continue;
		state.pad.wButtons = (WORD)buttons;
		for (index = 0; index < 8; index++)
			state.pad.bAnalogButtons[index] = (BYTE)analog[index];
		state.pad.sThumbLX = (SHORT)sticks[0];
		state.pad.sThumbLY = (SHORT)sticks[1];
		state.pad.sThumbRX = (SHORT)sticks[2];
		state.pad.sThumbRY = (SHORT)sticks[3];
		if (replay.state_count == capacity)
		{
			capacity = capacity ? capacity * 2 : 1024;
			replay.states = realloc(replay.states, (size_t)capacity * sizeof(*replay.states));
		}
		replay.states[replay.state_count++] = state;
	}
	fclose(file);
	platform_log("debug.input_replay: %ld states from %s, %.1f s of game time", replay.state_count,
		replay.replay_path, replay.state_count ? replay.states[replay.state_count - 1].ticks / 30.0 : 0.0);
	if (!replay.state_count)
		replay.replay_finished = TRUE;
}

/* ---------- automatic takes

debug.input_record = "auto": every level played is recorded from its load,
with nothing for the player to do. A take is three files in the data
folder's recordings folder (the host makes it), named for the map and the
moment of the load in UTC:

	recordings/take-a10-20261014-031502.input     the controller, as above
	recordings/take-a10-20261014-031502.actions   each tick's action, as above
	recordings/take-a10-20261014-031502.route     what a replay needs to start alike, as "key = value" lines

The .route is written when the take opens (the map, the difficulty, the
first tick, the random seed, the players, the build and the device
profile's settings in force) and gets its last lines when it closes
(closed_by, segments, last_tick, seconds). A take opens at a level's first
tick of game time and closes when another map loads, the same map starts
over (the game time back under a second), or the menu returns. Dying does
not close it: the revert to a checkpoint is a new segment, as in any
recording. Quitting the app or the system suspending it closes nothing, so
such a take's .route has no closed_by; its other two files are whole at
every moment, each line being flushed as written.

A take whose first_tick is not the level's start (a saved game continued)
replays nowhere else: entered says "saved" and tools leave it out. One
shorter than TAKE_MINIMUM_TICKS is deleted when it closes. */

#define TAKE_MINIMUM_TICKS (30 * 30)
/* a level's own start, against a saved game's first tick */
#define TAKE_START_TICKS 30

static struct
{
	BOOL automatic, open, players_written, paused;
	char map[32], base[256];
	long first_tick, last_tick, pauses, route_written_tick;
	/* the .route's lines so far, without the closing ones: the file is written whole from it */
	char route[4096];
} take;

/* The take's .route, whole: what was noted at its opening and since, then the closing lines as
they stand now. Written again every TAKE_ROUTE_TICKS, at a pause and at the close, so a take whose
app is quit or suspended (which the game never hears of) still has them, at most that old, with
closed_by = open. */
#define TAKE_ROUTE_TICKS 300

static void take_route_write(const char *closed_by)
{
	char path[512];
	FILE *route;

	snprintf(path, sizeof(path), "%s.route", take.base);
	if (!(route = fopen(path, "w")))
		return;
	fprintf(route, "%sclosed_by = %s\nsegments = %ld\npauses = %ld\nlast_tick = %ld\nseconds = %.1f\n", take.route,
		closed_by, replay.segment + 1, take.pauses, take.last_tick, (double)replay.actions_written / 30.0);
	fclose(route);
	take.route_written_tick = take.last_tick;
}

static void take_route_note(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void take_route_note(const char *format, ...)
{
	size_t length = strlen(take.route);
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(take.route + length, sizeof(take.route) - length, format, arguments);
	va_end(arguments);
}

static void take_close(const char *why)
{
	char path[512];

	if (!take.open)
		return;
	take.open = FALSE;
	if (replay.record_file)
		fclose(replay.record_file);
	if (replay.actions_file)
		fclose(replay.actions_file);
	if (replay.seeds_file)
		fclose(replay.seeds_file);
	replay.record_file = replay.actions_file = replay.seeds_file = NULL;
	replay.record_path[0] = replay.record_actions_path[0] = 0;
	if (replay.actions_written < TAKE_MINIMUM_TICKS)
	{
		static const char *const extensions[] = { "input", "actions", "route", "seeds" };
		int index;

		for (index = 0; index < 4; index++)
		{
			snprintf(path, sizeof(path), "%s.%s", take.base, extensions[index]);
			remove(path);
		}
		platform_log("debug.input_record: take %s was %ld ticks (%s): too short to keep", take.base,
			replay.actions_written, why);
		return;
	}
	take_route_write(why);
	platform_log("debug.input_record: take %s closed (%s): %ld ticks, %ld segment%s", take.base, why,
		replay.actions_written, replay.segment + 1, replay.segment ? "s" : "");
}

static void take_open(const char *map, long tick, long difficulty, long players, unsigned long seed)
{
	static const char *const difficulties[] = { "easy", "normal", "hard", "impossible" };
	char settings[2048];
	const char *build = getenv("HALO_APP_BUILD");
	time_t now = time(NULL);
	struct tm utc;

	gmtime_r(&now, &utc);
	snprintf(take.map, sizeof(take.map), "%s", map);
	snprintf(take.base, sizeof(take.base), "recordings/take-%s-%04d%02d%02d-%02d%02d%02d", map, utc.tm_year + 1900,
		utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
	take.first_tick = take.last_tick = tick;
	take.players_written = take.paused = FALSE;
	take.pauses = 0;
	take.route[0] = 0;
	replay.actions_written = 0;
	/* a recording of its own: its time and its segments begin here */
	replay.record_ended = FALSE;
	replay.segment = 0;
	replay.last_action_tick = 0;
	replay.timeline_offset = 0.0;
	replay.last_game_ticks = 0.0;
	snprintf(replay.record_path, sizeof(replay.record_path), "%s.input", take.base);
	snprintf(replay.record_actions_path, sizeof(replay.record_actions_path), "%s.actions", take.base);
	take.open = TRUE;
	config_profile_settings_text(settings, sizeof(settings));
	(void)players;   /* (none yet at a level's first tick: written once the game has made them, take_update) */
	take_route_note("map = %s\ndifficulty = %s\nentered = %s\nfirst_tick = %ld\nrandom_seed = %08lx\n"
		"stereo = %s\nversion = %s\nrecorded = %04d-%02d-%02dT%02d:%02d:%02dZ\n%s", map,
		difficulty >= 0 && difficulty < 4 ? difficulties[difficulty] : "unknown", tick <= TAKE_START_TICKS ? "start" : "saved",
		tick, seed, config_string("display.stereo"), build && *build ? build : "unknown", utc.tm_year + 1900,
		utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, settings);
	take_route_write("open");
	platform_log("debug.input_record: take %s: %s on %s from tick %ld%s", take.base, map,
		difficulty >= 0 && difficulty < 4 ? difficulties[difficulty] : "unknown", tick,
		tick <= TAKE_START_TICKS ? "" : " (a saved game: this take replays nowhere else)");
}

/* at each poll of the controller and each tick: the take for the level being played */
static void take_update(void)
{
	char map[32];
	long difficulty, players, tick;
	unsigned long seed;

	if (!take.automatic)
		return;
	if (!game_time_initialized() || input_replay_main_menu_loaded())
	{
		take_close("menu");
		return;
	}
	input_replay_level(map, sizeof(map), &difficulty, &players, &seed);
	tick = game_time_get();
	/* (the game time going back, to the level's first tick included, is no new take: skipping a level's opening
	cinematic reverts to its start, as restarting the level does, and either is a segment a replay follows) */
	if (take.open && strcmp(map, take.map))
		take_close("level_change");
	if (!take.open)
		take_open(map, tick, difficulty, players, seed);
	if (take.open)
	{
		BOOL paused = game_time_get_paused() != 0;

		take.last_tick = tick;
		/* a pause stops the game time, so the take simply has no ticks for it; counted for the take's listing */
		if (!take.players_written && players > 0)
		{
			take_route_note("players = %ld\n", players);
			take.players_written = TRUE;
			take.route_written_tick = -TAKE_ROUTE_TICKS;
		}
		if (paused && !take.paused)
		{
			take.pauses++;
			take.route_written_tick = -TAKE_ROUTE_TICKS;
		}
		take.paused = paused;
		if (tick - take.route_written_tick >= TAKE_ROUTE_TICKS || tick < take.route_written_tick)
			take_route_write("open");
	}
}

static void configure(void)
{
	const char *record = config_string("debug.input_record");
	const char *replay_name = config_string("debug.input_replay");

	replay.configured = TRUE;
	if (!strcmp(record, "auto") && !*replay_name)
	{
		take.automatic = TRUE;
		platform_log("debug.input_record: every level is recorded from its load, to recordings/take-<map>-<time> in the "
			"data folder");
	}
	else if (*record)
	{
		recording_path(record, replay.record_path, sizeof(replay.record_path));
		actions_path(replay.record_path, replay.record_actions_path, sizeof(replay.record_actions_path));
		platform_log("debug.input_record: recording controller 1 to %s and its tick actions to %s", replay.record_path,
			replay.record_actions_path);
	}
	if (*replay_name)
	{
		const char *base = strrchr(replay_name, '/');

		snprintf(replay.replay_name, sizeof(replay.replay_name), "%s", base ? base + 1 : replay_name);
		recording_path(replay_name, replay.replay_path, sizeof(replay.replay_path));
		actions_path(replay.replay_path, replay.replay_actions_path, sizeof(replay.replay_actions_path));
		replay.benchmark = config_boolean("debug.benchmark");
		replay_load();
		actions_load();
		seeds_load();
		route_load();
	}
}

/* The device benchmark's suite: replays NAME (a path ending in ".input", or NULL or "" for none) from the next
level's first tick, in place of whatever was replaying. From the first call on, the real controller is ignored
while a level's game time runs. 1 if there is nothing to load or the recording loaded with at least one state */
int input_replay_begin(const char *name)
{
	replay.configured = TRUE;
	free(replay.states);
	free(replay.actions);
	if (replay.record_file)
		fclose(replay.record_file);
	if (replay.actions_file)
		fclose(replay.actions_file);
	if (replay.seeds_file)
		fclose(replay.seeds_file);
	memset(&replay, 0, sizeof(replay));
	replay.configured = replay.suite = TRUE;
	if (game_time_initialized() && !input_replay_main_menu_loaded())
	{
		replay.await_level = TRUE;
		replay.await_ticks = (double)game_time_get();
	}
	if (!name || !*name)
		return 1;
	{
		const char *base = strrchr(name, '/');

		snprintf(replay.replay_name, sizeof(replay.replay_name), "%s", base ? base + 1 : name);
	}
	recording_path(name, replay.replay_path, sizeof(replay.replay_path));
	actions_path(replay.replay_path, replay.replay_actions_path, sizeof(replay.replay_actions_path));
	replay_load();
	actions_load();
	/* (a take's .seeds, both random seeds at every tick; and its .route: the older segment seeds, and whether it
	began after its level's opening was skipped) */
	seeds_load();
	route_load();
	return replay.state_count > 0;
}

/* whether the replay begun last has played to its end (or never loaded) */
int input_replay_ended(void)
{
	return !*replay.replay_path || replay.replay_finished;
}

/* how many times the game time has gone back during this replay, and the recorded length in ticks */
long input_replay_segment(void)
{
	return replay.segment;
}

double input_replay_length_ticks(void)
{
	return replay.state_count ? replay.states[replay.state_count - 1].ticks : 0.0;
}

static double game_ticks(void);

/* whether a replay begun during another level is still waiting for its own (input_replay_begin) */
static BOOL awaiting_level(double ticks)
{
	if (!replay.await_level)
		return FALSE;
	if (ticks + 1.0 < replay.await_ticks)
	{
		replay.await_level = FALSE;
		return FALSE;
	}
	replay.await_ticks = ticks;
	return TRUE;
}

/* the recorded time now: ticks of game time since the level loaded, which never goes back; negative while no
level's game time runs */
double input_replay_timeline(void)
{
	return game_ticks();
}

/* the recorded time: the game time in ticks, with the fraction into the
next, carried on across the game time going back (a checkpoint revert or
the next level); or a negative number while no level's game time runs (the
main menu's doesn't count) */
static double game_ticks(void)
{
	double ticks;

	if (!game_time_initialized() || input_replay_main_menu_loaded())
		return -1.0;
	ticks = (double)game_time_get() + (double)game_time_get_tick_fraction();
	if (awaiting_level(ticks))
		return -1.0;
	if (replay.last_game_ticks > 0.0 && ticks + 1.0 < replay.last_game_ticks)
	{
		/* resume at the next whole tick after the last seen, so recording
		and replay, whose last frames before the jump fall at different
		fractions, agree */
		/* (a take replayed from before its own beginning, route_load: its time starts at this revert, which is
		not one of the recording's own) */
		if (replay.preroll_timeline)
		{
			replay.preroll_timeline = FALSE;
			replay.timeline_offset = 0.0;
			replay.next_state = 0;
			replay.last_game_ticks = ticks;
			platform_log("debug.input_replay: the level's opening was skipped; the take's time begins");
			return ticks;
		}
		replay.timeline_offset += ceil(replay.last_game_ticks) - ticks;
		/* a replay lets go of its buttons at the jump and drops the states
		recorded before it: a press held across the jump can't be released
		while the game time stands still, and an accept held into a cinematic
		that the revert made skippable again skips it every frame, so the game
		time never moves (c40's opening) */
		if (*replay.replay_path)
		{
			memset(&replay.replay_pad, 0, sizeof(replay.replay_pad));
			while (replay.next_state < replay.state_count &&
				replay.states[replay.next_state].ticks < ticks + replay.timeline_offset)
				replay.next_state++;
		}
		replay.jumps++;
		replay.jumped = halo_frame_trace_milliseconds();
		platform_log("debug.input_record/input_replay: the game time went back from %.2f to %.2f ticks; the recorded time carries on",
			replay.last_game_ticks, ticks);
	}
	if (ticks != replay.last_game_ticks)
		replay.moved = halo_frame_trace_milliseconds();
	replay.last_game_ticks = ticks;
	return ticks + replay.timeline_offset;
}

/* ends a recording: both files close, and nothing more is written */
static void record_end(const char *why)
{
	if (replay.record_file)
		fclose(replay.record_file);
	if (replay.actions_file)
		fclose(replay.actions_file);
	if (replay.seeds_file)
		fclose(replay.seeds_file);
	replay.record_file = replay.actions_file = replay.seeds_file = NULL;
	replay.record_ended = TRUE;
	replay.record_actions_path[0] = 0;
	platform_log("debug.input_record: %s; the recording ends", why);
}

static void record(const XINPUT_GAMEPAD *pad, double ticks)
{
	int index;

	if (replay.record_ended)
		return;
	if (!replay.record_file)
	{
		replay.record_file = fopen(replay.record_path, "w");
		if (!replay.record_file)
		{
			platform_log("debug.input_record: cannot write %s", replay.record_path);
			replay.record_ended = TRUE;
			return;
		}
		fprintf(replay.record_file, "%s\n", RECORDING_HEADER);
		memset(&replay.last_pad, 0xff, sizeof(replay.last_pad));
	}
	if (!memcmp(pad, &replay.last_pad, sizeof(*pad)))
		return;
	replay.last_pad = *pad;
	fprintf(replay.record_file, "%.4f %04x", ticks, (unsigned int)pad->wButtons);
	for (index = 0; index < 8; index++)
		fprintf(replay.record_file, " %u", (unsigned int)pad->bAnalogButtons[index]);
	fprintf(replay.record_file, " %d %d %d %d\n", pad->sThumbLX, pad->sThumbLY, pad->sThumbRX, pad->sThumbRY);
	fflush(replay.record_file);
}

static void play(XINPUT_GAMEPAD *pad, double ticks)
{
	WORD pressed = 0;
	BYTE analog[8] = { 0 };
	int index;

	/* a press that began and ended between two frames still reaches the
	game for one frame: the buttons pressed in any state passed this frame */
	while (replay.next_state < replay.state_count && replay.states[replay.next_state].ticks <= ticks)
	{
		replay.replay_pad = replay.states[replay.next_state++].pad;
		pressed |= replay.replay_pad.wButtons;
		for (index = 0; index < 8; index++)
			if (replay.replay_pad.bAnalogButtons[index] > analog[index])
				analog[index] = replay.replay_pad.bAnalogButtons[index];
	}
	*pad = replay.replay_pad;
	pad->wButtons |= pressed;
	for (index = 0; index < 8; index++)
		if (analog[index] > pad->bAnalogButtons[index])
			pad->bAnalogButtons[index] = analog[index];
	/* with tick actions, the replay ends with them (input_replay_tick_action) */
	if (!replay.action_count && replay.next_state == replay.state_count &&
		ticks > replay.states[replay.state_count - 1].ticks + REPLAY_TAIL_TICKS)
		replay.replay_finished = TRUE;
}

/* controller 1's state as the game reads it (xinput_sdl.c's XInputGetState):
recorded, or replaced by the replay's */
void input_replay_filter(XINPUT_GAMEPAD *pad)
{
	double ticks;

	if (!replay.configured)
		configure();
	take_update();
	if (!*replay.record_path && !*replay.replay_path && !replay.suite)
		return;
	ticks = game_ticks();
	if (ticks < 0.0)
		return;
	/* the suite: nobody plays; a scene with no recording stands still */
	if (replay.suite && (!*replay.replay_path || replay.replay_finished))
	{
		memset(pad, 0, sizeof(*pad));
		return;
	}
	if (*replay.record_path && !replay.record_ended && !take.automatic &&
		(pad->wButtons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START))
	{
		/* Back and Start together end a recording, so it doesn't need trimming
		of the presses that quit the game; the game doesn't see them */
		record_end("Back and Start were pressed together");
		pad->wButtons &= (WORD)~(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START);
	}
	/* nor the presses that work the pause menu, while the game time stands
	still: a replay would press them at a moment it can't reach */
	if (*replay.record_path && !game_time_get_paused())
		record(pad, ticks);
	if (*replay.replay_path && !replay.replay_finished)
		play(pad, ticks);
}

/* ---------- each tick's action */

static int compare_action_keys(const struct recorded_action *action, long segment, long tick)
{
	if (action->segment != segment)
		return action->segment < segment ? -1 : 1;
	return action->tick < tick ? -1 : action->tick > tick;
}

/* player 1's action as a tick takes it (update_client_dequeue, single
player): recorded, or replaced by the recording's for this segment and tick */
void input_replay_tick_action(void *action)
{
	unsigned int words[ACTION_WORDS];
	long tick;

	if (!replay.configured)
		configure();
	take_update();
	if (!*replay.record_actions_path && !*replay.replay_actions_path)
		return;
	if (!game_time_initialized() || input_replay_main_menu_loaded())
		return;
	tick = game_time_get();
	if (awaiting_level((double)tick))
		return;
	if ((replay.last_action_tick > 0 && tick < replay.last_action_tick) ||
		(replay.skip_pending && (tick <= replay.last_action_tick || tick > replay.skip_tick + SKIP_REVERT_TICKS)))
	{
		if (replay.skip_pending && tick >= replay.last_action_tick && replay.preroll_timeline)
		{
			/* (game_ticks sees no step back either: the take's own time begins here) */
			replay.preroll_timeline = FALSE;
			replay.timeline_offset = 0.0;
			replay.next_state = 0;
			replay.last_game_ticks = (double)tick;
			platform_log("debug.input_replay: the level's opening was skipped on tick %ld; the take's time begins", tick);
		}
		replay.skip_pending = FALSE;
		replay.segment++;
		/* a take notes the random seed each segment starts with, and a replay of it starts each segment with
		that seed: what the game does after a revert depends on it, and it is not part of what a revert restores */
		if (take.open)
		{
			take_route_note("segment_seed = %ld %ld %08lx\n", replay.segment, tick, input_replay_random_seed(NULL));
			take.route_written_tick = -TAKE_ROUTE_TICKS;
		}
		replay_segment_began(tick);
	}
	replay.last_action_tick = tick;

	if (*replay.record_actions_path)
	{
		int index;

		if (!replay.actions_file)
		{
			replay.actions_file = fopen(replay.record_actions_path, "w");
			if (!replay.actions_file)
			{
				platform_log("debug.input_record: cannot write %s", replay.record_actions_path);
				replay.record_actions_path[0] = 0;
				return;
			}
			{
				const char *stereo = config_string("display.stereo");

				if (stereo && *stereo && strcmp(stereo, "off"))
					fprintf(replay.actions_file, "%s stereo=%s\n", ACTION_HEADER, stereo);
				else
					fprintf(replay.actions_file, "%s\n", ACTION_HEADER);
			}
			{
				char seeds_path[512];
				size_t length = strlen(replay.record_actions_path);

				/* NAME.actions: NAME.seeds */
				if (length > 8 && !strcmp(replay.record_actions_path + length - 8, ".actions"))
				{
					snprintf(seeds_path, sizeof(seeds_path), "%.*s.seeds", (int)(length - 8), replay.record_actions_path);
					if ((replay.seeds_file = fopen(seeds_path, "w")))
						fprintf(replay.seeds_file, "%s\n", SEEDS_HEADER);
				}
			}
		}
		if (replay.seeds_file)
		{
			unsigned long seeds[2];

			input_replay_random_seeds(seeds, FALSE);
			fprintf(replay.seeds_file, "%ld %ld %08lx %08lx\n", replay.segment, tick, seeds[0], seeds[1]);
			fflush(replay.seeds_file);
		}
		memcpy(words, action, sizeof(words));
		replay.actions_written++;
		fprintf(replay.actions_file, "%ld %ld", replay.segment, tick);
		for (index = 0; index < ACTION_WORDS; index++)
			fprintf(replay.actions_file, " %08x", words[index]);
		fputc('\n', replay.actions_file);
		fflush(replay.actions_file);
	}
	if (replay.action_count)
	{
		replay_seed(tick);
		while (replay.next_action < replay.action_count &&
			compare_action_keys(&replay.actions[replay.next_action], replay.segment, tick) < 0)
			replay.next_action++;
		if (replay.next_action < replay.action_count &&
			!compare_action_keys(&replay.actions[replay.next_action], replay.segment, tick))
		{
			/* (the recording's seeds for this tick first, when it has them) */
			if (replay.actions[replay.next_action].has_seeds)
				input_replay_random_seeds(replay.actions[replay.next_action].seeds, TRUE);
			memcpy(action, replay.actions[replay.next_action].words, sizeof(words));
		}
		/* past the end of this segment's actions: the recording went back to a
		checkpoint here. When that was a cinematic skip, the moment the
		cinematic can be skipped depends on loading (its script waits for the
		map's sounds and textures), so skip it as soon as it can be */
		else if (replay.next_action < replay.action_count &&
			replay.actions[replay.next_action].segment > replay.segment)
		{
			if (input_replay_skip_cinematic() && !replay.skip_pending)
			{
				replay.skip_pending = TRUE;
				replay.skip_tick = tick;
			}
		}
		/* the replay ends with the last recorded action, in the action's own
		segment, whenever the controller's timeline puts it */
		if (compare_action_keys(&replay.actions[replay.action_count - 1], replay.segment, tick) <= 0)
			replay.replay_finished = TRUE;
	}
}

/* ---------- the benchmark */

static int compare_doubles(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void benchmark_report(long width, long height)
{
	double *sorted, total = 0.0;
	static const double budgets[] = { 1000.0 / 90.0, 1000.0 / 60.0, 1000.0 / 45.0, 1000.0 / 30.0 };
	long over[4] = { 0 };
	long index;
	char path[600], stamp[32];
	time_t now = time(NULL);
	FILE *file;

	if (replay.frame_count < 2)
	{
		platform_log("debug.benchmark: no frames timed");
		return;
	}
	sorted = malloc((size_t)replay.frame_count * sizeof(*sorted));
	memcpy(sorted, replay.frame_milliseconds, (size_t)replay.frame_count * sizeof(*sorted));
	qsort(sorted, (size_t)replay.frame_count, sizeof(*sorted), compare_doubles);
	for (index = 0; index < replay.frame_count; index++)
	{
		int budget;

		total += sorted[index];
		for (budget = 0; budget < 4; budget++)
			if (sorted[index] > budgets[budget] * 1.05)
				over[budget]++;
	}
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	snprintf(path, sizeof(path), "benchmark-%s-%s.txt", replay.replay_name, stamp);
	file = fopen(path, "w");
	for (index = 0; index < 2; index++)
	{
		FILE *out = index ? file : NULL;
		char text[1024];

		snprintf(text, sizeof(text),
			"benchmark %s: %ld frames in %.2f s, %.1f frames a second; render %ldx%ld\n"
			"frame ms: average %.2f, median %.2f, 95th %.2f, 99th %.2f, worst %.2f\n"
			"frames over 90 Hz %ld, 60 Hz %ld, 45 Hz %ld, 30 Hz %ld (of %ld)\n",
			replay.replay_name, replay.frame_count, total / 1000.0, 1000.0 * (double)replay.frame_count / total,
			width, height, total / (double)replay.frame_count, sorted[replay.frame_count / 2],
			sorted[replay.frame_count * 95 / 100], sorted[replay.frame_count * 99 / 100], sorted[replay.frame_count - 1],
			over[0], over[1], over[2], over[3], replay.frame_count);
		if (out)
			fputs(text, out);
		else
			platform_log("%s", text);
	}
	if (file)
	{
		fputs("frame ms, in order:\n", file);
		for (index = 0; index < replay.frame_count; index++)
			fprintf(file, "%.3f\n", replay.frame_milliseconds[index]);
		fclose(file);
		platform_log("debug.benchmark: report in %s", path);
	}
	free(sorted);
}

/* each presented frame (d3d8_device.c), with the render size */
void input_replay_frame(long width, long height)
{
	double now;

	if (!replay.benchmark && !(replay.suite && *replay.replay_path))
		return;
	now = halo_frame_trace_milliseconds();
	if (replay.replay_finished)
	{
		if (replay.suite)
			return;
		benchmark_report(width, height);
		platform_log("debug.benchmark: the replay ended; quitting");
		exit(EXIT_SUCCESS);
	}
	/* past the recording's last jump back (counted frame by frame), the
	replay is over once the game time stands still for three seconds within a
	second of the last recorded tick (a10's second cinematic fades to white
	and holds it; a pause earlier in the segment, such as the load after
	skipping c40's opening cinematic, isn't the end), or at the latest after
	that last segment's recorded length plus ten seconds */
	if (replay.action_count && replay.jumps >= replay.actions[replay.action_count - 1].segment && replay.jumped > 0.0)
	{
		const struct recorded_action *last = &replay.actions[replay.action_count - 1];
		long first = replay.action_count - 1;

		while (first > 0 && replay.actions[first - 1].segment == last->segment)
			first--;
		if ((now - replay.moved > 3000.0 && game_time_get() >= last->tick - 30) ||
			now - replay.jumped > (double)(last->tick - replay.actions[first].tick) * 1000.0 / 30.0 + 10000.0)
		{
			platform_log("debug.benchmark: past the recording's end (the game time stopped or ran long)");
			replay.replay_finished = TRUE;
			return;
		}
	}
	if (replay.next_state == 0 || replay.suite)
		return;
	if (replay.previous_frame > 0.0)
	{
		if (replay.frame_count == replay.frame_capacity)
		{
			replay.frame_capacity = replay.frame_capacity ? replay.frame_capacity * 2 : 4096;
			replay.frame_milliseconds = realloc(replay.frame_milliseconds,
				(size_t)replay.frame_capacity * sizeof(*replay.frame_milliseconds));
		}
		replay.frame_milliseconds[replay.frame_count++] = now - replay.previous_frame;
	}
	replay.previous_frame = now;
}
