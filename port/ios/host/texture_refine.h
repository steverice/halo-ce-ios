/* bp and bpf (step-tools/upscale_lib/consistency.py; bump.refine for bump maps) as Metal compute passes; the NumPy
   code is the specification and the probe holds this to it at over 40 dB */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct texture_refine_context;

struct texture_refine_params { float low, high, sigma; uint32_t iterations; };   /* low, high in 0..255 levels */

struct texture_refine_context *texture_refine_create(void *device /* id<MTLDevice>; NULL: the default */,
	char *error, size_t error_size);
void texture_refine_destroy(struct texture_refine_context *context);
/* top: scale x (width, height), channels 1-4, floats 0..1, replaced by bp (fallback 0) or bpf (fallback 1);
   reference: the original at (width, height), the same channels; mask: (width, height) or NULL. 0 on success */
int texture_refine_color(struct texture_refine_context *handle, float *top, const float *reference,
	uint32_t width, uint32_t height, uint32_t channels, uint32_t scale, int wrap,
	const struct texture_refine_params *params, int fallback, float *mask);
/* CPU version with the same result: modifies top in place, streams upsampling rows, and retains only downsampled
   intermediates plus original-size work buffers. No Metal context or full-resolution copies are needed. */
int texture_refine_color_bounded(float *top, const float *reference, uint32_t width, uint32_t height,
	uint32_t channels, uint32_t scale, int wrap, const struct texture_refine_params *params, int fallback, float *mask);
/* top_height: scale x (width, height) in original-texel units; original_height: (width, height); normals:
   scale x (width, height) x 3 floats out; mask as above. Always wraps (bump maps tile). */
int texture_refine_bump(struct texture_refine_context *handle, const float *top_height, const float *original_height,
	uint32_t width, uint32_t height, uint32_t scale, const struct texture_refine_params *params, int fallback,
	float *normals, float *mask);
/* CPU bump version: projects top_height in place without clipping and streams the fallback target's height rows.
   normals is a separate three-channel output; no other full-resolution image is allocated. */
int texture_refine_bump_bounded(float *top_height, const float *original_height, uint32_t width,
	uint32_t height, uint32_t scale, const struct texture_refine_params *params, int fallback, float *normals, float *mask);
