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
/* 1: a run to carry out; 0: the suite is over and the game quits; 2: over, and the game goes back to its main menu */
int host_benchmark_next(char *text, unsigned int size);
void host_benchmark_window(int open, int width, int height);
void host_benchmark_end(const char *status, const char *note);
/* input_replay.c */
int input_replay_begin(const char *name, int benchmark_lip_sync);
int input_replay_ended(void);
long input_replay_segment(void);
double input_replay_length_ticks(void);
double input_replay_timeline(void);
/* port/linux/game/benchmark_scenario.c */
void benchmark_scenario_load(const char *map, const char *difficulty);
unsigned char benchmark_scenario_at_menu(void);
unsigned char benchmark_scenario_player_position(float *position);
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

enum suite_state { suite_unasked, suite_off, suite_starting, suite_next, suite_loading, suite_lead, suite_measuring };

static struct
{
	enum suite_state state;
	/* the run in hand */
	char scene[48], map[32], difficulty[16], recording[64];
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
	input_replay_begin(NULL, 0);
	enter(suite_next);
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
	char text[512], path[256];
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
		suite.state = suite_off;
		return;
	}
	if (!more)
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
	{
		char kind[16];

		suite.gameplay = field(text, "kind", kind, sizeof(kind)) && !strcmp(kind, "gameplay");
		suite.in_cutscene = 0;
	}
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
			finish("skipped", "the recording is not on this device");
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
			exit(EXIT_FAILURE);
		}
		enter(suite_starting);
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
		/* loaded: the menu's scenario for a menu scene, or the level's game time running (a replay begun during
		another level holds the recorded time negative until its own level starts) */
		if (menu ? benchmark_scenario_at_menu() && now - suite.entered >= MENU_SETTLE_MILLISECONDS :
			ticks >= 0.0 && !benchmark_scenario_at_menu())
			enter(suite_lead);
		else if (now - suite.entered > LOAD_LIMIT_MILLISECONDS)
			finish("failed", "the map did not start");
		break;
	case suite_lead:
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
			host_benchmark_window(!suite.in_cutscene, (int)width, (int)height);
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
		if (suite.gameplay && (halo_scene_class() == CONFIG_SCENE_CUTSCENE) != suite.in_cutscene)
		{
			suite.in_cutscene = !suite.in_cutscene;
			host_benchmark_window(!suite.in_cutscene, (int)width, (int)height);
		}
		if (suite.diverged)
		{
			host_benchmark_window(0, (int)width, (int)height);
			finish("diverged", suite.route);
		}
		else if (menu ? now - suite.opened >= suite.seconds * 1000.0 :
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
