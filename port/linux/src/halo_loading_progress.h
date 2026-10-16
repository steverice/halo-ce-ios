/* The loading picture keeps its existing animation; only its input changes.
Compiler progress describes a sealed batch, not elapsed time or a peak of
whatever work happened to remain when the game thread first polled it. */
#ifndef HALO_LOADING_PROGRESS_H
#define HALO_LOADING_PROGRESS_H

#include "gpu.h"
#include <math.h>

static inline float halo_loading_compile_fraction(const struct gpu_compile_progress *progress)
{
	if (!progress->sealed)
		return 0.0f;
	if (!progress->total)
		return 1.0f;
	return (float)(progress->completed - progress->failed) / (float)progress->total;
}

static inline float halo_loading_progress_advance(float shown, float target, unsigned long milliseconds, float ease)
{
	float advanced;
	if (target <= shown)
		return shown;
	/* A blocked game thread must not cause a single-frame jump. */
	if (milliseconds > 100)
		milliseconds = 100;
	advanced = shown + (target - shown) * (1.0f - (float)exp(-(float)milliseconds / (1000.0f * ease)));
	return advanced < target ? advanced : target;
}

#endif
