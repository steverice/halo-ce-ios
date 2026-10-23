/* A tvOS device profile's display.output_height for the built-in output (host_main.m): the drawable the game
   draws into is at most that many lines, and the system scales it to the TV with no change of the TV's mode
   (gpu_metal.m sets the layer's drawable to it). Plain C, for port/ios/tests/output_cap_probe.c. */
#pragma once
#include <stdlib.h>

/* The cap in force: a launch's own HALO_OUTPUT_HEIGHT (a whole number above zero, to measure another cap
   without a rebuild) over the profile's text (NULL or "0": none). 0 for no cap. */
static inline int host_output_cap_height(const char *launch, const char *profile)
{
	char *end;
	long value;

	if (launch && *launch)
	{
		value = strtol(launch, &end, 10);
		if (!*end && value > 0 && value <= 4320)
			return (int)value;
	}
	if (!profile || !*profile)
		return 0;
	value = strtol(profile, &end, 10);
	return !*end && value > 0 && value <= 4320 ? (int)value : 0;
}

/* The drawable for an output of width x height pixels under a cap of cap lines: the output's shape at cap
   lines, the width rounded down, as Core Animation truncates a drawable of bounds times a fractional scale (an
   iPad Pro 12.9-inch's 2732x2048 under 1080 is 1440x1080, its 1366-point width at 1.0546875). Returns 1 with *capped_width x *capped_height filled when the
   cap is below the output; 0 (and the output's own size) when there is no cap or the output is no taller. */
static inline int host_output_cap_apply(int width, int height, int cap, int *capped_width, int *capped_height)
{
	*capped_width = width;
	*capped_height = height;
	if (cap <= 0 || width <= 0 || height <= 0 || height <= cap)
		return 0;
	*capped_width = (int)((long long)width * cap / height);
	*capped_height = cap;
	return 1;
}
