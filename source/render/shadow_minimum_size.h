/*
SHADOW_MINIMUM_SIZE.H

port: display.shadow_minimum_size, the smallest an object may be on screen
and still cast its shadow (render_objects.c). Sizes are the object's bounding
sphere diameter in pixels of the 480-line picture, so a value means the same
at any render resolution. The game's own floor is 30 pixels, with the shadow
fading in over the next 15; a larger floor moves the fade up with it, over
the next half of the floor, so a shadow is invisible where it starts or stops
being drawn. A floor at or below the game's keeps the game's arithmetic
exactly. Standalone C89 so tools/test_shadow_minimum_size.py can build it.
*/

#ifndef SHADOW_MINIMUM_SIZE_H
#define SHADOW_MINIMUM_SIZE_H

#define SHADOW_MINIMUM_SIZE_GAME_PIXELS 30.f

/* the floor for one object: the game's where the setting is at or below it,
or the object rides with a local player (exempt) */
static float shadow_minimum_size_pixels(
	long configured,
	int exempt)
{
	if (exempt || (float)configured <= SHADOW_MINIMUM_SIZE_GAME_PIXELS)
		return SHADOW_MINIMUM_SIZE_GAME_PIXELS;
	return (float)configured;
}

/* whether a floor above the game's leaves the object without its shadow:
decided before the shadow's lighting is looked up */
static int shadow_minimum_size_culled(
	float level_of_detail_pixels,
	float minimum_pixels)
{
	return minimum_pixels > SHADOW_MINIMUM_SIZE_GAME_PIXELS &&
		!(level_of_detail_pixels > minimum_pixels);
}

/* how far into its fade the shadow is, before the caller pins it to 0..1 */
static float shadow_minimum_size_fraction(
	float level_of_detail_pixels,
	float minimum_pixels)
{
	if (minimum_pixels > SHADOW_MINIMUM_SIZE_GAME_PIXELS)
		return (level_of_detail_pixels - minimum_pixels) * (2.f / minimum_pixels);
	return (level_of_detail_pixels - SHADOW_MINIMUM_SIZE_GAME_PIXELS) * 0.06666667f;
}

/* whether root_index (an object's outermost parent) is one of the local
players' */
static int shadow_minimum_size_exempt(
	long root_index,
	long const *exempt_roots,
	int exempt_root_count)
{
	int index;

	for (index = 0; index < exempt_root_count; index++)
	{
		if (exempt_roots[index] == root_index)
			return 1;
	}
	return 0;
}

#endif
