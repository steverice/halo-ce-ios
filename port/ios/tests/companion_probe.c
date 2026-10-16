#include "gpu_companion.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    float scale[2];
    companion_picture_scale(852,480,1920,1080,1280,720,scale);
    assert(lroundf(852*scale[0])==1280 && lroundf(480*scale[1])==720);
    companion_picture_scale(640,480,1440,1080,1280,720,scale);
    assert(lroundf(640*scale[0])==960 && lroundf(480*scale[1])==720);
    companion_picture_scale(768,480,2560,1600,2732,2048,scale);
    assert(lroundf(768*scale[0])==2732 && lroundf(480*scale[1])==1707);
	struct gpu_draw draw = {0};
	struct gpu_constant_store constants = {0};
	struct gpu_uniforms uniforms = {0};
	struct companion_records records = {0};
	float bounds[4] = {1, 2, 640, 480};
	struct gpu_rect rect = companion_rect(bounds, 1.5f, 1.25f);
	assert(rect.x == 2 && rect.y == 3 && rect.width == 958 && rect.height == 597);
	assert(companion_eligible(1, 0, 0, 1, 1));
	assert(!companion_eligible(1, 1, 0, 1, 1));
	assert(!companion_eligible(1, 0, 1, 1, 1));
	assert(!companion_eligible(1, 0, 0, 2, 1));
	assert(!companion_eligible(0, 0, 0, 1, 1));
	constants.c[0][0] = 7;
	uniforms.serial = 9;
	draw.stages[0].texture = 33;
	assert(companion_record_draw(&records, &draw, &constants, &uniforms, bounds));
	constants.c[0][0] = 17;
	uniforms.serial = 19;
	draw.stages[0].texture = 77;
	assert(records.count == 1);
	assert(records.items[0].constants.c[0][0] == 7);
	assert(records.items[0].uniforms.serial == 9);
	assert(records.items[0].draw.stages[0].texture == 33);
	companion_records_reset(&records);
	assert(records.count == 0);
	free(records.items);
	puts("companion: geometry, selection and independent snapshots pass");
	return 0;
}
