/*
SCENE_CLASS.C

Which scene class the frame about to be drawn is (port/linux/src/port_config.h:
CONFIG_SCENE_MENU, _CUTSCENE or _GAMEPLAY), for the settings a device profile
gives a class its own value ([scene.menu.display] in profiles/*.toml). The
menu and cutscenes are the sequences whose cost is known in advance, the
camera and the content being scripted, so a profile can budget them apart
from play.

- The menu is the main menu's own scenario. The pause menu and the menus
  over a level are whatever is behind them.
- A cutscene is a cinematic the scripts bracket (cinematic_start to
  cinematic_stop: cinematic_in_progress) or the letterbox being in
  (cinematic_show_letterbox, which our stereo path takes for the film:
  cinematic_screen.c), while the script also has the camera
  (halo_scripted_camera: the director's scripted camera, or the player's
  look taken away). Both together turn it on at once. It ends when that
  has not held for SCENE_CUTSCENE_HOLD_TICKS: a10 drops its letterbox for
  two ticks in the middle of a cutscene, and a class that flickered would
  resize the screen's targets twice. A cinematic in which the player looks
  around (a10's cryo pod, measured on an Apple TV HD: the script keeps the
  cinematic flag for the whole look test) is play: a profile's cutscene
  frame rate there would be felt in the stick.

Asked once a frame, before the frame's targets are chosen
(d3d8_device.c, screen_mode_choose), so a frame is one class throughout. A
change is logged with what the class brings, and the GPU backend is told the
class, the map and the class's frame rate ([scene.cutscene.display]
frame_rate = "30"), which it holds frames for and names in its frame log
(gpu_frame_context).
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "main/main.h"
#include "../src/gpu.h"
#include "../src/halo_stereo.h"
#include "../src/port_config.h"

#include <stdlib.h>
#include <string.h>

/* port/linux/src/sdl_platform.c (the host's on iOS) */
void platform_log(const char *format, ...);

/* half a second of game time */
#define SCENE_CUTSCENE_HOLD_TICKS 15

/* the map's name without its folders ("levels\\a10\\a10": "a10"), "" before a map */
static void scene_level(char *level, size_t size)
{
	/* (the menu's own map: main_get_map_name still names the level before it) */
	const char *name = !global_scenario ? NULL : global_scenario->type == _scenario_type_main_menu ? "ui" :
		main_get_map_name();
	const char *cursor;

	for (cursor = name; cursor && *cursor; cursor++)
	{
		if (*cursor == '\\' || *cursor == '/')
			name = cursor + 1;
	}
	snprintf(level, size, "%s", name ? name : "");
}

/* the class's frame rate from the device's profile, 0 for none: only a
scene class's value counts here, since the setting's own is the Mac app's
cap (d3d8_device.c, frame_rate_flags) */
static unsigned scene_frame_rate(int scene)
{
	char text[16], *end;
	long rate;

	if (!config_scene_string("display.frame_rate", scene, text, sizeof(text)))
		return 0;
	rate = strtol(text, &end, 10);
	return *text && !*end && rate >= 30 && rate <= 1000 ? (unsigned)rate : 0;
}

int halo_scene_class(void)
{
	static int scene_class = CONFIG_SCENE_GAMEPLAY;
	static long cutscene_last_tick = -1;
	static char told_level[32] = "\1";
	static int told_class = -1;
	static unsigned told_rate;
	char level[32];
	unsigned rate;
	int now = CONFIG_SCENE_GAMEPLAY;

	if (!global_scenario)
	{
		/* (before the first map: the loading screen ahead of the menu) */
		now = CONFIG_SCENE_MENU;
	}
	else if (global_scenario->type == _scenario_type_main_menu)
	{
		now = CONFIG_SCENE_MENU;
		cutscene_last_tick = -1;
	}
	else if (!game_time_initialized())
	{
		/* a frame presented while one level is being replaced by the next (game_precache_new_map presents with the
		old game's time already disposed of): the class stays what it was. game_time_get asserts here, which ended
		the game at a level change */
		return scene_class;
	}
	else
	{
		long tick = game_time_get();

		if (cinematic_globals && (cinematic_globals->in_progress || cinematic_globals->show_letterbox) &&
			halo_scripted_camera())
			cutscene_last_tick = tick;
		/* (a tick before the last cutscene's: a new game or a reverted one) */
		if (cutscene_last_tick >= 0 && tick >= cutscene_last_tick && tick - cutscene_last_tick <= SCENE_CUTSCENE_HOLD_TICKS)
			now = CONFIG_SCENE_CUTSCENE;
		else
			cutscene_last_tick = -1;
	}
	if (now != scene_class)
	{
		char changes[512];

		scene_class = now;
		if (config_scene_describe(now, changes, sizeof(changes)))
			platform_log("scene: %s (%s)", config_scene_name(now), changes);
		else
			platform_log("scene: %s", config_scene_name(now));
	}
	scene_level(level, sizeof(level));
	rate = scene_frame_rate(scene_class);
	if (scene_class != told_class || rate != told_rate || strcmp(level, told_level))
	{
		told_class = scene_class;
		told_rate = rate;
		snprintf(told_level, sizeof(told_level), "%s", level);
		gpu_frame_context((uint32_t)scene_class, rate, level);
	}
	return scene_class;
}
