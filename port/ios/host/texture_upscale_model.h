/* S4g's per-channel SPAN inference. The caller serializes access and owns the image buffers. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct texture_upscale_model;

struct texture_upscale_model_stats {
    uint64_t predictions;
    double seconds;
    uint64_t footprint_before;
    uint64_t footprint_after;
    uint64_t peak_footprint;
    uint32_t input_width, input_height;
};

typedef void (*texture_upscale_model_observer)(const struct texture_upscale_model_stats *stats, void *context);

/* Includes Core ML's current device availability and the compiled resource in the main bundle. */
int texture_upscale_model_available(void);
/* At most one live service. Reports unavailable rather than falling back to CPU-only or GPU inference. */
struct texture_upscale_model *texture_upscale_model_create(char *error, size_t error_size);
void texture_upscale_model_destroy(struct texture_upscale_model *model);
void texture_upscale_model_set_observer(struct texture_upscale_model *model,
    texture_upscale_model_observer observer, void *context);
void texture_upscale_model_get_stats(struct texture_upscale_model *model, struct texture_upscale_model_stats *stats);

/* Input and output are packed interleaved floats. channels is 1 (replicated in, averaged out) or 3
   (the package runs SPAN independently on each channel). Output is width*4 by height*4 with the same
   channel count. Values remain unclamped, as in the Mac pipeline. 0 succeeds; -1 reports an error.
   Tiles use one 288x288 input shape: 256 texture texels plus 16 real wrap/clamp neighbors per side. */
int texture_upscale_model_predict4x(struct texture_upscale_model *model, const float *input,
    uint32_t width, uint32_t height, uint32_t channels, int wrap, float *output,
    char *error, size_t error_size);
/* Scatter each predicted pixel into an interleaved destination, writing only channels values.
   output can point to one channel of an RGBA image; output_pixel_stride is in floats, at least channels. */
int texture_upscale_model_predict4x_strided(struct texture_upscale_model *model, const float *input,
    uint32_t width, uint32_t height, uint32_t channels, int wrap, float *output,
    uint32_t output_pixel_stride, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
