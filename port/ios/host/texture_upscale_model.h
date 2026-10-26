/* S4g's per-channel SPAN inference. The caller serializes access and owns the image buffers. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct texture_upscale_model;

struct texture_upscale_model_stats {
    uint64_t predictions;    /* Core ML calls */
    uint64_t tiles;          /* tiles predicted: three single-channel tiles share one call */
    double seconds;          /* inside the Core ML calls */
    double pack_seconds;     /* packing inputs, on the calling thread */
    double consume_seconds;  /* the consumer's work on outputs, beside the next call */
    uint64_t footprint_before;
    uint64_t footprint_after;
    uint64_t peak_footprint;    /* the process's ledger peak, including the app's own launch */
    uint64_t sampled_peak, texture_peak;    /* generator samples of phys_footprint: all textures, the last one */
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

/* The scheduler's stop for gameplay: while *flag reads nonzero, predict_tiles ends with an error before its next
   Core ML call, so a stop waits for one call at most. NULL (the default) never stops. Process-wide. */
void texture_upscale_model_set_cancel(const volatile int *flag);
int texture_upscale_model_cancelled(void);

/* One predicted tile channel. The core is input texels [x, x + width) by [y, y + height); the model's 4x output
   around it is readable from 64 output pixels before the core's first 4x row and column to 64 after its last
   (texture_upscale_tile_at), from real neighbors wrapped or clamped as the input requested. */
struct texture_upscale_tile {
    uint32_t x, y, width, height;
    const void *data;    /* the core's first 4x pixel */
    ptrdiff_t row_stride, column_stride;    /* in elements */
    int half;    /* elements are _Float16, else float */
};
#define TEXTURE_UPSCALE_TILE_CONTEXT 64
static inline float texture_upscale_tile_at(const struct texture_upscale_tile *tile, int32_t x, int32_t y)
{
    ptrdiff_t i = (ptrdiff_t)y * tile->row_stride + (ptrdiff_t)x * tile->column_stride;
    return tile->half ? (float)((const _Float16 *)tile->data)[i] : ((const float *)tile->data)[i];
}
/* Called once for each tile and channel, on one other thread at a time, while the next call runs; the caller's
   predict_tiles returns after the last. Nonzero stops the prediction with an error. */
typedef int (*texture_upscale_tile_consumer)(void *context, const struct texture_upscale_tile *tile, uint32_t channel);
/* Input is packed interleaved floats with channels 1 or 3. Each call takes as many tile channels as the package has
   planes: a three-plane package runs S4g on each plane as a batch of three, a one-plane package on one. */
int texture_upscale_model_predict_tiles(struct texture_upscale_model *model, const float *input,
    uint32_t width, uint32_t height, uint32_t channels, int wrap,
    texture_upscale_tile_consumer consumer, void *context, char *error, size_t error_size);

/* Input and output are packed interleaved floats. channels is 1 or 3 (the package runs SPAN independently
   on each channel). Output is width*4 by height*4 with the same
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
