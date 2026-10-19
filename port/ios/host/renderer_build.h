#ifndef HALO_RENDERER_BUILD_H
#define HALO_RENDERER_BUILD_H

#include <string.h>

#ifndef HALO_METAL_ONLY
#define HALO_METAL_ONLY 0
#endif

static inline int host_renderer_selects_metal(const char *renderer)
{
	return HALO_METAL_ONLY || (renderer && !strcmp(renderer, "metal"));
}

static inline const char *host_renderer_guest_value(int visionos, int tvos, const char *launch_renderer)
{
	if (HALO_METAL_ONLY || visionos)
		return "metal";
	if (tvos && launch_renderer && (!strcmp(launch_renderer, "gl") || !strcmp(launch_renderer, "metal")))
		return launch_renderer;
	return NULL;
}

static inline const char *host_render_replay_rejection(int metal_capture, int visionos)
{
	if (metal_capture)
		return NULL;
	if (visionos)
		return "an OpenGL ES capture cannot be replayed on visionOS, which has no OpenGL ES";
#if HALO_METAL_ONLY
	return "an OpenGL ES capture cannot be replayed in a Metal-only build";
#else
	return NULL;
#endif
}

#endif
