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
can differ between controller-only replays at different frame rates.
Action-backed replays instead synchronize the shared camera/control pose
when each recorded action is injected.

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
segment, tick and the action's eight 32-bit words in hex. A run that aims
with a hand (input.aim "controller", display.stereo head or side_by_side, and
debug.test_controller or a Sense controller configured, decided at the first
tick) writes "halo action recording 2 stereo=<mode> aim=controller"; every
other run writes version 1, so older readers and the benchmark tooling still
read head-aim runs. The words are the simulation's action: the facing words
are the aim (the hand's, when the action's pad says so, else the head's look),
pad (the eighth word's upper 16 bits) is the look's offset from that aim (bit 0
the hand flag, bits 1-15 the look's yaw less the aim's), and the throttle is
the stick's, in the head's frame. Version 1 uses pad for tutorial action
tests; those bits move to the upper control halfword while replay runs, and
never become a hand flag. Version 2 keeps pad for the hand and stores
tutorial tests in the upper control halfword. Action-backed replay
temporarily bypasses the direct camera so observer interpolation presents
the recorded facing without late controller input.

A recording and a replay each log, when they end, "debug.input_replay: ended
at tick T, unit at (x y z) facing yaw Y aim yaw A" for the local player's
unit as the last recorded tick began, so two runs can be compared.

With debug.benchmark, a replay's frames are timed from the first recorded
state to the last; then the report goes to benchmark-<name>-<time>.txt next
to the recording, a summary to the log, and the game quits.
*/

#include "platform.h"
#include "port_config.h"
#include "input_replay.h"
#include "../../runtime/replay_environment.h"

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
/* player 1's position; 0 while there is no player unit */
extern unsigned char input_replay_player_position(float position[3]);
/* player 1's vitality and the AI's count of actors and of dead objects; 0 while there is no player unit */
extern unsigned char input_replay_fight_state(float *body, float *shield, long *actors, long *dead_objects);
/* port/linux/game/coop_enemies.c: debug.enemy_multiplier in force for a single-player game, and a replayed take's own */
extern double coop_enemies_solo_multiplier(void);
extern void coop_enemies_solo_override(double multiplier);
/* xbox_kernel.c */
double halo_frame_trace_milliseconds(void);
/* port/linux/game/controller_aim_game.c (halo_spatial.h) */
extern int halo_controller_run_aimed(void);
extern int halo_controller_aim_configured(void);
extern int halo_controller_unit_state(float position[3], float *facing_yaw, float *aim_yaw);

#define RECORDING_HEADER "halo input recording 1"
/* how long after the last recorded state a replay ends, in ticks */
#define REPLAY_TAIL_TICKS 30.0

#define ACTION_HEADER "halo action recording"
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
	struct replay_environment environment;
	BOOL environment_invalid, environment_owned, environment_final_tick;
	BOOL record_environment_valid;
	int record_enhanced_animations;
	BOOL benchmark;
	/* the device benchmark's suite (benchmark_suite.c) owns the replays: several in one launch, started with
	input_replay_begin; it times the frames itself, and the real controller is ignored throughout */
	BOOL suite;
	/* Only a benchmark suite's current replay opts in here; ordinary play uses the default-off config. */
	BOOL benchmark_lip_sync;
	BOOL recorded_original_lip_sync;
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
	struct input_replay_camera_sample camera_sample;
	BOOL control_pending, control_owned;
	short control_local_player;
	/* tick actions written to the take being recorded */
	long actions_written;
	/* a replayed take's .route: the seed each of its segments began with, and whether the take began after the
	level's opening was skipped (its first tick is past 0), which the replay must do first */
	struct { long segment, tick; unsigned long seed; BOOL applied; } segment_seeds[32];
	int segment_seed_count;
	/* the difficulty the take was recorded at (its .route's), checked against the level's at the first replayed
	tick: a replay at another difficulty meets other enemies and other scripts (a10's tutorial runs only on easy
	and normal) */
	char route_difficulty[16];
	BOOL difficulty_checked;
	/* ticks whose random seeds, as the game had them before the recording's were set, were not the recording's:
	the tick before drew a different count of random numbers, so the replay had left the take's path there */
	long global_misses;
	/* the seeds as the last tick's hook found them, before the recording's were set */
	unsigned long found_seeds[2];
	BOOL preroll, preroll_timeline;
	/* the replay itself skipped a cinematic and the revert that follows has not been seen yet. The revert is a
	segment's beginning even when the game time does not go back by it: a cinematic skipped on the level's first
	tick reverts to that same tick (a fast load, or debug.fixed_timestep), which the tick alone cannot show */
	BOOL skip_pending;
	long skip_tick;
	/* the local player's unit as the latest tick's dequeue found it, for the
	end-of-run line, and whether that line has been logged */
	BOOL unit_known, ended_logged, exit_hooked;
	long unit_tick;
	float unit_position[3], unit_facing_yaw, unit_aim_yaw;
	/* the header's version a recording was written in */
	int actions_version;
	/* the game time's jumps back seen frame by frame (the ticks' own
	segment count can miss one: a cinematic can stop the player updates
	right after its checkpoint), when the last came, and when the game
	time last moved */
	long jumps;
	double jumped, moved;
} replay;
static BOOL suite_suspended;

/* A suspended suite owns input even in synthetic scenes without a replay. */
#ifdef HALO_IOS
extern void host_touch_replay_active(int active);
#endif
static void replay_touch_update(void)
{
#ifdef HALO_IOS
	host_touch_replay_active(suite_suspended || (*replay.replay_path && !replay.replay_finished));
#endif
}

static void replay_finish(void)
{
	replay.replay_finished = TRUE;
	if (!replay.environment_final_tick)
		replay.environment_owned = FALSE;
	replay_touch_update();
}

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
	int version = 0;
	BOOL controller_aim = FALSE;

	if (!file)
	{
		platform_log("debug.input_replay: no %s; the controller alone drives the replay", replay.replay_actions_path);
		return;
	}
	if (!fgets(line, sizeof(line), file) || !strchr(line, '\n') ||
		!replay_environment_parse(line, &replay.environment))
	{
		platform_log("debug.input_replay: %s has an invalid action header", replay.replay_actions_path);
		replay.environment_invalid = TRUE;
		replay_finish();
		fclose(file);
		return;
	}
	{
		/* Older benchmark takes predate this tag and used simulated mouths implicitly. */
		replay.recorded_original_lip_sync = replay.environment.original_lip_sync;
		platform_log("debug.input_replay: recorded lip sync: %s%s",
			replay.recorded_original_lip_sync ? "original" : "simulated",
			strstr(line, " lip_sync=") ? "" : " (legacy take)");
		if (!replay.recorded_original_lip_sync && !replay.benchmark_lip_sync &&
			!config_boolean("debug.benchmark_lip_sync"))
			platform_log("debug.input_replay: warning: this take needs debug.benchmark_lip_sync=true to match its simulated mouths; this replay may diverge");
		if (replay.recorded_original_lip_sync)
			platform_log("debug.input_replay: warning: original audio-driven mouths cannot be reproduced by a replay; record benchmark takes with debug.benchmark_lip_sync=true");
		version = replay.environment.action_version;
		controller_aim = replay.environment.controller_aim;
		platform_log("input replay: actions aim=%s", controller_aim ? "controller" : "head");
		/* the stereo mode the recording was made in; an older header has none, which reads as off */
		const char *recorded = replay.environment.stereo;
		const char *run = config_string("display.stereo");

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
		/* Version 1 stores tutorial flags in pad. Translate them to the local
		queue's control metadata, leaving no hand flag. Version 2 keeps pad for
		tracked aim and stores tutorial flags in the upper control halfword. */
		if (version == 1)
		{
			w[0] = (w[0] & 0xffffu) | (w[7] & 0xffff0000u);
			w[7] &= 0xffffu;
		}
		else if ((w[7] >> 16) && !(w[7] & 0x00010000u))
		{
			/* All fifteen delta bits belong to a hand action. Without its
			flag the only valid pad is zero (controller_facing_pack). */
			replay.environment_invalid = TRUE;
			replay_finish();
			platform_log("debug.input_replay: invalid controller action pad");
			break;
		}
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
	double multiplier = 1.0, configured;

	coop_enemies_solo_override(-1.0);
	configured = coop_enemies_solo_multiplier();
	if (length < 6 || strcmp(replay.replay_path + length - 6, ".input"))
		return;
	snprintf(path, sizeof(path), "%.*s.route", (int)(length - 6), replay.replay_path);
	if (!(file = fopen(path, "r")))
	{
		if (configured > 1.0)
			platform_log("debug.input_replay: %s has no .route, so the enemy multiplier it was recorded with is unknown; "
				"using debug.enemy_multiplier %.3f", replay.replay_path, configured);
		return;
	}
	while (fgets(line, sizeof(line), file))
	{
		double recorded;

		if (sscanf(line, "first_tick = %ld", &tick) == 1)
			first_tick = tick;
		else if (sscanf(line, "enemy_multiplier = %lf", &recorded) == 1)
			multiplier = recorded >= 1.0 ? recorded : 1.0;
		else if (sscanf(line, "difficulty = %15s", replay.route_difficulty) == 1)
		{
			/* (checked at the first replayed tick, replay_check_difficulty) */
		}
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
	/* the multiplier the take was recorded with decides which enemies it meets, so a replay uses it; a replay that is
	also recording a new take only supplies the input, and the setting stands for the new take */
	if (fabs(multiplier - configured) > 0.0005)
	{
		if (*config_string("debug.input_record"))
			platform_log("debug.input_replay: the take was recorded with debug.enemy_multiplier %.3f; this run records a new "
				"take at %.3f", multiplier, configured);
		else
		{
			platform_log("debug.input_replay: warning: the take was recorded with debug.enemy_multiplier %.3f and the setting "
				"is %.3f; the replay uses the recorded value", multiplier, configured);
			coop_enemies_solo_override(multiplier);
		}
	}
	else if (multiplier > 1.0)
		platform_log("debug.input_replay: the take's enemy multiplier is %.3f, as the setting", multiplier);
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
		replay_finish();
		return;
	}
	if (!fgets(line, sizeof(line), file) || strncmp(line, RECORDING_HEADER, strlen(RECORDING_HEADER)))
	{
		platform_log("debug.input_replay: %s isn't an input recording", replay.replay_path);
		fclose(file);
		replay_finish();
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
		replay_finish();
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

/* Events of the level being played, in the recorded time: each cinematic's start and end, the checkpoints saved and
the structure BSP switches (and "cinematic_done", where a script leaves its skippable cinematic whether it was watched
or skipped: a skip reverts the game, so a skipped cinematic has a start but no end), numbered in order of play per kind ("cinematic_end:2:3412": the second cinematic ended at
recorded tick 3412). A take's .route carries them as "events = a:1:10,b:1:20", and a catalog case can anchor its
window on one (benchmark_plan.c). Bookkeeping only: nothing here reads or changes the simulation. */
#define EVENT_KINDS 5
static const char *const event_kinds[EVENT_KINDS] = { "cinematic_start", "cinematic_end", "checkpoint", "bsp_switch",
	"cinematic_done" };
static struct { char text[1536]; int counts[EVENT_KINDS]; } events;

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
	fputs(take.route, route);
	if (events.text[0])
		fprintf(route, "events = %s\n", events.text);
	fprintf(route, "closed_by = %s\nsegments = %ld\npauses = %ld\nlast_tick = %ld\nseconds = %.1f\n",
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

#ifdef _WIN32
	/* (the Windows C library has gmtime_s, its arguments the other way around) */
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	snprintf(take.map, sizeof(take.map), "%s", map);
	snprintf(take.base, sizeof(take.base), "recordings/take-%s-%04d%02d%02d-%02d%02d%02d", map, utc.tm_year + 1900,
		utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
	take.first_tick = take.last_tick = tick;
	take.players_written = take.paused = FALSE;
	take.pauses = 0;
	take.route[0] = 0;
	memset(&events, 0, sizeof(events));
	replay.actions_written = 0;
	replay.record_environment_valid = FALSE;
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
	if (coop_enemies_solo_multiplier() > 1.0)
		take_route_note("enemy_multiplier = %.3f\n", coop_enemies_solo_multiplier());
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
		if (replay.benchmark && replay.recorded_original_lip_sync)
		{
			platform_log("debug.benchmark: refusing an original-mouth take; record it again with debug.benchmark_lip_sync=true");
			exit(EXIT_FAILURE);
		}
		if (replay.benchmark && replay.action_count && !config_boolean("debug.benchmark_lip_sync"))
		{
			platform_log("debug.benchmark: simulated-mouth takes require debug.benchmark_lip_sync=true");
			exit(EXIT_FAILURE);
		}
		seeds_load();
		route_load();
	}
	replay_touch_update();
}

/* The device benchmark's suite: replays NAME (a path ending in ".input", or NULL or "" for none) from the next
level's first tick, in place of whatever was replaying. From the first call on, the real controller is ignored
while a level's game time runs. 1 if there is nothing to load or the recording loaded with at least one state */
int input_replay_begin(const char *name, int benchmark_lip_sync)
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
	coop_enemies_solo_override(-1.0);
	memset(&events, 0, sizeof(events));
	replay_touch_update();
	if (game_time_initialized() && !input_replay_main_menu_loaded())
	{
		replay.await_level = TRUE;
		replay.await_ticks = (double)game_time_get();
	}
	if (!name || !*name)
		return 1;
	replay.benchmark_lip_sync = benchmark_lip_sync != 0;
	{
		const char *base = strrchr(name, '/');

		snprintf(replay.replay_name, sizeof(replay.replay_name), "%s", base ? base + 1 : name);
	}
	recording_path(name, replay.replay_path, sizeof(replay.replay_path));
	actions_path(replay.replay_path, replay.replay_actions_path, sizeof(replay.replay_actions_path));
	replay_load();
	actions_load();
	if (benchmark_lip_sync && replay.recorded_original_lip_sync)
	{
		platform_log("benchmark suite: refusing an original-mouth take; record it again with debug.benchmark_lip_sync=true");
		replay_finish();
		return 0;
	}
	/* (a take's .seeds, both random seeds at every tick; and its .route: the older segment seeds, and whether it
	began after its level's opening was skipped) */
	seeds_load();
	route_load();
	replay_touch_update();
	return replay.state_count > 0 && !replay.environment_invalid && !replay.replay_finished;
}

/* A suite owns effective input without changing the user's saved recording settings. */
int input_replay_suite_suspend(void)
{
	if (suite_suspended)
		return 1;
	if (!replay.configured)
		configure();
	if (take.open || replay.record_file || replay.actions_file || replay.seeds_file)
		return 0;
	suite_suspended = TRUE;
	take.automatic = FALSE;
	input_replay_begin(NULL, 0);
	return 1;
}

void input_replay_suite_restore(void)
{
	if (!suite_suspended)
		return;
	input_replay_begin(NULL, 0);
	replay.await_level = FALSE; /* the released suite no longer owns a level transition */
	replay.suite = FALSE;
	replay.configured = FALSE;
	take.automatic = FALSE;
	suite_suspended = FALSE;
	replay_touch_update();
}

int input_replay_suite_suspended(void)
{
	return suite_suspended;
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

/* the end-of-run line, once: the local player's unit as the last tick that
took an action began (snapshotted at each tick's dequeue, so a recording
cut off by the game's exit and a replay that reached its last action report
the same moment) */
static void log_ended(void)
{
	if (replay.ended_logged)
		return;
	replay.ended_logged = TRUE;
	if (replay.unit_known)
		platform_log("debug.input_replay: ended at tick %ld, unit at (%.4f %.4f %.4f) facing yaw %.2f aim yaw %.2f",
			replay.unit_tick, replay.unit_position[0], replay.unit_position[1], replay.unit_position[2],
			replay.unit_facing_yaw, replay.unit_aim_yaw);
	else
		platform_log("debug.input_replay: ended at tick %ld, no unit", replay.last_action_tick);
	if (*replay.record_actions_path && replay.actions_version == 1 && halo_controller_run_aimed())
		platform_log("debug.input_record: warning: a hand aimed, but %s was written as version 1 (the configuration"
			" didn't say a hand would aim at the first tick)", replay.record_actions_path);
}

static void exit_hook(void)
{
	/* a recording the game's exit cuts off (debug.exit_after, closing the window) */
	if (*replay.record_actions_path && replay.actions_file)
		log_ended();
}

/* ends a recording: both files close, and nothing more is written */
static void record_end(const char *why)
{
	log_ended();
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

/* A mode change closes the take before either stream can contain the new mode.
   Automatic recording stops explicitly too, so a same-second take cannot overwrite it. */
static int record_environment_ready(void)
{
    int enhanced;
    if (replay.record_ended || (!*replay.record_path && !*replay.record_actions_path)) return 0;
    enhanced = input_replay_enhanced_animations(config_boolean("game.enhanced_animations") != 0);
    if (!replay.record_environment_valid) {
        replay.record_enhanced_animations = enhanced;
        replay.record_environment_valid = TRUE;
    } else if (enhanced != replay.record_enhanced_animations) {
        take_close("enhanced_animation_mode_changed");
        record_end("enhanced animation mode changed; start a new recording to continue");
        take.automatic = FALSE;
        return 0;
    }
    return 1;
}

static void record(const XINPUT_GAMEPAD *pad, double ticks)
{
	int index;

	if (!record_environment_ready())
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
		replay_finish();
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

/* at the first replayed tick: whether the level runs at the difficulty the take was recorded at */
static void replay_check_difficulty(void)
{
	static const char *const names[] = { "easy", "normal", "hard", "impossible" };
	char map[32];
	long difficulty, players;
	unsigned long seed;
	const char *running;

	replay.difficulty_checked = TRUE;
	input_replay_level(map, sizeof(map), &difficulty, &players, &seed);
	running = difficulty >= 0 && difficulty < 4 ? names[difficulty] : "unknown";
	if (strcmp(running, replay.route_difficulty))
		platform_log("debug.input_replay: warning: the take was recorded on %s and %s runs on %s; the replay will not "
			"follow its route (set the difficulty for the next map with game_difficulty_set or main_set_difficulty, "
			"not game_difficulty_level_set, which the map load overwrites)", replay.route_difficulty, map, running);
	else
		platform_log("debug.input_replay: %s runs on %s, as the take was recorded", map, running);
}

/* debug.input_replay_route: "debug.input_replay: route segment S tick T at X,Y,Z (what the tick took)" every
route_every ticks, which tools/replay_check.py compares with a recording's known positions */
static void route_log(long tick, const char *took)
{
	static long every = -1;
	float position[3];

	if (every < 0)
		every = config_integer("debug.input_replay_route");
	if (every <= 0 || tick % every)
		return;
	if (input_replay_player_position(position))
	{
		float body = 0, shield = 0;
		long actors = 0, dead = 0;

		input_replay_fight_state(&body, &shield, &actors, &dead);
		platform_log("debug.input_replay: route segment %ld tick %ld at %.2f,%.2f,%.2f (%s; body %.2f shield %.2f "
			"actors %ld dead %ld; seed misses %ld; found %08lx %08lx)", replay.segment, tick,
			position[0], position[1], position[2], took, body, shield, actors, dead, replay.global_misses,
			replay.found_seeds[0], replay.found_seeds[1]);
	}
	else
		platform_log("debug.input_replay: route segment %ld tick %ld at none (%s)", replay.segment, tick, took);
}

/* The simulation draws from the random sequences before the tick's player action is taken (game_tick runs
ai_update, where the enemies choose, ahead of players_update_before_game, where the action and the recording's
seeds are set), and the frames drawn between two ticks draw from them too, as often as the frames come. So the
seeds an enemy's choice starts from depend on how many frames came between the ticks: two replays of one take part
at the first choice a frame's draw changed, and the recording's seeds, set after the choices, cannot undo it. While
a take is recorded or replayed, a tick ends by noting both seeds and the next one begins by putting them back, so
the frames' draws never reach the simulation. (The local sequence is started again from the global one: the numbers
a tick draws from it are not the same in two runs.) The sounds reach the simulation too: a unit speaks a scripted
sound only if the mixer started it, which follows the sound cache's loading and the playing sounds' timing, and the
sounds drew from the same sequences. So the sounds draw from a sequence of their own, a unit speaks a scripted
sound whether or not it started while a take is recorded or replayed (input_replay_sim_isolated), and a replay
plays its sounds but its mixer moves no mouths (input_replay_mixer_detached). */
static struct { BOOL valid; long tick; unsigned long seeds[2]; } tick_seeds;
static long tick_seeds_begun = -1;

static BOOL tick_seeds_wanted(void)
{
	return replay.action_count || *replay.record_actions_path;
}

/* whether the simulation is kept clear of what only the frames and the devices decide (a take is being recorded or
replayed) */
unsigned char input_replay_sim_isolated(void)
{
	return tick_seeds_wanted();
}

/* Mouth animation is a benchmark-only approximation. Do not couple it to the RNG/scripted-speech isolation:
   recording normal play keeps the original mixer behavior, and a completed or cleared replay releases it. */
unsigned char input_replay_benchmark_lip_sync(void)
{
	if (!config_boolean("debug.benchmark_lip_sync") && !replay.benchmark_lip_sync)
		return FALSE;
	if (replay.action_count)
		return !replay.await_level && !replay.replay_finished && !replay.recorded_original_lip_sync;
	return *replay.record_actions_path && !replay.record_ended;
}

/* whether a replay is under way: its sounds play, but the mixer sets nothing the simulation reads (a unit's mouth,
game_sound_set_mouth_aperture) */
unsigned char input_replay_mixer_detached(void)
{
	return replay.action_count != 0;
}

/* Consulted on each enhanced-animation decision, outside the configuration cache. */
int input_replay_enhanced_animations(int configured)
{
    return replay.environment_owned ? replay.environment.enhanced_animations : configured;
}

void input_replay_tick_begin(void)
{
	long tick;

	if (!replay.configured)
		configure();
	if (game_time_initialized() && !input_replay_main_menu_loaded()) {
		if (*replay.replay_path && !replay.replay_finished && !replay.environment_invalid &&
			!awaiting_level((double)game_time_get()))
			replay.environment_owned = TRUE;
		record_environment_ready();
	}
	if (!tick_seeds_wanted())
		return;
	tick = game_time_get();
	{
		unsigned long seeds[2];

		/* (the local sequence starts each tick from the global one: how many numbers a tick draws from it is not the
		same in two runs, objects_update's draws among them, and what the next tick's enemies draw follows it. That
		holds for a tick that follows no tick too, a level's first and the first after a revert: the frames drew from
		the local sequence while the level loaded or the dead player's timer ran, as many numbers as they came. The
		frames never draw from the global sequence, so it is as the last tick left it, or as the level or the revert
		set it) */
		input_replay_random_seeds(seeds, FALSE);
		if (tick_seeds.valid && tick == tick_seeds.tick + 1)
			seeds[0] = tick_seeds.seeds[0];
		seeds[1] = seeds[0] * 1664525ul + 1013904223ul;
		input_replay_random_seeds(seeds, TRUE);
	}
	tick_seeds_begun = tick;
}

void input_replay_tick_end(void)
{
	/* Only mode ownership is deferred; replay finish/input scheduling stays unchanged. */
	if (replay.environment_final_tick) {
		replay.environment_final_tick = FALSE;
		replay.environment_owned = FALSE;
	}
	if (!tick_seeds_wanted() || tick_seeds_begun < 0)
		return;
	tick_seeds.valid = TRUE;
	tick_seeds.tick = tick_seeds_begun;
	input_replay_random_seeds(tick_seeds.seeds, FALSE);
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
	replay.control_pending = replay.control_owned = FALSE;
	if (replay.environment_invalid || (*replay.replay_path && !replay.state_count))
		return;
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
		platform_log("debug.input_replay: segment %ld begins at tick %ld (%s; the last tick was %ld)", replay.segment + 1,
			tick, replay.skip_pending && tick >= replay.last_action_tick ? "a skipped cinematic" : "the game time went back",
			replay.last_action_tick);
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
	/* Keep diagnostics keyed to the action actually injected, including gaps. */
	{
		struct input_replay_camera_sample *sample = &replay.camera_sample;
		sample->previous_valid = sample->valid && sample->segment == replay.segment && sample->tick + 1 == tick;
		sample->previous_tick = sample->tick;
		memcpy(sample->previous_facing, sample->facing, sizeof(sample->facing));
		sample->segment = replay.segment;
		sample->tick = tick;
		sample->valid = FALSE;
	}
	/* the unit as this tick begins, for the end-of-run line */
	replay.unit_known = halo_controller_unit_state(replay.unit_position, &replay.unit_facing_yaw, &replay.unit_aim_yaw);
	replay.unit_tick = tick;
	if (*replay.record_actions_path && !replay.exit_hooked)
	{
		replay.exit_hooked = TRUE;
		atexit(exit_hook);
	}

	if (*replay.record_actions_path && record_environment_ready())
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
				/* a hand's aim can start after this tick, so the version
				follows the configuration (a hand could aim here) */
				int aims = stereo && (!strcmp(stereo, "head") || !strcmp(stereo, "side_by_side")) &&
					halo_controller_aim_configured();

				replay.actions_version = aims ? 2 : 1;
				fprintf(replay.actions_file, "%s %d lip_sync=%s enhanced_animations=%d", ACTION_HEADER, replay.actions_version,
					input_replay_benchmark_lip_sync() ? "simulated" : "original", replay.record_enhanced_animations);
				if (stereo && *stereo && strcmp(stereo, "off"))
					fprintf(replay.actions_file, " stereo=%s", stereo);
				if (aims)
					fprintf(replay.actions_file, " aim=controller");
				fputc('\n', replay.actions_file);
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
					/* (a recording of its own, not a take: NAME.route, with only what a replay needs to meet the same
					enemies) */
					if (!take.open && coop_enemies_solo_multiplier() > 1.0)
					{
						FILE *route;

						snprintf(seeds_path, sizeof(seeds_path), "%.*s.route", (int)(length - 8), replay.record_actions_path);
						if ((route = fopen(seeds_path, "w")))
						{
							fprintf(route, "enemy_multiplier = %.3f\n", coop_enemies_solo_multiplier());
							fclose(route);
						}
					}
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
		/* Keep existing head-aim recordings byte-compatible. Only version 2
		needs both tutorial flags and the independent tracked-hand halfword. */
		if (replay.actions_version == 1)
		{
			words[7] = (words[7] & 0xffffu) | (words[0] & 0xffff0000u);
			words[0] &= 0xffffu;
		}
		replay.actions_written++;
		fprintf(replay.actions_file, "%ld %ld", replay.segment, tick);
		for (index = 0; index < ACTION_WORDS; index++)
			fprintf(replay.actions_file, " %08x", words[index]);
		fputc('\n', replay.actions_file);
		fflush(replay.actions_file);
	}
	if (replay.action_count)
	{
		BOOL applied = FALSE;

		if (!replay.difficulty_checked && replay.route_difficulty[0])
			replay_check_difficulty();
		replay_seed(tick);
		while (replay.next_action < replay.action_count &&
			compare_action_keys(&replay.actions[replay.next_action], replay.segment, tick) < 0)
			replay.next_action++;
		if (replay.next_action < replay.action_count &&
			!compare_action_keys(&replay.actions[replay.next_action], replay.segment, tick))
		{
			/* (the recording's seeds for this tick first, when it has them) */
			if (replay.actions[replay.next_action].has_seeds)
			{
				unsigned long seeds[2];

				input_replay_random_seeds(seeds, FALSE);
				replay.found_seeds[0] = seeds[0];
				replay.found_seeds[1] = seeds[1];
				{
					const unsigned long *recorded = replay.actions[replay.next_action].seeds;

					/* (the global sequence is the simulation's own: a miss there is a replay off the take's path; the local
					one is drawn from by what is only drawn, so a miss there alone does not leave the path) */
					if (seeds[0] != recorded[0] && !replay.global_misses++)
						platform_log("debug.input_replay: segment %ld, tick %ld: the game's global seed %08lx is not the "
							"recording's %08lx: the replay left the take's path in the tick before", replay.segment, tick,
							seeds[0], recorded[0]);
				}
				input_replay_random_seeds(replay.actions[replay.next_action].seeds, TRUE);
			}
			memcpy(action, replay.actions[replay.next_action].words, sizeof(words));
			memcpy(replay.camera_sample.facing, &replay.actions[replay.next_action].words[1], sizeof(float) * 2);
			memcpy(&replay.camera_sample.zoom, &replay.actions[replay.next_action].words[7], sizeof(short));
			replay.camera_sample.valid = TRUE;
			replay.control_pending = TRUE;
			applied = TRUE;
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
		if (compare_action_keys(&replay.actions[replay.action_count - 1], replay.segment, tick) <= 0) {
			replay.environment_final_tick = replay.environment_owned;
			replay_finish();
			log_ended();
		}
		route_log(tick, applied ? "recorded action" : "no recorded action");
	}
	else if (*replay.record_actions_path)
		route_log(tick, "recorded");
}

/* The queue calls this once for its mapped local player. Later damage impulses
   stay in control until the next recorded action; render frames never restamp it. */
unsigned char input_replay_apply_control(short local_player_index, float facing[2], short *zoom)
{
	if (!replay.control_pending || local_player_index < 0 || !replay.state_count ||
		*replay.record_actions_path || replay.await_level || !game_time_initialized() ||
		input_replay_main_menu_loaded())
		return FALSE;
	memcpy(facing, replay.camera_sample.facing, sizeof(replay.camera_sample.facing));
	*zoom = replay.camera_sample.zoom;
	replay.control_pending = FALSE;
	replay.control_owned = TRUE;
	replay.control_local_player = local_player_index;
	return TRUE;
}

unsigned char input_replay_controls_facing(short local_player_index)
{
	long tick;
	if (replay.await_level || !game_time_initialized() || input_replay_main_menu_loaded())
	{
		replay.control_pending = replay.control_owned = FALSE;
		return FALSE;
	}
	/* game_time advances after injection. Keep the last action through its
	   presentation even though tick_action has already marked replay finished. */
	tick = game_time_get();
	if (tick < replay.camera_sample.tick || tick > replay.camera_sample.tick + 1)
		replay.control_pending = replay.control_owned = FALSE;
	return replay.control_owned && local_player_index == replay.control_local_player;
}

/* Head input runs before tick injection. Keep it in the eye cameras even
   before the first action and through gaps, where a later action would erase
   a head turn already consumed into raw control. Legacy/recording stays live. */
unsigned char input_replay_action_camera_active(void)
{
	long tick;
	if (!replay.state_count || !replay.action_count || *replay.record_actions_path ||
		replay.await_level || !game_time_initialized() || input_replay_main_menu_loaded())
		return FALSE;
	if (!replay.replay_finished)
		return TRUE;
	tick = game_time_get();
	return replay.control_owned && tick >= replay.camera_sample.tick && tick <= replay.camera_sample.tick + 1;
}

/* Read cached time only: game_ticks() also advances controller/segment state. */
unsigned char input_replay_camera_sample(struct input_replay_camera_sample *sample)
{
	long every = config_integer("debug.input_replay_route");
	if (every <= 0 || !replay.action_count || replay.await_level ||
		!game_time_initialized() || input_replay_main_menu_loaded() || replay.last_action_tick % every)
		return FALSE;
	*sample = replay.camera_sample;
	sample->timeline = replay.last_game_ticks + replay.timeline_offset;
	return TRUE;
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
		log_ended();
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
			replay_finish();
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

/* A cinematic started or ended, a checkpoint was saved or a structure BSP was switched to (cinematics.c, main.c,
scenario.c): noted in the recorded time for the take's .route and the log. */
void input_replay_note_event(const char *kind, const char *name)
{
	int index;
	double ticks;
	char entry[96], script[40];
	size_t length = strlen(events.text);

	if (!game_time_initialized())
		return;
	ticks = game_ticks();
	if (ticks < 0.0)
		return;
	for (index = 0; index < EVENT_KINDS; index++)
	{
		if (!strcmp(kind, event_kinds[index]))
			break;
	}
	if (index == EVENT_KINDS)
		return;
	events.counts[index]++;
	/* (the script that called, for a cinematic: lowercase letters, digits and underscores; none from the engine) */
	script[0] = 0;
	if (name && name[0] != '[')
	{
		size_t at;

		for (at = 0; name[at] && at < sizeof(script) - 1; at++)
			script[at] = (name[at] >= 'A' && name[at] <= 'Z') ? (char)(name[at] + 32) :
				((name[at] >= 'a' && name[at] <= 'z') || (name[at] >= '0' && name[at] <= '9')) ? name[at] : '_';
		script[at] = 0;
	}
	if (script[0])
		snprintf(entry, sizeof(entry), "%s:%d:%ld:%s", kind, events.counts[index], (long)ticks, script);
	else
		snprintf(entry, sizeof(entry), "%s:%d:%ld", kind, events.counts[index], (long)ticks);
	platform_log("input_replay: event %s %d at recorded tick %ld %s", kind, events.counts[index], (long)ticks, script);
	if (length + strlen(entry) + 2 < sizeof(events.text))
		snprintf(events.text + length, sizeof(events.text) - length, "%s%s", length ? "," : "", entry);
}
