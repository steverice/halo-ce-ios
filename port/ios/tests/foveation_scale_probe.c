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
	return 1472.0f + 960.0f * quality;
}

int main(void)
{
	float scale;

	/* no scaling leaves the quality as configured, and so does anything out of range */
	assert(host_foveation_scaled_quality(0.6f, 1.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, 2.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, 0.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, -1.0f) == 0.6f);
	assert(host_foveation_scaled_quality(0.6f, NAN) == 0.6f);
	/* a scale below 1 lowers it, 0.8 to about 0.17 from 0.6 */
	assert(near(host_foveation_scaled_quality(0.6f, 0.8f), 0.192f));
	assert(host_foveation_scaled_quality(0.6f, 0.8f) < host_foveation_scaled_quality(0.6f, 0.9f));
	assert(host_foveation_scaled_quality(0.6f, 0.9f) < 0.6f);
	/* never below 0, and a zero maximum stays 0 */
	assert(host_foveation_scaled_quality(0.6f, 0.25f) == 0.0f);
	assert(host_foveation_scaled_quality(0.0f, 0.8f) == 0.0f);
	/* the view's width follows the scale to within 3% on the measured line */
	for (scale = 0.7f; scale < 1.0f; scale += 0.05f)
	{
		float ratio = measured_width(host_foveation_scaled_quality(0.6f, scale)) / measured_width(0.6f);

		assert(fabsf(ratio - scale) < 0.03f);
	}
	puts("foveation_scale_probe: ok");
	return 0;
}
