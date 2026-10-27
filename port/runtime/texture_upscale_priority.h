/* How hard the on-device upscaler works (port/ios/host/host_texture_upscale.m), as plain functions so a host probe can
check them without the app.

The import phase and a level's wait are the player waiting, so they get user-initiated priority and every core. Anything
else is background work behind a menu the player is using: utility priority and half the cores, so the touch controls,
the render server and the game's own threads keep a share of the processor. The priority is read again for every
texture. A pump is a block on one serial queue that works through the whole backlog, so a priority chosen when the
pump was queued would last as long as the backlog does, long after the wait that asked for it was over. */
#pragma once
#include <sys/qos.h>

/* the quality of service for the worker and the threads it starts; urgent is the number of waits on screen */
static inline qos_class_t texture_upscale_priority_qos(int urgent)
{
	return urgent > 0 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY;
}

/* how many threads one texture's encode and refinement may use */
static inline unsigned texture_upscale_priority_threads(unsigned cores, int urgent, int thermal_serious)
{
	unsigned count = cores > 8 ? 8 : cores < 1 ? 1 : cores;

	if (urgent <= 0 || thermal_serious)
		count /= 2;
	return count < 1 ? 1 : count;
}
