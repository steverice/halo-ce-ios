/* display.render_scale on Apple Vision Pro's foveated head-tracked stereo.
The Compositor sizes the views itself: each view's rate map has a fixed
logical (screen) size and a physical size that follows the layer's render
quality, so the picture's pixel count cannot be scaled the way a flat
screen's is. The quality is the lever. Least squares over the headset's
2026-10-07 foveation sweep (baselines/avp-benchmarks/foveation-sweep-20261007-0157,
qualities 0.4 to 1.0) gives a physical width of about 1450 + 983 q and a
height of about 1348 + 1006 q. A render scale s below 1 picks the quality at
which each axis is s of its size at the configured maximum, the two
averaged, and not below 0. The sweep only ever ran with the runtime quality
at the maximum, so the headset check (the log's "physical now" size) is what
confirms a lower runtime quality alone shrinks the views. Pure C so
port/ios/tests/foveation_scale_probe.c runs it. */
#ifndef HOST_FOVEATION_SCALE_H
#define HOST_FOVEATION_SCALE_H

#define HOST_FOVEATION_WIDTH_INTERCEPT 1450.0f
#define HOST_FOVEATION_WIDTH_SLOPE 983.0f
#define HOST_FOVEATION_HEIGHT_INTERCEPT 1348.0f
#define HOST_FOVEATION_HEIGHT_SLOPE 1006.0f

/* the runtime render quality for display.render_scale scale (0.25..1; the
game's own range: a smaller one is 0.25, anything else, or NaN, is 1: no
scaling) when display.render_quality is maximum */
static inline float host_foveation_scaled_quality(float maximum, float scale)
{
	float drop;

	if (!(maximum > 0.0f))
		return 0.0f;
	if (!(scale > 0.0f) || scale >= 1.0f)
		return maximum;
	if (scale < 0.25f)
		scale = 0.25f;
	drop = (1.0f - scale) * 0.5f *
		((HOST_FOVEATION_WIDTH_INTERCEPT + HOST_FOVEATION_WIDTH_SLOPE * maximum) / HOST_FOVEATION_WIDTH_SLOPE +
		 (HOST_FOVEATION_HEIGHT_INTERCEPT + HOST_FOVEATION_HEIGHT_SLOPE * maximum) / HOST_FOVEATION_HEIGHT_SLOPE);
	return maximum - drop < 0.0f ? 0.0f : maximum - drop;
}

#endif
