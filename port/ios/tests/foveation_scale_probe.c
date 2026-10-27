/* host_foveation_scale.h: display.render_scale's effect on the foveated
stereo views' render quality */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "host_foveation_scale.h"

static int near(float a, float b)
{
	return fabsf(a - b) < 0.005f;
}

/* the headset's measured physical widths (the 2026-10-07 foveation sweep) by quality, as a line */
static float measured_width(float quality)
{
	return 1450.0f + 983.0f * quality;
}

static float measured_height(float quality)
{
	return 1348.0f + 1006.0f * quality;
}

int main(void)
{
	float scale, maximum;

	/* no scaling leaves the quality as configured, and so does anything out of range */
	assert(host_foveation_scaled_quality(0.6f, 1.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, 2.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, 0.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, -1.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, NAN) == 0.6f);
	/* a scale below 1 lowers it */
	assert(near(host_foveation_scaled_quality(0.6f, 0.8f), 0.198f));
	assert(host_foveation_scaled_quality(0.6f, 0.8f) < host_foveation_scaled_quality(0.6f, 0.9f));
	assert(host_foveation_scaled_quality(0.6f, 0.9f) < 0.6f);
	/* never below 0, and a zero maximum stays 0 */
	assert(host_foveation_scaled_quality(0.6f, 0.25f) == 0.0f);
	assert(host_foveation_scaled_quality(0.0f, 0.8f) == 0.0f);
	/* the view's width follows the scale to within 3% on the measured line */
	for (maximum = 0.6f; maximum <= 1.0f; maximum += 0.4f)
		for (scale = 0.75f; scale < 1.0f; scale += 0.05f)
		{
			float quality = host_foveation_scaled_quality(maximum, scale);
			float width = measured_width(quality) / measured_width(maximum);
			float height = measured_height(quality) / measured_height(maximum);

			if (quality > 0.0f)
				assert(fabsf(width - scale) < 0.03f && fabsf(height - scale) < 0.03f);
		}
	/* below the game's 0.25 floor is the floor */
	assert(host_foveation_scaled_quality(1.0f, 0.1f) == host_foveation_scaled_quality(1.0f, 0.25f));
	puts("foveation_scale_probe: ok");
	return 0;
}
