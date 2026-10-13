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
  cinematic_screen.c). Either turns it on at once. It ends when both have
  been off for SCENE_CUTSCENE_HOLD_TICKS: a10 drops its letterbox for two
  ticks in the middle of a cutscene, and a class that flickered would resize
  the screen's targets twice. A camera the script holds without either (the
  look-around in a10's cryo pod) is play: it costs what play costs.

Asked once a frame, before the frame's targets are chosen
(d3d8_device.c, screen_mode_choose), so a frame is one class throughout. A
change is logged with what the class brings.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "../src/port_config.h"

/* port/linux/src/sdl_platform.c (the host's on iOS) */
void platform_log(const char *format, ...);

/* half a second of game time */
#define SCENE_CUTSCENE_HOLD_TICKS 15

int halo_scene_class(void)
{
	static int scene_class = CONFIG_SCENE_GAMEPLAY;
	static long cutscene_last_tick = -1;
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
	else
	{
		long tick = game_time_get();

		if (cinematic_globals && (cinematic_globals->in_progress || cinematic_globals->show_letterbox))
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
	return scene_class;
}
