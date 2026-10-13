/*
BENCHMARK_SUITE.C

The device benchmark's suite, inside the game: debug.benchmark_suite =
"quick" or "full" plays the suite's scenes by itself, several times over at
different settings, and quits.

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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HALO_IOS

/* port/runtime/guest/runtime/guest_host.h */
int host_benchmark_start(const char *mode, int drawable_lines, int base_lines, int anisotropy, int shadow_size,
	int model_lod_max);
int host_benchmark_next(char *text, unsigned int size);
void host_benchmark_window(int open, int width, int height);
void host_benchmark_end(const char *status, const char *note);
/* input_replay.c */
int input_replay_begin(const char *name);
int input_replay_ended(void);
long input_replay_segment(void);
double input_replay_length_ticks(void);
double input_replay_timeline(void);
/* port/linux/game/benchmark_scenario.c */
void benchmark_scenario_load(const char *map, const char *difficulty);
unsigned char benchmark_scenario_at_menu(void);
unsigned char benchmark_scenario_player_position(float *position);
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
/* the menu fades in and its music starts before its frames settle */
#define MENU_SETTLE_MILLISECONDS 3000.0
/* the route: player 1's position every this many ticks of the recorded time */
#define ROUTE_TICKS 300.0

enum suite_state { suite_unasked, suite_off, suite_next, suite_loading, suite_lead, suite_measuring };

static struct
{
	enum suite_state state;
	/* the run in hand */
	char scene[48], map[32], difficulty[16], recording[64];
	double lead, seconds;
	long lines, anisotropy, shadow_size;
	int model_lod_max;
	/* real time: when the state was entered; when the window opened */
	double entered, opened;
	/* the recorded time when the window opened (a level), and the next route point's */
	double opened_ticks, route_due;
	char route[1536];
	size_t route_length;
} suite;

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

static void finish(const char *status, const char *note)
{
	platform_log("benchmark suite: %s ends: %s%s%s", suite.scene, status, *note ? ", " : "", note);
	host_benchmark_end(status, note);
	input_replay_begin(NULL);
	enter(suite_next);
}

/* takes the next run from the host and starts it; quits when there is none */
static void next_run(void)
{
	char text[512], path[256];

	if (!host_benchmark_next(text, sizeof(text)))
	{
		platform_log("benchmark suite: over; quitting");
		exit(EXIT_SUCCESS);
	}
	field(text, "scene", suite.scene, sizeof(suite.scene));
	field(text, "map", suite.map, sizeof(suite.map));
	field(text, "difficulty", suite.difficulty, sizeof(suite.difficulty));
	field(text, "recording", suite.recording, sizeof(suite.recording));
	suite.lead = number_field(text, "lead");
	suite.seconds = number_field(text, "seconds");
	suite.lines = (long)number_field(text, "lines");
	suite.anisotropy = (long)number_field(text, "anisotropy");
	suite.shadow_size = (long)number_field(text, "shadow");
	suite.model_lod_max = (int)number_field(text, "lod");
	suite.route_length = 0;
	suite.route[0] = 0;
	suite.route_due = 0.0;
	platform_log("benchmark suite: %s", text);
	enter(suite_loading);
	if (*suite.recording)
	{
		snprintf(path, sizeof(path), RECORDINGS "/%s.input", suite.recording);
		if (access(path, R_OK))
		{
			finish("skipped", "the recording is not on this device");
			return;
		}
	}
	halo_benchmark_overrides(suite.lines, suite.shadow_size, suite.anisotropy);
	halo_model_lod_set(suite.model_lod_max);
	/* (a recording begun now waits for its own level's first tick; with none, the controller is ignored) */
	if (!input_replay_begin(*suite.recording ? path : NULL))
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
		if (strcmp(mode, "quick") && strcmp(mode, "full"))
		{
			platform_log("debug.benchmark_suite: unknown value \"%s\" (\"quick\" or \"full\"); no benchmark", mode);
			suite.state = suite_off;
			return;
		}
		platform_video_drawable_size(&drawable_width, &drawable_height);
		if (drawable_height <= 0)
			drawable_height = (int)height;
		/* the settings in force for this device: what a run "at the profile's values" means */
		if (!host_benchmark_start(mode, drawable_height, (int)height, (int)config_integer("display.anisotropic_filtering"),
			(int)config_integer("display.shadow_map_size"), strcmp(config_string("display.model_lod"), "auto") != 0))
		{
			platform_log("benchmark suite: the app refused to start it; quitting");
			exit(EXIT_FAILURE);
		}
		enter(suite_next);
	}
	now = halo_frame_trace_milliseconds();
	ticks = input_replay_timeline();
	switch (suite.state)
	{
	case suite_next:
		next_run();
		break;
	case suite_loading:
		/* loaded: the menu's scenario for a menu scene, or the level's game time running (a replay begun during
		another level holds the recorded time negative until its own level starts) */
		if (menu ? benchmark_scenario_at_menu() && now - suite.entered >= MENU_SETTLE_MILLISECONDS :
			ticks >= 0.0 && !benchmark_scenario_at_menu())
			enter(suite_lead);
		else if (now - suite.entered > LOAD_LIMIT_MILLISECONDS)
			finish("failed", "the map did not start");
		break;
	case suite_lead:
		if (menu ? now - suite.entered >= suite.lead * 1000.0 : ticks >= suite.lead * 30.0)
		{
			suite.opened = now;
			suite.opened_ticks = ticks;
			host_benchmark_window(1, (int)width, (int)height);
			enter(suite_measuring);
		}
		else if (*suite.recording && input_replay_ended())
			finish("failed", "the recording ended before the measured window");
		else if (now - suite.entered > LOAD_LIMIT_MILLISECONDS + suite.lead * 1000.0)
			finish("failed", "the lead-in never ended");
		break;
	case suite_measuring:
		if (!menu)
			route_point(ticks);
		if (menu ? now - suite.opened >= suite.seconds * 1000.0 :
			suite.seconds > 0.0 ? ticks - suite.opened_ticks >= suite.seconds * 30.0 : input_replay_ended())
		{
			host_benchmark_window(0, (int)width, (int)height);
			finish("ok", suite.route);
		}
		else if (*suite.recording && suite.seconds > 0.0 && input_replay_ended())
		{
			host_benchmark_window(0, (int)width, (int)height);
			finish("ok", suite.route);
		}
		else if (now - suite.opened > (suite.seconds > 0.0 ? suite.seconds * 1000.0 * 3.0 :
			input_replay_length_ticks() * 1000.0 / 30.0) + REPLAY_OVERRUN_MILLISECONDS)
		{
			/* the game time stopped (a death the recording did not have, a script waiting on something else):
			the replay left its route */
			host_benchmark_window(0, (int)width, (int)height);
			finish("diverged", suite.route);
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
