#include "../host/renderer_build.h"

#include <assert.h>
#include <string.h>

int main(void)
{
#if HALO_METAL_ONLY
	assert(host_renderer_selects_metal("gl"));
	assert(!strcmp(host_renderer_guest_value(0, 0, "gl"), "metal"));
	assert(host_render_replay_rejection(0, 0) != NULL);
#else
	assert(!host_renderer_selects_metal("gl"));
	assert(host_renderer_selects_metal("metal"));
	assert(!strcmp(host_renderer_guest_value(0, 1, "gl"), "gl"));
	assert(!strcmp(host_renderer_guest_value(0, 1, "metal"), "metal"));
	assert(!strcmp(host_renderer_guest_value(1, 0, NULL), "metal"));
	assert(host_renderer_guest_value(0, 0, NULL) == NULL);
	assert(host_render_replay_rejection(0, 0) == NULL);
#endif
	assert(host_render_replay_rejection(1, 0) == NULL);
	assert(host_render_replay_rejection(0, 1) != NULL);
	return 0;
}
