/* texture_upscale_priority.h: the upscaler works hard only while the player is waiting for it */
#include <assert.h>
#include <stdio.h>
#include "texture_upscale_priority.h"

int main(void)
{
	assert(texture_upscale_priority_qos(0) == QOS_CLASS_UTILITY);
	assert(texture_upscale_priority_qos(-1) == QOS_CLASS_UTILITY);
	assert(texture_upscale_priority_qos(1) == QOS_CLASS_USER_INITIATED);
	assert(texture_upscale_priority_qos(2) == QOS_CLASS_USER_INITIATED);
	/* waiting: every core up to 8, half when the device is hot */
	assert(texture_upscale_priority_threads(6, 1, 0) == 6);
	assert(texture_upscale_priority_threads(12, 1, 0) == 8);
	assert(texture_upscale_priority_threads(6, 1, 1) == 3);
	/* background: half the cores, never fewer than one */
	assert(texture_upscale_priority_threads(6, 0, 0) == 3);
	assert(texture_upscale_priority_threads(12, 0, 0) == 4);
	assert(texture_upscale_priority_threads(6, 0, 1) == 3);
	assert(texture_upscale_priority_threads(2, 0, 0) == 1);
	assert(texture_upscale_priority_threads(1, 0, 0) == 1);
	assert(texture_upscale_priority_threads(0, 1, 0) == 1);
	puts("texture_upscale_priority_probe: ok");
	return 0;
}
