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

The band's place is the load's progress, which moves smoothly and never
back. Its target is the lesser of the time's share of the minimum and the
work's share done, both scaled to end at COMPILE_END: the copy's first
half (0 to 0.075), the read (to READ_SHARE) and the compiles (to
COMPILE_END). While the work stalls (the compiles of a first launch take
32 s on an Apple TV) the target creeps on toward the next boundary, a
little each second, so the band does not stop. The displayed value eases
toward the target with a time constant, so the frame rate does not matter.
Once the work is done and the minimum has passed, the screen goes on into
the level's start (the tail): the level's first ticks run behind it (the
opening script needs three to start its cutscene; main_loop draws this
screen in place of the game, never the player's start), game time stands
still once they have, and the displayed value eases the rest of the way to
1, where the picture and the blur have faded to black, as the original's
do. The level's first frame follows that.

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

#include <math.h>
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
	/* the most the screen stays, once the load is done, to draw its end */
	END_MILLISECONDS = 500,
};

/* the shares of the progress the level's read and the compiles take */
#define READ_SHARE 0.15f
/* the work and the minimum time end here; the tail fills the rest */
#define COMPILE_END 0.9f
/* the displayed progress eases toward its target with this time constant */
#define EASE_SECONDS 0.15f
/* ... and toward the end, over the tail, with this one */
#define END_EASE_SECONDS 0.1f
/* while the work stalls the target creeps on by this much each second, to
at most CREEP_MAX over the work */
#define CREEP_PER_SECOND 0.02f
#define CREEP_MAX 0.06f
/* the shortest run from the loaded level to the end of the compiles' range */
#define RUN_MILLISECONDS 600
/* the tail gives up after this long, however far the level's ticks have got */
#define TAIL_MAXIMUM_MILLISECONDS 3000
/* the level's ticks the tail waits for (cinematic_screen.c) */
#define TAIL_TICKS 3

static struct
{
	boolean active;        /* from begin to the end of finish */
	boolean holding;       /* finish's loop: the level is loaded and waits */
	boolean tail;          /* the level's first ticks run behind the screen */
	unsigned long tail_started;
	unsigned long tail_frozen; /* system_milliseconds when game time stood still */
	unsigned long started; /* system_milliseconds at begin */
	unsigned long minimum; /* milliseconds */
	real work;             /* 0 to 1: how much of the load is done */
	real shown;            /* the displayed progress */
	unsigned long updated; /* system_milliseconds of the last update */
	unsigned long work_changed;
	unsigned long loaded;  /* system_milliseconds when the level was loaded */
	real last_work_target;
	unsigned long logged;
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
	unsigned long now, elapsed, dt;
	real time_share, work_target, target, time_target, seconds;

	if (!loading_screen.active)
		return FALSE;
	/* (game_precache_new_map ends the progress bar after a copy) */
	if (!progress_bar_is_active())
		progress_bar_begin(TRUE);
	now = system_milliseconds();
	elapsed = now - loading_screen.started;
	dt = loading_screen.updated ? now - loading_screen.updated : 0;
	if (dt > 100)
		dt = 100;
	loading_screen.updated = now ? now : 1;
	seconds = (real)dt / 1000.0f;
	if (loading_screen.tail)
	{
		target = 1.0f;
		seconds /= END_EASE_SECONDS;
	}
	else
	{
		/* while a level is copied, the copy's own progress is the read's first half */
		if (!loading_screen.holding)
		{
			real copy = 0.0f;

			work_target = game_map_loading_in_progress(&copy) ? 0.5f * READ_SHARE * copy : 0.0f;
		}
		else
			work_target = READ_SHARE + (COMPILE_END - READ_SHARE) * loading_screen.work;
		if (loading_screen.holding && !loading_screen.minimum)
			time_target = COMPILE_END;
		else if (loading_screen.holding)
		{
			/* the minimum time's remainder after the load, at least RUN_MILLISECONDS, so
			a load that took the whole minimum still has a run to the end */
			unsigned long left = loading_screen.minimum > loading_screen.loaded - loading_screen.started ?
				loading_screen.minimum - (loading_screen.loaded - loading_screen.started) : 0;

			if (left < RUN_MILLISECONDS)
				left = RUN_MILLISECONDS;
			time_share = (real)(now - loading_screen.loaded) / (real)left;
			time_target = READ_SHARE + (COMPILE_END - READ_SHARE) * MIN(time_share, 1.0f);
		}
		else
			time_target = READ_SHARE * MIN(loading_screen.minimum ? (real)elapsed / (real)loading_screen.minimum : 1.0f, 1.0f);
		if (work_target > loading_screen.last_work_target + 0.0005f || !loading_screen.work_changed)
		{
			loading_screen.last_work_target = work_target;
			loading_screen.work_changed = now;
		}
		target = work_target;
		if (work_target < COMPILE_END && work_target < time_target)
		{
			/* the work stalls: creep on, slowly */
			real creep = CREEP_PER_SECOND * (real)(now - loading_screen.work_changed) / 1000.0f;

			target = work_target + MIN(creep, CREEP_MAX);
			if (target > COMPILE_END)
				target = COMPILE_END;
		}
		if (target > time_target)
			target = time_target;
		seconds /= EASE_SECONDS;
	}
	if (target > loading_screen.shown)
		loading_screen.shown += (target - loading_screen.shown) * (1.0f - (real)exp(-seconds));
	if (loading_screen.shown > 1.0f)
		loading_screen.shown = 1.0f;
	*progress = loading_screen.shown;
	if (now - loading_screen.logged >= 100)
	{
		loading_screen.logged = now;
		platform_log("loading screen: progress %.3f (target %.3f, %s) at %lu ms, game time %d", *progress, target,
			loading_screen.tail ? "tail" : loading_screen.holding ? "compiles" : "read", elapsed,
			game_in_progress() ? (int)game_time_get() : -1);
	}
	/* (progress_bar_display draws nothing at 0) */
	*progress = PIN(*progress, 0.001f, 1.0f);
	return TRUE;
}

/* 1 when the screen draws the progress itself, without the original's
wobble and per-frame lag (progress_bar_render) */
boolean halo_loading_screen_smooth(void)
{
	return loading_screen.active;
}

/* 1 while the screen stands in for the level's first frames (main_loop) */
boolean halo_loading_screen_tail(void)
{
	return loading_screen.active && loading_screen.tail;
}

/* main_loop, after each frame the tail drew: the level's ticks are running
behind the screen; when they have run, game time stands still until the
screen has faded out, so the level's opening starts as the screen goes */
void halo_loading_screen_tail_frame(void)
{
	unsigned long now = system_milliseconds();

	if (!halo_loading_screen_tail())
		return;
	if (game_time_get() >= TAIL_TICKS)
	{
		if (!loading_screen.tail_frozen)
			loading_screen.tail_frozen = now ? now : 1;
		halo_main_time_restart();
	}
	if ((loading_screen.tail_frozen && (last_t >= 0.985f || now - loading_screen.tail_frozen >= END_MILLISECONDS)) ||
		now - loading_screen.tail_started >= TAIL_MAXIMUM_MILLISECONDS)
	{
		platform_log("loading screen: the tail ended at %.3f after %lu ms, game time %d", loading_screen.shown,
			now - loading_screen.tail_started, (int)game_time_get());
		loading_screen.tail = FALSE;
		loading_screen.active = FALSE;
		progress_bar_end();
		halo_main_time_restart();
	}
}

/* the level is loaded and the screen holds its start: no game sound is
rendered behind it (main_pregame_render) */
boolean halo_loading_screen_holding(void)
{
	return loading_screen.holding || halo_loading_screen_tail();
}

void halo_loading_screen_finish(void)
{
	unsigned long peak, outstanding, last_outstanding, last_change, now, frames = 0;
	unsigned long loaded, finished = 0;
	struct scenario *scenario;

	if (!loading_screen.active)
		return;
	scenario = global_scenario_index != NONE ? global_scenario_get() : NULL;
	loaded = system_milliseconds();
	loading_screen.loaded = loaded;
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
		loading_screen.work = peak ? (real)(peak - outstanding) / (real)peak : 1.0f;
		/* done once the work is and the minimum has passed, and the bar has
		eased up to where the work ends; the tail fills the rest */
		if (!outstanding && now - loading_screen.started >= loading_screen.minimum && (now - loaded >= RUN_MILLISECONDS || !loading_screen.minimum) && frames)
		{
			if (!finished)
				finished = now ? now : 1;
			if (loading_screen.shown >= COMPILE_END - 0.01f || now - finished >= END_MILLISECONDS)
				break;
		}
		else
			finished = 0;
		loading_screen_frame();
		frames++;
	}
	now = system_milliseconds();
	platform_log("loading screen: up for %lu ms: %lu ms to load the level, then %lu ms and %lu frames while %lu "
		"shaders and pipelines compiled", now - loading_screen.started, loaded - loading_screen.started, now - loaded,
		frames, peak);
	loading_screen.holding = FALSE;
	/* the tail: the screen goes on over the level's first ticks (a campaign
	level that loaded, not one that fell back to the menus) */
	if (scenario && scenario->type == _scenario_type_solo && outstanding == 0 && game_in_progress())
	{
		loading_screen.tail = TRUE;
		loading_screen.tail_started = now ? now : 1;
		loading_screen.tail_frozen = 0;
	}
	else
	{
		loading_screen.active = FALSE;
		progress_bar_end();
	}
	/* the level's first frame takes its time from here, not from before the load */
	halo_main_time_restart();
}
