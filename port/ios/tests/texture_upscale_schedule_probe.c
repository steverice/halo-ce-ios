/* texture_upscale_schedule.h: the profile gate (rule 1) and when the work may run (rules 4 and 6) */
#include <assert.h>
#include <stdio.h>
#include "texture_upscale_schedule.h"

static void gate(void)
{
	assert(texture_upscale_mode_parse(NULL) == TEXTURE_UPSCALE_MODE_OFF);
	assert(texture_upscale_mode_parse("off") == TEXTURE_UPSCALE_MODE_OFF);
	assert(texture_upscale_mode_parse("nonsense") == TEXTURE_UPSCALE_MODE_OFF);
	assert(texture_upscale_mode_parse("auto") == TEXTURE_UPSCALE_MODE_AUTO);
	assert(texture_upscale_mode_parse("true") == TEXTURE_UPSCALE_MODE_AUTO);
	assert(texture_upscale_mode_parse("force") == TEXTURE_UPSCALE_MODE_FORCE);
	/* the profile is asked first: the player's switch never turns it on where the profile has it off */
	assert(texture_upscale_gate(TEXTURE_UPSCALE_MODE_AUTO, 1, 1, 1));
	assert(!texture_upscale_gate(TEXTURE_UPSCALE_MODE_AUTO, 0, 1, 1));
	assert(!texture_upscale_gate(TEXTURE_UPSCALE_MODE_AUTO, 1, 0, 1));
	assert(!texture_upscale_gate(TEXTURE_UPSCALE_MODE_OFF, 1, 1, 1));
	assert(texture_upscale_gate(TEXTURE_UPSCALE_MODE_FORCE, 0, 0, 1));
	/* no Neural Engine, nothing, even forced (rule 8) */
	assert(!texture_upscale_gate(TEXTURE_UPSCALE_MODE_FORCE, 1, 1, 0));
	assert(!texture_upscale_gate(TEXTURE_UPSCALE_MODE_AUTO, 1, 1, 0));
}

static void activity(void)
{
	struct texture_upscale_activity a = { 0 };

	/* menus: free to run */
	assert(texture_upscale_activity_may_run(&a, 0) && texture_upscale_activity_wait(&a, 0) == 0);
	/* a level's game: never, until something changes */
	texture_upscale_activity_game_started(&a, 1);
	assert(!texture_upscale_activity_may_run(&a, 10) && texture_upscale_activity_wait(&a, 10) < 0);
	/* paused: only after TEXTURE_UPSCALE_PAUSE_SECONDS */
	texture_upscale_activity_paused(&a, 1, 20);
	assert(!texture_upscale_activity_may_run(&a, 21));
	assert(texture_upscale_activity_wait(&a, 21) > 0.9 && texture_upscale_activity_wait(&a, 21) < 1.1);
	assert(texture_upscale_activity_may_run(&a, 22));
	/* a second pause signal does not restart the clock */
	texture_upscale_activity_paused(&a, 1, 21.5);
	assert(texture_upscale_activity_may_run(&a, 22));
	/* resumed: stops at once */
	texture_upscale_activity_paused(&a, 0, 23);
	assert(!texture_upscale_activity_may_run(&a, 100));
	/* the next map's load, and the menus' map, run again */
	texture_upscale_activity_map_will_load(&a);
	assert(texture_upscale_activity_may_run(&a, 100));
	texture_upscale_activity_game_started(&a, 1);
	texture_upscale_activity_game_started(&a, 0);
	assert(texture_upscale_activity_may_run(&a, 100));
}

static void order_and_skip(void)
{
	/* the main menu's map is first, then the campaign in order, then multiplayer maps, then community maps */
	assert(texture_upscale_map_rank("ui", 0) < texture_upscale_map_rank("a10", 0));
	assert(texture_upscale_map_rank("a10", 0) < texture_upscale_map_rank("d40", 0));
	assert(texture_upscale_map_rank("d40", 0) < texture_upscale_map_rank("bloodgulch", 0));
	assert(texture_upscale_map_rank("bloodgulch", 0) < texture_upscale_map_rank("anything", 1));
	assert(texture_upscale_map_rank("ui", 0) == 0);
	/* Skip waits for the menu's textures: not before the catalog lists them, not while any is missing */
	assert(!texture_upscale_import_skippable(0, 0, 0));
	assert(!texture_upscale_import_skippable(1, 0, 40));
	assert(!texture_upscale_import_skippable(1, 39, 40));
	assert(texture_upscale_import_skippable(1, 40, 40));
	/* nothing to upscale in the menu map (or no menu map): nothing to wait for */
	assert(texture_upscale_import_skippable(1, 0, 0));
}

int main(void)
{
	gate();
	activity();
	order_and_skip();
	puts("texture_upscale_schedule_probe: ok");
	return 0;
}
