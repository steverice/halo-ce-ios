/* the per-level gate: the switch and a skip take effect at the next map load, never mid-level; the debug toggle */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "texture_upscale_state.h"

int main(void)
{
	struct texture_upscale_state s = { 0 };

	assert(!texture_upscale_state_enabled(&s));          /* nothing before the first map */
	texture_upscale_state_map_loaded(&s, 1);
	assert(texture_upscale_state_enabled(&s));
	texture_upscale_state_request_original(&s);         /* asked mid-level: this level is unchanged */
	assert(texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 1);             /* the next load plays original ... */
	assert(!texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 1);             /* ... and only that one */
	assert(texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 0);             /* the switch off */
	assert(!texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 1);
	assert(texture_upscale_state_enabled(&s));
	/* debug.upscaled_textures_toggle: every texture of the level at once, and each level starts upscaled */
	assert(!texture_upscale_state_show_original(&s));
	texture_upscale_state_replaced(&s);                 /* one arrives ... */
	assert(texture_upscale_state_toggle(&s));
	assert(texture_upscale_state_show_original(&s));
	texture_upscale_state_replaced(&s);                 /* ... and one arrives while the originals show: they stay */
	assert(texture_upscale_state_show_original(&s) && s.replaced == 2);
	assert(texture_upscale_state_enabled(&s));          /* the level's own gate is untouched */
	assert(texture_upscale_state_toggle(&s));
	assert(!texture_upscale_state_show_original(&s));
	assert(texture_upscale_state_toggle(&s));
	texture_upscale_state_map_loaded(&s, 1);             /* a new level starts upscaled */
	assert(!texture_upscale_state_show_original(&s) && s.replaced == 0);
	texture_upscale_state_map_loaded(&s, 0);             /* a level without upscaled textures: nothing to switch */
	assert(!texture_upscale_state_toggle(&s));
	assert(!texture_upscale_state_show_original(&s));
	texture_upscale_state_map_loaded(&s, 1);
	texture_upscale_state_compare(&s, 1);
	assert(s.compare && !s.higher && !s.show_original);
	assert(texture_upscale_state_toggle(&s) && s.higher && !s.show_original);
	s.fallback = 1;                                      /* a switch starts the fallback note again */
	assert(texture_upscale_state_toggle(&s) && !s.higher && !s.show_original && !s.fallback);
	texture_upscale_state_map_loaded(&s, 1);
	texture_upscale_state_compare(&s, 1);
	assert(!s.higher);
	texture_upscale_state_compare(&s, 0);
	assert(texture_upscale_state_toggle(&s) && s.show_original); /* old cycle without an alternate */
	texture_upscale_state_map_loaded(&s, 0);
	texture_upscale_state_compare(&s, 1);
	assert(!s.compare && !texture_upscale_state_toggle(&s));
	puts("texture_upscale_state_probe: ok");
	return 0;
}
