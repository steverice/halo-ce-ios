#include "host_output_cap.h"
#include <assert.h>
#include <stdio.h>

static void capped(int width, int height, int cap, int expected_width, int expected_height)
{
	int w, h;
	assert(host_output_cap_apply(width, height, cap, &w, &h) == 1);
	assert(w == expected_width && h == expected_height);
}

static void uncapped(int width, int height, int cap)
{
	int w, h;
	assert(host_output_cap_apply(width, height, cap, &w, &h) == 0);
	assert(w == width && h == height);
}

int main(void)
{
	/* a 4K TV under the Apple TV 4K (1st generation)'s 720, and under 1080 */
	capped(3840, 2160, 720, 1280, 720);
	capped(3840, 2160, 1080, 1920, 1080);
	/* a 1080 TV under a 720 debug cap; a 16:10 panel keeps its shape */
	capped(1920, 1080, 720, 1280, 720);
	capped(2560, 1600, 900, 1440, 900);
	/* an iPad Pro 12.9-inch: the width is rounded down, as Core Animation does with its points times scale */
	capped(2732, 2048, 1080, 1440, 1080);
	capped(2732, 2048, 720, 960, 720);
	/* a cap at or above the output, or none, leaves it alone */
	uncapped(1920, 1080, 1080);
	uncapped(1280, 720, 1080);
	uncapped(3840, 2160, 0);
	uncapped(0, 0, 720);

	/* the launch's own value over the profile's; malformed or zero values are no cap */
	assert(host_output_cap_height(NULL, "720") == 720);
	assert(host_output_cap_height("", "720") == 720);
	assert(host_output_cap_height("540", "720") == 540);
	assert(host_output_cap_height("abc", "720") == 720);
	assert(host_output_cap_height("0", "720") == 720);
	assert(host_output_cap_height(NULL, "0") == 0);
	assert(host_output_cap_height(NULL, NULL) == 0);
	assert(host_output_cap_height(NULL, "99999") == 0);
	assert(host_output_cap_height("720x", NULL) == 0);
	puts("PASS: built-in output caps keep the output's shape and only ever lower it");
	return 0;
}
