/*
LOADING_SCREEN.C

The loading screen, held over a campaign level's whole load.

The game's loading screen (source/interface/progress_bar.c) is the last
frame blurring away under a gray picture of the ring that a band of light
crosses from left to right as the load goes, over four loops of a low tone.
The Xbox showed it while a level was copied from the disc to the hard
disk's cache, which took as long as the level's load did there. Here that
copy happens once for each level and takes a fraction of a second, so the
screen was all but never seen, and what a level's start waits for is
something else: reading the level (game_load) and then the Metal backend
compiling the shaders and pipelines its list names (halo_shader_list_warm),
during which the first frames had draws missing.

So a campaign level's load holds the screen itself:

- halo_loading_screen_begin, as main_change_map_name starts the change (a
  level picked in the menus, the next level at a level's end, a checkpoint
  or a saved game resumed: they all load the level's map), starts the screen
  over the last frame and shows its first frame;
- the level is unloaded, copied if it never was, and loaded. The game
  thread does that work, so the picture stands still for it (0.07 to 0.1 s
  measured on an M6 Mac, more on slower devices); the tone plays on, mixed
  on the audio thread;
- halo_loading_screen_finish, at the end of main_new_map, keeps drawing the
  screen, sixty frames a second as the Xbox did, until the shaders and
  pipelines still compiling on the worker threads (gpu_compiles_outstanding)
  are done and game.loading_screen_seconds have passed since it began. No
  game tick runs meanwhile and sound_render is not called
  (halo_loading_screen_holding), so the level's opening script, its music
  and its first frame all start after the screen has gone.

The band's place is the load's progress: the lesser of the time's share of
the minimum and the work's share done (the read, then the compiles), so it
crosses in the minimum time when everything is ready early and follows the
compiles when they take longer (a first launch compiles every pipeline: 32 s
on an Apple TV). The screen leaves as the original does: the picture and
the blur fade to black as the progress reaches its end, and the level's
first frame follows.

The picture is the player's own (port/runtime/loading_picture.h); without
it the screen is the blur and the tone alone.

game.loading_screen: "auto" (the default) shows it except where a run must
draw the same frames every time (debug.fixed_timestep, debug.input_replay,
debug.benchmark, debug.exit_after, debug.screenshot_every: the same runs
that compile shaders at their first draw) and except in stereo, where it has
not been seen on a headset; "on" and "off" force it.
With display.async_shaders off (those same runs, or the setting) the compiles
block halo_shader_list_warm on the game thread, so a forced-on screen stands
still for them: a run that wants to see the screen as a player does sets both
game.loading_screen = "on" and display.async_shaders = "on".
*/

#include "cseries.h"
#include "game/game.h"
#include "interface/progress_bar.h"
#include "main/main.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "../src/halo_stereo.h"

#include <string.h>

void platform_log(const char *format, ...);
int platform_fixed_timestep(void);
int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
const char *config_string(const char *name);
/* port/linux/src/gpu.h */
unsigned int gpu_compiles_outstanding(void);
/* source/interface/progress_bar.c: the progress the screen last drew */
extern real last_t;
void halo_main_time_restart(void);
short main_get_solo_level_from_name(char const *name);
unsigned long system_milliseconds(void);
void __stdcall Sleep(unsigned long milliseconds);

enum
{
	/* the screen's frames, as the Xbox's display paced them */
	FRAME_MILLISECONDS = 16,
	/* compiles that make no headway for this long are given up on */
	STALLED_MILLISECONDS = 60000,
};

/* the shares of the progress the level's read and the compiles take */
#define READ_SHARE 0.15f

static struct
{
	boolean active;        /* from begin to the end of finish */
	boolean holding;       /* finish's loop: the level is loaded and waits */
	unsigned long started; /* system_milliseconds at begin */
	unsigned long minimum; /* milliseconds */
	real work;             /* 0 to 1: how much of the load is done */
} loading_screen;

static boolean loading_screen_wanted(void)
{
	const char *mode = config_string("game.loading_screen");
	const char *reason = platform_fixed_timestep() ? "debug.fixed_timestep" :
		*config_string("debug.input_replay") ? "debug.input_replay" :
		config_boolean("debug.benchmark") ? "debug.benchmark" :
		config_real("debug.exit_after") > 0.0 ? "debug.exit_after" :
		config_integer("debug.screenshot_every") > 0 ? "debug.screenshot_every" :
		halo_stereo_frame()->mode != HALO_STEREO_OFF ? "stereo" : NULL;
	static boolean logged = FALSE;

	if (!strcmp(mode, "on"))
		return TRUE;
	if (!strcmp(mode, "off"))
		return FALSE;
	if (strcmp(mode, "auto") && !logged)
		platform_log("game.loading_screen: unknown value \"%s\"; using \"auto\"", mode);
	if (reason && !logged)
		platform_log("game.loading_screen: off for this run (%s)", reason);
	logged = TRUE;
	return reason == NULL;
}

static void loading_screen_frame(void)
{
	unsigned long frame_started = system_milliseconds(), elapsed;

	main_pregame_render();
	main_rasterizer_throttle();
	main_present_frame();
	elapsed = system_milliseconds() - frame_started;
	if (elapsed < FRAME_MILLISECONDS)
		Sleep(FRAME_MILLISECONDS - elapsed);
}

void halo_loading_screen_begin(char const *map_name)
{
	double seconds = config_real("game.loading_screen_seconds");

	/* (the campaign's own levels; a multiplayer map loads through the network
	game's lobby, and a Custom Edition level starts as before) */
	if (loading_screen.active || main_get_solo_level_from_name(map_name) == NONE || !loading_screen_wanted())
		return;
	if (seconds < 0.0)
		seconds = 0.0;
	if (seconds > 30.0)
		seconds = 30.0;
	memset(&loading_screen, 0, sizeof(loading_screen));
	loading_screen.active = TRUE;
	loading_screen.started = system_milliseconds();
	loading_screen.minimum = (unsigned long)(seconds * 1000.0);
	platform_log("loading screen: %s, at least %.1f s", map_name, seconds);
	/* over the last frame (a map is loaded: the menu's or the level left) */
	progress_bar_begin(global_scenario_index != NONE);
	loading_screen_frame();
}

/* whether the loading screen is up, and the progress it draws
(render_frame_pregame) */
boolean halo_loading_screen_progress(real *progress)
{
	unsigned long elapsed;
	real time_share, work = loading_screen.work;

	if (!loading_screen.active)
		return FALSE;
	/* (game_precache_new_map ends the progress bar after a copy) */
	if (!progress_bar_is_active())
		progress_bar_begin(TRUE);
	/* while a level is copied, the copy's own progress is the read's first half */
	if (!loading_screen.holding)
	{
		real copy = 0.0f;

		work = game_map_loading_in_progress(&copy) ? 0.5f * READ_SHARE * copy : 0.0f;
	}
	elapsed = system_milliseconds() - loading_screen.started;
	time_share = loading_screen.minimum ? (real)elapsed / (real)loading_screen.minimum : 1.0f;
	*progress = MIN(time_share, work);
	/* (progress_bar_display draws nothing at 0) */
	*progress = PIN(*progress, 0.001f, 1.0f);
	return TRUE;
}

/* the level is loaded and the screen holds its start: no game sound is
rendered behind it (main_pregame_render) */
boolean halo_loading_screen_holding(void)
{
	return loading_screen.holding;
}

void halo_loading_screen_finish(void)
{
	unsigned long peak, outstanding, last_outstanding, last_change, now, frames = 0;
	unsigned long loaded;
	struct scenario *scenario;

	if (!loading_screen.active)
		return;
	scenario = global_scenario_index != NONE ? global_scenario_get() : NULL;
	loaded = system_milliseconds();
	peak = last_outstanding = gpu_compiles_outstanding();
	last_change = loaded;
	/* (only a campaign level waits: a failed load that fell back to the
	menus' map ends the screen at once) */
	loading_screen.holding = scenario && scenario->type == _scenario_type_solo;
	while (loading_screen.holding)
	{
		outstanding = gpu_compiles_outstanding();
		now = system_milliseconds();
		if (outstanding > peak)
			peak = outstanding;
		if (outstanding != last_outstanding)
		{
			last_outstanding = outstanding;
			last_change = now;
		}
		else if (outstanding && now - last_change > STALLED_MILLISECONDS)
		{
			platform_log("loading screen: %lu compiles made no headway for %d s; starting the level",
				outstanding, STALLED_MILLISECONDS / 1000);
			break;
		}
		loading_screen.work = peak ? READ_SHARE + (1.0f - READ_SHARE) * (real)(peak - outstanding) / (real)peak : 1.0f;
		/* done once the work is, the minimum has passed and the screen has
		drawn its end: the picture and the blur faded to black */
		if (!outstanding && now - loading_screen.started >= loading_screen.minimum && frames && last_t >= 0.985f)
			break;
		loading_screen_frame();
		frames++;
	}
	now = system_milliseconds();
	platform_log("loading screen: up for %lu ms: %lu ms to load the level, then %lu ms and %lu frames while %lu "
		"shaders and pipelines compiled", now - loading_screen.started, loaded - loading_screen.started, now - loaded,
		frames, peak);
	loading_screen.holding = FALSE;
	loading_screen.active = FALSE;
	progress_bar_end();
	/* the level's first frame takes its time from here, not from before the load */
	halo_main_time_restart();
}
