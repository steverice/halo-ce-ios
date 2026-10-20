#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../host/host_stereo_hud.h"

int main(void)
{
	struct host_stereo_hud_aim aim;
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	const float reticle[3] = { 0.2f, 0.1f, -1.0f };
	const float tangents[2] = { 0.6f, 0.4f };
	float extents[HALO_HUD_GROUP_COUNT][4] = {{0}};
	int count;

	memset(&aim, 0, sizeof(aim));
	aim.world = 1;
	aim.distance = 1.75f;
	aim.laser = 1;
	aim.laser_from[0] = 0.1f;
	aim.laser_from[2] = -0.2f;
	aim.laser_opacity = 0.5f;
	count = host_stereo_hud_layout_aimed(640.0f, 0, reticle, &aim, tangents,
		(const float (*)[4])extents, NULL, NULL, quads);
	if (count < 2 || !quads[0].laser || quads[1].layer != HOST_STEREO_HUD_LAYER_RETICLE ||
		quads[1].center[2] > -1.5f || fabsf(quads[1].center[0] / -quads[1].center[2] - 0.2f) > 0.01f)
	{
		printf("tracked_hud_probe: FAILED\n");
		return 1;
	}
	printf("tracked_hud_probe: PASS\n");
	return 0;
}
