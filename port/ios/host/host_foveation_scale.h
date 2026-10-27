/* display.render_scale on Apple Vision Pro's foveated head-tracked stereo.
The Compositor sizes the views itself: each view's rate map has a fixed
logical (screen) size and a physical size that follows the layer's render
quality, so the picture's pixel count cannot be scaled the way a flat
screen's is. The quality is the lever. Measured on the headset
(baselines/avp-benchmarks/foveation-sweep-20261007-0157), the physical size
of a view grows about linearly with the quality: 1856x1760 at 0.4, 2048x1952
at 0.6, 2432x2368 at 1.0, a width and height that each change by about 0.49
of the size at the configured quality per 1.0 of quality. A render scale s
below 1 therefore lowers the quality by (1 - s) / 0.49, from the configured
maximum (display.render_quality), and not below 0. Pure C so
port/ios/tests/foveation_scale_probe.c runs it. */
#ifndef HOST_FOVEATION_SCALE_H
#define HOST_FOVEATION_SCALE_H

#define HOST_FOVEATION_SIZE_PER_QUALITY 0.49f

/* the runtime render quality for display.render_scale scale (0..1; anything
else, or NaN, is 1: no scaling) when display.render_quality is maximum */
static inline float host_foveation_scaled_quality(float maximum, float scale)
{
	float quality;

	if (!(maximum > 0.0f))
		return 0.0f;
	if (!(scale > 0.0f) || scale >= 1.0f)
		return maximum;
	quality = maximum - (1.0f - scale) / HOST_FOVEATION_SIZE_PER_QUALITY;
	return quality < 0.0f ? 0.0f : quality;
}

#endif
