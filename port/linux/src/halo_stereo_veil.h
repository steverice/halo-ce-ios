/* The eyes' veil: the presenter's blur and dim of the eyes' picture, shared
by what veils it (port/ios/host/host_stereo.m's eye shader): a seat's look
fade (halo_stereo_seat_look_fade, display.seat_look_fade) over the whole
view, and the immersive cutscene's outside (halo_stereo_cutscene.h). The
picture goes toward its blur (the presenter's quarter-size Gaussian of the
eye, HALO_STEREO_CUTSCENE_BLUR_SCALE), then is desaturated toward its luma
and darkened, both by the dim: 0 leaves it as it is, 1 is black.

HALO_STEREO_VEIL_SOURCE is written in the subset of C and the Metal Shading
Language both take (as halo_stereo_cutscene.h's), on one channel of
display-encoded RGB at a time: the eye shader compiles it as text and calls
it per channel (eye_veil), and the probes run it as C. */
#ifndef HALO_STEREO_VEIL_H
#define HALO_STEREO_VEIL_H

/* halo_stereo_veil_luma: the luma of display-encoded RGB (Rec. 601's
weights). halo_stereo_veil: one channel veiled, sharp and blurred the
channel's sharp and blurred values, sharp_luma and blurred_luma the pixel's
lumas of each (the mix's luma is the mix of theirs); blur (0 to 1) how far
toward the blur, dim (0 to 1) how much it's desaturated and darkened */
#define HALO_STEREO_VEIL_SOURCE \
static inline float halo_stereo_veil_luma(float r, float g, float b) \
{ \
	return 0.299f * r + 0.587f * g + 0.114f * b; \
} \
static inline float halo_stereo_veil(float sharp, float blurred, float sharp_luma, float blurred_luma, float blur, \
	float dim) \
{ \
	float color = sharp + (blurred - sharp) * blur; \
	float luma = sharp_luma + (blurred_luma - sharp_luma) * blur; \
	float keep = 1.0f - dim; \
	return (luma + (color - luma) * keep) * keep; \
}

HALO_STEREO_VEIL_SOURCE

#define HALO_STEREO_VEIL_TEXT(...) #__VA_ARGS__
#define HALO_STEREO_VEIL_STRING(...) HALO_STEREO_VEIL_TEXT(__VA_ARGS__)

#endif
