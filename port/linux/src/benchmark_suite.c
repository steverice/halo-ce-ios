/*
BENCHMARK_SUITE.C

The device benchmark's suite, inside the game: debug.benchmark_suite =
"quick" or "full" plays its scenes by itself, several times over at
different settings, and quits. Downloaded multiplayer workloads run after
the solo adaptation stages.

The app around the game (port/ios/host/host_benchmark.m) decides which run
comes next, measures every frame, memory and heat, and writes the report;
its plan and journal are port/runtime/benchmark_plan.c. This file only
carries a run out: it sets the run's render size, shadow size and
filtering, loads the scene's map (or the main menu), starts the scene's
recording if it has one (input_replay.c), waits out the scene's lead-in,
opens the measured window, and closes it when the window's time is up or
the recording ends. Nobody holds the controller: a scene without a
recording plays by itself.

Once a presented frame (d3d8_device.c). Apple builds only: the host calls
are the iOS, tvOS, visionOS and Mac app's.
*/

#include "platform.h"
#include "port_config.h"
#include "benchmark_mp_state.h"
#include "benchmark_mp_arena.h"
#include "network_test_suite.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HALO_IOS

/* port/runtime/guest/runtime/guest_host.h */
int host_benchmark_start(const char *mode, int drawable_lines, int base_lines, int anisotropy, int shadow_size,
	int model_lod_max);
/* 1: a run to carry out; 0: the suite is over and the game quits; 2: over, and the game goes back to its main menu */
int host_benchmark_next(char *text, unsigned int size);
void host_benchmark_window(int open, int width, int height);
void host_benchmark_graphics(const char *json);
int host_external_active(void);
void host_benchmark_end(const char *status, const char *note);
void host_benchmark_fault(const char *note);
/* input_replay.c */
int input_replay_begin(const char *name, int benchmark_lip_sync);
int input_replay_suite_suspend(void);
void input_replay_suite_restore(void);
int input_replay_ended(void);
long input_replay_segment(void);
double input_replay_length_ticks(void);
double input_replay_timeline(void);
/* port/linux/game/benchmark_scenario.c */
void benchmark_scenario_load(const char *map, const char *difficulty);
unsigned char benchmark_scenario_at_menu(void);
unsigned char benchmark_scenario_player_position(float *position);
void mp_benchmark_probe_set_suite_requested(int requested);
void mp_benchmark_probe_reset_controllers(void);
/* source/interface/ui_widget.c: cleared when the ordinary UI pump joins the startup worker */
unsigned char filesystem_check_thread_is_active(void);
/* port/linux/game/scene_class.c: the menu, a cutscene or gameplay, this frame (port_config.h's CONFIG_SCENE_ values) */
int halo_scene_class(void);
/* d3d8_device.c */
void halo_benchmark_overrides(long render_lines, long shadow_size, long anisotropy);
/* source/models/models.c: display.model_lod's value while the game runs (1 "max", 0 "auto") */
void halo_model_lod_set(int is_max);
/* sdl_platform.c */
void platform_video_drawable_size(int *width, int *height);
/* xbox_kernel.c: milliseconds of real time */
double halo_frame_trace_milliseconds(void);

/* where the suite's recordings are, in the data folder */
#define RECORDINGS "benchmark/recordings"
/* a map that has not started this long after it was asked for never will */
#define LOAD_LIMIT_MILLISECONDS 180000.0
/* a recording that has not ended this long after its length has left its route */
#define REPLAY_OVERRUN_MILLISECONDS 60000.0
/* how long the first run waits for the main menu before going ahead without it (a launch straight into a map) */
#define START_LIMIT_MILLISECONDS 60000.0
/* the menu fades in and its music starts before its frames settle */
#define MENU_SETTLE_MILLISECONDS 3000.0
/* the route: player 1's position every this many ticks of the recorded time */
#define ROUTE_TICKS 300.0
/* a replay this many world units from the recording's own route has left it (a tick's difference in when a point
is taken moves a running player a small fraction of this) */
#define ROUTE_TOLERANCE 2.0

enum suite_state { suite_unasked, suite_off, suite_starting, suite_next, suite_loading, suite_lead, suite_measuring,
	suite_wait_mp_teardown };
enum suite_action { suite_action_none, suite_action_menu, suite_action_mp_begin, suite_action_mp_end };

static struct
{
	enum suite_state state;
	/* the run in hand */
	char scene[48], map[32], difficulty[16], recording[64];
	char mp_simulation[32], mp_game_mode[16];
	int mp_players, mp_views, mp_score_limit, mp_time_limit_minutes;
	struct benchmark_mp_arena_config mp_arena;
	double lead, seconds;
	long lines, anisotropy, shadow_size;
	int model_lod_max;
	/* a gameplay scene: frames of a cutscene inside it are not measured (a cutscene has its own target, and a
	profile may hold it to 30); whether the window is closed for one now */
	int gameplay, in_cutscene;
	/* real time: when the state was entered; when the window opened */
	double entered, opened;
	/* the recorded time when the window opened (a level), and the next route point's */
	double opened_ticks, route_due;
	char route[4096];
	size_t route_length;
	/* the recording's own route (NAME.checks beside it: the same "tick:x,y,z;..." text, made from replays that
	agreed), the next point of it to compare, and how far off the worst point so far was */
	char checks[4096];
	const char *next_check;
	double worst_distance;
	int diverged;
	enum suite_action pending_action;
	int mp_action_failed, mp_host_started, mp_scene, mp_fault_reported;
	long mp_first_tick, mp_ready_tick, mp_last_tick;
	double mp_menu_seen_at, mp_advanced_at;
	struct benchmark_mp_state mp_previous;
	int mp_previous_valid;
	double mp_distance[BENCHMARK_MP_ACTORS];
	unsigned long mp_fire[BENCHMARK_MP_ACTORS], mp_jump[BENCHMARK_MP_ACTORS], mp_grenade_requests[BENCHMARK_MP_ACTORS];
	unsigned long mp_ammo_spent, mp_grenades_used, mp_damage_changes, mp_deaths, mp_respawns;
	unsigned long mp_ammo_spent_actor[BENCHMARK_MP_ACTORS];
	uint32_t mp_energy_projectiles_actor[BENCHMARK_MP_ACTORS];
	int mp_energy_invalid;
	int mp_projectile_seen;
	unsigned long mp_repositions, mp_scripted_hits, mp_scripted_kills;
} suite;

void benchmark_suite_multiplayer_fault(const char *note)
{
	if (!suite.mp_scene || suite.mp_fault_reported || (suite.state != suite_loading && suite.state != suite_lead &&
		suite.state != suite_measuring && suite.state != suite_wait_mp_teardown))
		return;
	suite.mp_fault_reported = 1;
	suite.pending_action = suite_action_none;
	suite.state = suite_off;
	host_benchmark_fault(note);
}

/* Present queues map/network work; the next game loop consumes it before network updates. */
void benchmark_suite_mainloop_update(void)
{
	enum suite_action action = suite.pending_action;

	suite.pending_action = suite_action_none;
	switch (action)
	{
	case suite_action_menu:
		benchmark_scenario_load("", "");
		break;
	case suite_action_mp_begin:
		if ((suite.mp_arena.columns || suite.mp_players > 4) &&
			(strcmp(suite.mp_simulation, "network_players") || !benchmark_mp_arena_valid(&suite.mp_arena)))
		{
			suite.mp_action_failed = 1;
			break;
		}
		if (!input_replay_suite_suspend())
		{
			suite.mp_action_failed = 1;
			break;
		}
		mp_benchmark_probe_set_suite_requested(1);
		mp_benchmark_probe_reset_controllers();
		if (!network_test_suite_begin(suite.mp_simulation, suite.map, suite.mp_game_mode,
			suite.mp_players, suite.mp_views, suite.mp_score_limit, suite.mp_time_limit_minutes))
		{
			mp_benchmark_probe_set_suite_requested(0);
			suite.mp_action_failed = 1;
		}
		else if (suite.mp_arena.columns && !benchmark_mp_arena_configure(&suite.mp_arena))
		{
			network_test_suite_end();
			mp_benchmark_probe_set_suite_requested(0);
			suite.mp_action_failed = 1;
		}
		else suite.mp_host_started = 1;
		break;
	case suite_action_mp_end:
		network_test_suite_end();
		mp_benchmark_probe_set_suite_requested(0);
		suite.mp_host_started = 0;
		break;
	default:
		break;
	}
}

static void enter(enum suite_state state)
{
	suite.state = state;
	suite.entered = halo_frame_trace_milliseconds();
}

/* the value of key= in a run's text; 0 if it has none */
static int field(const char *text, const char *key, char *value, size_t size)
{
	size_t length = strlen(key);
	const char *at = text;

	while ((at = strstr(at, key)) != NULL)
	{
		if ((at == text || at[-1] == ' ') && at[length] == '=')
		{
			size_t count = strcspn(at + length + 1, " ");

			if (count >= size)
				count = size - 1;
			memcpy(value, at + length + 1, count);
			value[count] = 0;
			return 1;
		}
		at += length;
	}
	*value = 0;
	return 0;
}

static double number_field(const char *text, const char *key)
{
	char value[32];

	return field(text, key, value, sizeof(value)) ? atof(value) : 0.0;
}

static int mp_match_valid(const struct benchmark_mp_state *state, int require_spawned)
{
	if (!strcmp(suite.mp_simulation, "network_players"))
	{
		int i;
		if (suite.mp_players < 2 || suite.mp_players > BENCHMARK_MP_ACTORS ||
			!state->multiplayer || !state->map_ok || !state->slayer || state->score_limit != suite.mp_score_limit ||
			state->time_limit_minutes != suite.mp_time_limit_minutes || state->local_players != 1 ||
			state->roster != suite.mp_players || (require_spawned && state->spawned != suite.mp_players) ||
			(require_spawned && !state->synthetic_queue_ready) || state->view_count != suite.mp_views ||
			!state->observer0 || !state->probe_active || !state->can_score || state->postgame)
			return 0;
		for (i = 0; i < suite.mp_players; i++)
			if (!state->actors[i].present || (require_spawned && !state->actors[i].spawned))
				return 0;
		return 1;
	}
	return state->multiplayer && state->map_ok && state->slayer && state->score_limit == suite.mp_score_limit &&
		state->time_limit_minutes == suite.mp_time_limit_minutes && state->local_players == 4 && state->roster == 4 &&
		(!require_spawned || state->spawned == 4) && state->view_count == suite.mp_views && state->observer0 &&
		state->probe_active && state->can_score && !state->postgame;
}

static int mp_ticks_advance(const struct benchmark_mp_state *state, double now)
{
	if (state->tick > suite.mp_last_tick)
	{
		suite.mp_last_tick = state->tick;
		suite.mp_advanced_at = now;
	}
	return now - suite.mp_advanced_at <= 10000.0;
}

static void mp_collect(const struct benchmark_mp_state *state)
{
	short local;

	if (suite.mp_previous_valid && state->projectiles_alive > 0)
		suite.mp_projectile_seen = 1;

	if (!suite.mp_previous_valid)
	{
		suite.mp_previous = *state;
		suite.mp_previous_valid = 1;
		return;
	}
	if (state->scripted_hits > suite.mp_previous.scripted_hits)
		suite.mp_scripted_hits += state->scripted_hits - suite.mp_previous.scripted_hits;
	if (state->scripted_kills > suite.mp_previous.scripted_kills)
		suite.mp_scripted_kills += state->scripted_kills - suite.mp_previous.scripted_kills;
	for (local = 0; local < BENCHMARK_MP_ACTORS; local++)
	{
		const struct benchmark_mp_actor *before = &suite.mp_previous.actors[local];
		const struct benchmark_mp_actor *after = &state->actors[local];
		float dx, dy, dz;
		double distance;

		if (!before->present || !after->present)
			continue;
		if (after->energy_projectiles >= before->energy_projectiles)
			suite.mp_energy_projectiles_actor[local] += after->energy_projectiles - before->energy_projectiles;
		else
			suite.mp_energy_invalid = 1;
		if (after->repositions > before->repositions)
			suite.mp_repositions += after->repositions - before->repositions;
		if (before->spawned && after->spawned && after->throttle > 0.2f && after->repositions == before->repositions)
		{
			dx = after->position[0] - before->position[0];
			dy = after->position[1] - before->position[1];
			dz = after->position[2] - before->position[2];
			distance = sqrt((double)dx * dx + (double)dy * dy + (double)dz * dz);
			if (distance < 3.0) /* a host gather teleport is not walking */
				suite.mp_distance[local] += distance;
		}
		if (after->fire_requests > before->fire_requests)
			suite.mp_fire[local] += after->fire_requests - before->fire_requests;
		if (after->jump_requests > before->jump_requests)
			suite.mp_jump[local] += after->jump_requests - before->jump_requests;
		if (after->grenade_requests > before->grenade_requests)
			suite.mp_grenade_requests[local] += after->grenade_requests - before->grenade_requests;
		if (before->spawned && after->spawned && before->weapon_definition == after->weapon_definition &&
			before->weapon_definition != -1 && before->ammo > after->ammo)
		{
			suite.mp_ammo_spent += (unsigned long)(before->ammo - after->ammo);
			suite.mp_ammo_spent_actor[local] += (unsigned long)(before->ammo - after->ammo);
		}
		if (before->spawned && after->spawned)
		{
			if (before->grenades[0] > after->grenades[0])
				suite.mp_grenades_used += (unsigned long)(before->grenades[0] - after->grenades[0]);
			if (before->grenades[1] > after->grenades[1])
				suite.mp_grenades_used += (unsigned long)(before->grenades[1] - after->grenades[1]);
			if (before->body > after->body || before->shield > after->shield)
				suite.mp_damage_changes++;
		}
		if (after->deaths > before->deaths)
			suite.mp_deaths += (unsigned long)(after->deaths - before->deaths);
		if (!before->spawned && after->spawned)
			suite.mp_respawns++;
	}
	suite.mp_previous = *state;
}

static int mp_evidence_note(char *note, size_t size)
{
	int moved = 0, firing = 0, local;
	unsigned long jumps = 0, grenades = 0;

	if (!strcmp(suite.mp_simulation, "network_players"))
	{
		double minimum_distance = suite.mp_distance[0];
		unsigned long remote_ammo = 0, minimum_ammo = suite.mp_ammo_spent_actor[1], energy_projectiles = 0;
		int energy_actors = 0;
		int i;

		if (suite.mp_players < 2 || suite.mp_players > BENCHMARK_MP_ACTORS)
			return 0;
		for (i = 0; i < suite.mp_players; i++)
		{
			moved += suite.mp_distance[i] >= 5.0;
			if (suite.mp_distance[i] < minimum_distance) minimum_distance = suite.mp_distance[i];
			if (i)
			{
				remote_ammo += suite.mp_ammo_spent_actor[i];
				firing += suite.mp_ammo_spent_actor[i] > 0 || suite.mp_energy_projectiles_actor[i] > 0;
				energy_projectiles += suite.mp_energy_projectiles_actor[i];
				energy_actors += suite.mp_energy_projectiles_actor[i] > 0;
				if (suite.mp_ammo_spent_actor[i] < minimum_ammo) minimum_ammo = suite.mp_ammo_spent_actor[i];
			}
		}
		snprintf(note, size, "observed roster %d spawned %d local %d views %d; "
			"synthetic host local1 synthetic%d one view; moved %d/%d (min %.1f), "
			"remote firing %d/%d ammo spent %lu (min %lu), successful energy projectiles %lu (actors %d valid %d), projectile seen %d, damage changes %lu, deaths %lu, respawns %lu; "
			"scripted hits %lu kills %lu repositions %lu; "
			"zero external clients; visible opponents require frame review",
			suite.mp_previous.roster, suite.mp_previous.spawned, suite.mp_previous.local_players,
			suite.mp_previous.view_count, suite.mp_players - 1, moved, suite.mp_players, minimum_distance,
			firing, suite.mp_players - 1, remote_ammo, minimum_ammo, energy_projectiles, energy_actors, !suite.mp_energy_invalid,
			suite.mp_projectile_seen, suite.mp_damage_changes, suite.mp_deaths, suite.mp_respawns,
			suite.mp_scripted_hits, suite.mp_scripted_kills, suite.mp_repositions);
		return !suite.mp_energy_invalid && moved == suite.mp_players && firing == suite.mp_players - 1 && suite.mp_projectile_seen &&
			suite.mp_damage_changes > 0 && suite.mp_deaths > 0 && suite.mp_respawns > 0;
	}

	for (local = 0; local < BENCHMARK_MP_ACTORS; local++)
	{
		moved += suite.mp_distance[local] >= 5.0;
		firing += suite.mp_fire[local] > 0;
		jumps += suite.mp_jump[local];
		grenades += suite.mp_grenade_requests[local];
	}
	snprintf(note, size, "observed roster %d spawned %d local %d views %d; "
		"synthetic local4 one view; moved %d/4 (%.1f,%.1f,%.1f,%.1f), fire %d/4 "
		"(%lu,%lu,%lu,%lu), jump %lu, grenade requests %lu, ammo spent %lu, grenades used %lu, "
		"damage changes %lu, deaths %lu, respawns %lu; no remote clients",
		suite.mp_previous.roster, suite.mp_previous.spawned, suite.mp_previous.local_players,
		suite.mp_previous.view_count, moved, suite.mp_distance[0], suite.mp_distance[1], suite.mp_distance[2], suite.mp_distance[3],
		firing, suite.mp_fire[0], suite.mp_fire[1], suite.mp_fire[2], suite.mp_fire[3], jumps, grenades,
		suite.mp_ammo_spent, suite.mp_grenades_used, suite.mp_damage_changes, suite.mp_deaths, suite.mp_respawns);
	return moved == 4 && firing == 4 && jumps > 0 && grenades > 0 && suite.mp_ammo_spent > 0 &&
		suite.mp_grenades_used > 0 && suite.mp_damage_changes > 0 && suite.mp_deaths > 0 && suite.mp_respawns > 0;
}

/* The window is updated every frame, but serialize only at boundaries or resolver/context changes. */
static int graphics_window_open;
static int graphics_scene, graphics_external;
static unsigned long graphics_revision;
static void benchmark_graphics_window(int open, int width, int height)
{
	int scene = halo_scene_class(), external = host_external_active();
	unsigned long revision = config_changes();
	int publish = open ? !graphics_window_open || scene != graphics_scene || external != graphics_external || revision != graphics_revision : graphics_window_open;
	if (open && !graphics_window_open) host_benchmark_window(open,width,height);
	if (publish)
	{
		char json[CONFIG_GRAPHICS_MAXIMUM_BYTES];
		if (!config_graphics_settings_json(scene,json,sizeof(json))) json[0] = 0;
		host_benchmark_graphics(json);
		graphics_scene = scene; graphics_external = external; graphics_revision = config_changes();
	}
	if (!open) host_benchmark_window(open,width,height);
	graphics_window_open = open;
}

static void benchmark_graphics_observe(int width, int height)
{
	if (graphics_window_open && (config_changes() != graphics_revision || halo_scene_class() != graphics_scene ||
		host_external_active() != graphics_external)) benchmark_graphics_window(1,width,height);
}

static void finish(const char *status, const char *note)
{
	if (graphics_window_open) benchmark_graphics_window(0,0,0);
	platform_log("benchmark suite: %s ends: %s%s%s", suite.scene, status, *note ? ", " : "", note);
	host_benchmark_end(status, note);
	input_replay_begin(NULL, 0);
	if (suite.mp_scene)
	{
		suite.pending_action = suite_action_mp_end;
		enter(suite_wait_mp_teardown);
	}
    else {
        /* Every case leaves its campaign before interstitials, upload or a native
           menu can block the frame loop. This also covers Finish between cases. */
        suite.pending_action = suite_action_menu;
        enter(suite_wait_mp_teardown);
    }
}

/* A mode change can alter both the drawable and the contextual profile.
   Build a fresh ladder only after that output committed between frames. */
static int start_suite_for_output(void)
{
    int width=0,height=0;
    platform_video_drawable_size(&width,&height);
    if(height<=0)return 0;
    double scale=config_scene_real("display.render_scale",CONFIG_SCENE_GAMEPLAY);
    long limit=config_scene_integer("display.render_height",CONFIG_SCENE_GAMEPLAY);
    long base=scale>=0.25 && scale<1.0 ? lround(height*scale) : height;
    if(limit>0 && base>limit)base=limit;
    return host_benchmark_start(config_string("debug.benchmark_suite"),height,(int)base,
        (int)config_integer("display.anisotropic_filtering"),(int)config_integer("display.shadow_map_size"),
        strcmp(config_string("display.model_lod"),"auto")!=0);
}

/* takes the next run from the host and starts it; quits when there is none */
static void next_run(void)
{
	char text[1024], path[256];
	int more = host_benchmark_next(text, sizeof(text));

    if(more==4) {
        if(!start_suite_for_output()) {
            platform_log("benchmark suite: cannot replan for the selected output");exit(EXIT_FAILURE);
        }
        return;
    }
	if (more == 3) return; /* the output switch commits between frames, before the next run */
	if (more == 2)
	{
		/* the report was shown and dismissed: the settings the runs set are the game's own again, and the player is
		where they would be without the suite, at the main menu */
		platform_log("benchmark suite: over; back to the main menu");
		halo_benchmark_overrides(0, 0, 0);
		halo_model_lod_set(-1);
		benchmark_scenario_load("", "");
		input_replay_suite_restore();
		suite.state = suite_off;
		return;
	}
	if (!more)
	{
		platform_log("benchmark suite: over; quitting");
		input_replay_suite_restore();
		exit(EXIT_SUCCESS);
	}
	field(text, "scene", suite.scene, sizeof(suite.scene));
	field(text, "map", suite.map, sizeof(suite.map));
	field(text, "difficulty", suite.difficulty, sizeof(suite.difficulty));
	field(text, "recording", suite.recording, sizeof(suite.recording));
	field(text, "simulation", suite.mp_simulation, sizeof(suite.mp_simulation));
	field(text, "game_mode", suite.mp_game_mode, sizeof(suite.mp_game_mode));
	suite.mp_players = (int)number_field(text, "players");
	suite.mp_views = (int)number_field(text, "views");
	suite.mp_score_limit = (int)number_field(text, "score_limit");
	suite.mp_time_limit_minutes = (int)number_field(text, "time_limit_minutes");
	suite.mp_arena.columns = (int)number_field(text, "arena_columns");
	suite.mp_arena.spacing = (float)number_field(text, "arena_spacing");
	suite.mp_arena.distance = (float)number_field(text, "arena_distance");
	suite.mp_arena.movement_radius = (float)number_field(text, "arena_radius");
	suite.lead = number_field(text, "lead");
	suite.seconds = number_field(text, "seconds");
	suite.lines = (long)number_field(text, "lines");
	suite.anisotropy = (long)number_field(text, "anisotropy");
	suite.shadow_size = (long)number_field(text, "shadow");
	suite.model_lod_max = (int)number_field(text, "lod");
	{
		char kind[32];

		field(text, "kind", kind, sizeof(kind));
		suite.mp_scene = !strcmp(kind, "multiplayer_synthetic");
		suite.gameplay = suite.mp_scene || !strcmp(kind, "gameplay");
		suite.in_cutscene = 0;
	}
	suite.pending_action = suite_action_none;
	suite.mp_action_failed = suite.mp_host_started = 0;
	suite.mp_fault_reported = 0;
	suite.mp_first_tick = suite.mp_ready_tick = suite.mp_last_tick = -1;
	suite.mp_menu_seen_at = suite.mp_advanced_at = 0.0;
	suite.mp_previous_valid = 0;
	memset(&suite.mp_previous, 0, sizeof(suite.mp_previous));
	memset(suite.mp_distance, 0, sizeof(suite.mp_distance));
	memset(suite.mp_fire, 0, sizeof(suite.mp_fire));
	memset(suite.mp_jump, 0, sizeof(suite.mp_jump));
	memset(suite.mp_grenade_requests, 0, sizeof(suite.mp_grenade_requests));
	suite.mp_ammo_spent = suite.mp_grenades_used = suite.mp_damage_changes = suite.mp_deaths = suite.mp_respawns = 0;
	memset(suite.mp_ammo_spent_actor, 0, sizeof(suite.mp_ammo_spent_actor));
	memset(suite.mp_energy_projectiles_actor, 0, sizeof(suite.mp_energy_projectiles_actor));
	suite.mp_energy_invalid = 0;
	suite.mp_projectile_seen = 0;
	suite.mp_repositions = suite.mp_scripted_hits = suite.mp_scripted_kills = 0;
	suite.route_length = 0;
	suite.route[0] = 0;
	suite.route_due = 0.0;
	suite.checks[0] = 0;
	suite.next_check = suite.checks;
	suite.worst_distance = 0.0;
	suite.diverged = 0;
	platform_log("benchmark suite: %s", text);
	enter(suite_loading);
	if (*suite.recording)
	{
		snprintf(path, sizeof(path), RECORDINGS "/%s.input", suite.recording);
		if (access(path, R_OK))
		{
			finish("skipped", "the catalog recording is not available in this snapshot");
			return;
		}
	}
	if (*suite.recording)
	{
		char checks_path[256];
		FILE *file;

		snprintf(checks_path, sizeof(checks_path), RECORDINGS "/%s.checks", suite.recording);
		file = fopen(checks_path, "r");
		if (file)
		{
			size_t count = fread(suite.checks, 1, sizeof(suite.checks) - 1, file);

			suite.checks[count] = 0;
			fclose(file);
		}
	}
	halo_benchmark_overrides(suite.lines, suite.shadow_size, suite.anisotropy);
	halo_model_lod_set(suite.model_lod_max);
	if (suite.mp_scene)
	{
		if (!benchmark_scenario_at_menu())
			suite.pending_action = suite_action_menu;
		return;
	}
	/* (a recording begun now waits for its own level's first tick; with none, the controller is ignored) */
	if (!input_replay_begin(*suite.recording ? path : NULL, 1))
	{
		finish("skipped", "the recording could not be read");
		return;
	}
	if (*suite.map || !benchmark_scenario_at_menu())
		benchmark_scenario_load(suite.map, suite.difficulty);
}

/* player 1's position into the run's note every ROUTE_TICKS of the recorded time, for the route check */
static void route_point(double ticks)
{
	float position[3];
	int written;

	if (ticks < suite.route_due)
		return;
	suite.route_due = ticks - fmod(ticks, ROUTE_TICKS) + ROUTE_TICKS;
	if (!benchmark_scenario_player_position(position) || suite.route_length + 48 > sizeof(suite.route))
		return;
	written = snprintf(suite.route + suite.route_length, sizeof(suite.route) - suite.route_length, "%s%.0f:%.2f,%.2f,%.2f",
		suite.route_length ? ";" : "", ticks, position[0], position[1], position[2]);
	if (written > 0)
		suite.route_length += (size_t)written;
	/* against the recording's own route: the point for this stretch of ticks, if it has one */
	while (suite.next_check && *suite.next_check)
	{
		double check_ticks, x, y, z, distance;
		const char *following = strchr(suite.next_check, ';');

		if (sscanf(suite.next_check, "%lf:%lf,%lf,%lf", &check_ticks, &x, &y, &z) != 4)
			break;
		if (check_ticks > ticks + ROUTE_TICKS / 2.0)
			break;
		suite.next_check = following ? following + 1 : NULL;
		if (check_ticks < ticks - ROUTE_TICKS / 2.0)
			continue;
		distance = sqrt((x - position[0]) * (x - position[0]) + (y - position[1]) * (y - position[1]) +
			(z - position[2]) * (z - position[2]));
		if (distance > suite.worst_distance)
			suite.worst_distance = distance;
		if (distance > ROUTE_TOLERANCE && !suite.diverged)
		{
			suite.diverged = 1;
			platform_log("benchmark suite: %s left its route at tick %.0f: %.1f world units from the recording's own position",
				suite.scene, ticks, distance);
		}
		break;
	}
}

void benchmark_suite_frame(long width, long height)
{
	double now, ticks;
	int menu = !*suite.map;

	if (suite.state == suite_off)
		return;
	if (suite.state == suite_unasked)
	{
		const char *mode = config_string("debug.benchmark_suite");
		int drawable_width = 0, drawable_height = 0;

		if (!*mode)
		{
			suite.state = suite_off;
			return;
		}
		/* ("quick:a10-crew,c40-bridge" runs only the named scenes: for trying a route, not for a report to act on) */
		if (strncmp(mode, "quick", 5) && strncmp(mode, "full", 4))
		{
			platform_log("debug.benchmark_suite: unknown value \"%s\" (\"quick\" or \"full\"); no benchmark", mode);
			suite.state = suite_off;
			return;
		}
		if (!input_replay_suite_suspend())
		{
			platform_log("benchmark suite: refused while an active recording is in progress; finish the take before starting a suite");
			suite.state = suite_off;
			return;
		}
		platform_video_drawable_size(&drawable_width, &drawable_height);
		if (drawable_height <= 0)
			drawable_height = (int)height;
		/* the settings in force for this device: what a run "at the profile's values" means. The render size is
		gameplay's (the first frame here is the menu's, which a profile may give its own) */
		{
			double scale = config_scene_real("display.render_scale", CONFIG_SCENE_GAMEPLAY);
			long limit = config_scene_integer("display.render_height", CONFIG_SCENE_GAMEPLAY);

			height = scale >= 0.25 && scale < 1.0 ? lround((double)drawable_height * scale) : drawable_height;
			if (limit > 0 && height > limit)
				height = limit;
		}
		if (!host_benchmark_start(mode, drawable_height, (int)height, (int)config_integer("display.anisotropic_filtering"),
			(int)config_integer("display.shadow_map_size"), strcmp(config_string("display.model_lod"), "auto") != 0))
		{
			platform_log("benchmark suite: the app refused to start it; quitting");
			input_replay_suite_restore();
			exit(EXIT_FAILURE);
		}
		enter(suite_starting);
	}
    /* A control query through the existing import does not advance the planner.
       Finish uses the normal case end and multiplayer teardown before returning. */
    if (suite.state != suite_off && suite.state != suite_unasked &&
        host_benchmark_next(NULL, 0) == 5) {
        if (suite.state == suite_loading || suite.state == suite_lead || suite.state == suite_measuring) {
            finish("partial", "Finish requested; this case contains only the available partial measurements");
            return;
        }
        if (suite.state == suite_starting) enter(suite_next);
    }
	now = halo_frame_trace_milliseconds();
	/* The menu scenario can render while its filesystem worker still reads
	menu tags. Keep the frame loop running so the UI can join that worker,
	and recheck on the next-run handoff before requesting any map change. */
	if ((suite.state == suite_starting || suite.state == suite_next) && filesystem_check_thread_is_active())
	{
		if (now - suite.entered > START_LIMIT_MILLISECONDS)
		{
			platform_log("benchmark suite: startup failed: filesystem initialization did not finish; no map requested");
			exit(EXIT_FAILURE);
		}
		return;
	}
	ticks = input_replay_timeline();
	switch (suite.state)
	{
	case suite_starting:
		/* the first run waits for the game to have finished starting: the main menu loaded and settled. A map
		asked for on the launch's first frames (a suite that carries on at a level) crashed the game */
		if ((benchmark_scenario_at_menu() && now - suite.entered >= MENU_SETTLE_MILLISECONDS) ||
			now - suite.entered > START_LIMIT_MILLISECONDS)
		{
			platform_log("benchmark suite: startup filesystem worker is inactive");
			enter(suite_next);
		}
		break;
	case suite_next:
		next_run();
		break;
	case suite_loading:
		if (suite.mp_scene)
		{
			struct benchmark_mp_state state;

			if (suite.mp_action_failed || network_test_suite_failed())
				finish("failed", "the private match or synthetic player could not start");
			else if (!suite.mp_host_started)
			{
				if (!benchmark_scenario_at_menu())
				{
					suite.mp_menu_seen_at = 0.0;
					if (suite.pending_action == suite_action_none)
						suite.pending_action = suite_action_menu;
				}
				else
				{
					if (!suite.mp_menu_seen_at)
						suite.mp_menu_seen_at = now;
					if (now - suite.mp_menu_seen_at >= MENU_SETTLE_MILLISECONDS &&
						suite.pending_action == suite_action_none)
						suite.pending_action = suite_action_mp_begin;
				}
			}
			else
			{
				benchmark_scenario_multiplayer_state(&state);
				if (state.multiplayer && (!state.map_ok || !state.slayer || state.score_limit != suite.mp_score_limit ||
					state.time_limit_minutes != suite.mp_time_limit_minutes || state.postgame))
					finish("failed", "wrong multiplayer map, variant, limit, or game state");
				else if (mp_match_valid(&state, 1))
				{
					if (suite.mp_first_tick < 0)
					{
						suite.mp_first_tick = suite.mp_last_tick = state.tick;
						suite.mp_advanced_at = now;
					}
					else if (!mp_ticks_advance(&state, now))
						finish("failed", "multiplayer game ticks stopped before warmup");
					else if (state.tick - suite.mp_first_tick >= 30)
					{
						if (!benchmark_mp_arena_begin_combat())
							finish("failed", "the arena could not enable combat after host readiness");
						else
						{
							suite.mp_ready_tick = state.tick;
							enter(suite_lead);
						}
					}
				}
			}
			if (suite.state == suite_loading && now - suite.entered > LOAD_LIMIT_MILLISECONDS)
				finish("failed", "multiplayer game did not become ready");
			break;
		}
		/* loaded: the menu's scenario for a menu scene, or the level's game time running (a replay begun during
		another level holds the recorded time negative until its own level starts) */
		if (menu ? benchmark_scenario_at_menu() && now - suite.entered >= MENU_SETTLE_MILLISECONDS :
			ticks >= 0.0 && !benchmark_scenario_at_menu())
			enter(suite_lead);
		else if (now - suite.entered > LOAD_LIMIT_MILLISECONDS)
			finish("failed", "the map did not start");
		break;
	case suite_lead:
		if (suite.mp_scene)
		{
			struct benchmark_mp_state state;

			benchmark_scenario_multiplayer_state(&state);
			if (!mp_match_valid(&state, 0) || !mp_ticks_advance(&state, now))
				finish("failed", "multiplayer match or ticks stopped during warmup");
			else if (state.tick - suite.mp_ready_tick >= (long)(suite.lead * 30.0))
			{
				suite.opened = now;
				suite.opened_ticks = state.tick;
				mp_collect(&state);
				benchmark_graphics_window(1, (int)width, (int)height);
				enter(suite_measuring);
			}
			else if (now - suite.entered > LOAD_LIMIT_MILLISECONDS + suite.lead * 3000.0)
				finish("failed", "multiplayer warmup did not finish");
			break;
		}
		/* the route is followed from the level's first tick, so a replay that leaves it during the lead-in is
		caught before anything is measured */
		if (!menu)
			route_point(ticks);
		if (suite.diverged)
			finish("diverged", suite.route);
		else if (menu ? now - suite.entered >= suite.lead * 1000.0 : ticks >= suite.lead * 30.0)
		{
			suite.opened = now;
			suite.opened_ticks = ticks;
			suite.in_cutscene = suite.gameplay && halo_scene_class() == CONFIG_SCENE_CUTSCENE;
			benchmark_graphics_window(!suite.in_cutscene, (int)width, (int)height);
			enter(suite_measuring);
		}
		else if (*suite.recording && input_replay_ended())
			finish("failed", "the recording ended before the measured window");
		else if (now - suite.entered > LOAD_LIMIT_MILLISECONDS + suite.lead * 1000.0)
			finish("failed", "the lead-in never ended");
		break;
	case suite_measuring:
		benchmark_graphics_observe((int)width,(int)height);
		if (suite.mp_scene)
		{
			struct benchmark_mp_state state;

			benchmark_scenario_multiplayer_state(&state);
			if (!mp_match_valid(&state, 0) || !mp_ticks_advance(&state, now))
			{
				benchmark_graphics_window(0, (int)width, (int)height);
				finish("failed", "multiplayer match or ticks stopped during measurement");
			}
			else
			{
				mp_collect(&state);
				if (state.tick - (long)suite.opened_ticks >= (long)(suite.seconds * 30.0))
				{
					char note[512];
					int useful = mp_evidence_note(note, sizeof(note));

					benchmark_graphics_window(0, (int)width, (int)height);
					finish(useful ? "ok" : "failed", note);
				}
				else if (now - suite.opened > suite.seconds * 3000.0 + REPLAY_OVERRUN_MILLISECONDS)
				{
					benchmark_graphics_window(0, (int)width, (int)height);
					finish("failed", "multiplayer measurement overran its tick window");
				}
			}
			break;
		}
		if (!menu)
			route_point(ticks);
		if (suite.gameplay && (halo_scene_class() == CONFIG_SCENE_CUTSCENE) != suite.in_cutscene)
		{
			suite.in_cutscene = !suite.in_cutscene;
			benchmark_graphics_window(!suite.in_cutscene, (int)width, (int)height);
		}
		if (suite.diverged)
		{
			benchmark_graphics_window(0, (int)width, (int)height);
			finish("diverged", suite.route);
		}
		else if (menu ? now - suite.opened >= suite.seconds * 1000.0 :
			suite.seconds > 0.0 ? ticks - suite.opened_ticks >= suite.seconds * 30.0 : input_replay_ended())
		{
			benchmark_graphics_window(0, (int)width, (int)height);
			finish("ok", suite.route);
		}
		else if (*suite.recording && suite.seconds > 0.0 && input_replay_ended())
		{
			benchmark_graphics_window(0, (int)width, (int)height);
			finish("ok", suite.route);
		}
		else if (now - suite.opened > (suite.seconds > 0.0 ? suite.seconds * 1000.0 * 3.0 :
			input_replay_length_ticks() * 1000.0 / 30.0) + REPLAY_OVERRUN_MILLISECONDS)
		{
			/* the game time stopped (a death the recording did not have, a script waiting on something else):
			the replay left its route */
			benchmark_graphics_window(0, (int)width, (int)height);
			finish("diverged", suite.route);
		}
		break;
	case suite_wait_mp_teardown:
		if (!suite.mp_host_started && suite.pending_action == suite_action_none && benchmark_scenario_at_menu())
			enter(suite_next);
		else if (now - suite.entered > LOAD_LIMIT_MILLISECONDS)
		{
			platform_log("benchmark suite: scene teardown did not return to menu");
			exit(EXIT_FAILURE);
		}
		break;
	default:
		break;
	}
}

#else

void benchmark_suite_frame(long width, long height)
{
	(void)width;
	(void)height;
}

#endif
