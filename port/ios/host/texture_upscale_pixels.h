/* Pixel operations defined by the Mac pipeline: pipeline.py, consistency.py, bump.py and damage.py. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum texture_upscale_alpha { TEXTURE_UPSCALE_ALPHA_NONE, TEXTURE_UPSCALE_ALPHA_BINARY, TEXTURE_UPSCALE_ALPHA_GRADED };

/* Interleaved floats, rows first. Buffers must not overlap. Return 0 on success, -1 on invalid input/allocation.
   down2 halves axes longer than 1 (floor, at least 1); resize multiplies both axes, including a singleton axis. */
int texture_upscale_down2(const float *src, uint32_t width, uint32_t height, uint32_t channels, int wrap, float *dst);
/* Scalar output from one interleaved input channel; avoids extracting a full-resolution channel. */
int texture_upscale_down2_channel(const float *src, uint32_t width, uint32_t height, uint32_t channels,
    uint32_t channel, int wrap, float *dst);
int texture_upscale_resize(const float *src, uint32_t width, uint32_t height, uint32_t channels, uint32_t scale,
    int wrap, float *dst);
/* Includes real neighbors beyond a tile, wrapping or clamping against the full source texture. */
int texture_upscale_pad_region(const float *src, uint32_t width, uint32_t height, uint32_t channels,
    int32_t x, int32_t y, uint32_t out_width, uint32_t out_height, int wrap, float *dst);
/* Push-pull fills only RGB whose alpha is zero; alpha and visible RGB stay intact. Power-of-two axes required. */
int texture_upscale_dilate(float *rgba, uint32_t width, uint32_t height);
void texture_upscale_finish(const float *rgba, uint32_t width, uint32_t height, enum texture_upscale_alpha alpha,
    uint8_t *dst);
/* top has 3 or 4 channels; reference always has 4, already dilated as refine_inputs specifies. When alpha_visible
   is true, only reference alpha > 0 contributes. Color-like treatments only; callers exclude bump/multipurpose. */
int texture_upscale_damage(const float *top, uint32_t top_channels, const float *reference, uint32_t width,
    uint32_t height, uint32_t scale, int wrap, int alpha_visible, double *share, double *structure_loss);
/* Decode the biased RGB, normalize, and integrate with the exact periodic forward-difference Fourier response.
   Bluestein convolution handles non-power-of-two axes without changing their periodic boundary. */
int texture_upscale_integrate_normals(const uint8_t *rgba, uint32_t width, uint32_t height, float *height_out,
    float *low, float *span);
void texture_upscale_derive_normals(const float *height, uint32_t width, uint32_t height_count, float factor,
    float *normals);

#ifdef __cplusplus
}
#endif
