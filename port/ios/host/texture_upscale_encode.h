/* Linear BC7/ASTC compression and texture_record.h serialization for the generator. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct texture_upscale_rgba_level
{
    const uint8_t *pixels; /* tightly packed RGBA8, top row first */
    uint32_t width, height;
};
struct texture_upscale_encode_metrics
{
    double rgb_psnr; /* level 0, RGB over source texels with nonzero alpha */
    unsigned binary_alpha_max_error; /* endpoint alpha (0 or 255) only */
    unsigned attempts;
    unsigned preset; /* BC7: 0 16 partitions, 1 64 and uber 1, 2 64 and the highest uber level; ASTC: 0 fastest, 1 fast, 2 medium */
};
enum texture_upscale_encode_result
{
    TEXTURE_UPSCALE_ENCODE_OK,
    TEXTURE_UPSCALE_ENCODE_EXCLUDED,
    TEXTURE_UPSCALE_ENCODE_ERROR
};
/* The exact quality gate, also available to probes: 38 dB inclusive and every
   endpoint alpha within 8 inclusive. Intermediate alpha does not affect RGB masking. */
int texture_upscale_encode_gate(const uint8_t *source, const uint8_t *decoded, size_t texels,
    struct texture_upscale_encode_metrics *metrics);
const char *texture_upscale_encoder_name(uint32_t format);
/* Require consecutive mip sizes and a 1x/2x/4x/8x source-size multiple. Preserve
   the supplied tail: original DXT chains may end at 4x4. On OK,
   *record is malloc'd and *size includes its header; the caller writes it through
   texture_cache_write_entry, then saves its manifest. Exclusion/error returns no
   record. Threads is capped at four to bound codec scratch; zero selects that cap.
   A quality miss retries level 0 before encoding the remaining levels. */
enum texture_upscale_encode_result texture_upscale_encode_record(
    const struct texture_upscale_rgba_level *levels, unsigned count,
    uint32_t source_width, uint32_t source_height, uint64_t hash, uint64_t recipe_key,
    uint32_t format, unsigned threads, void **record, size_t *size,
    struct texture_upscale_encode_metrics *metrics);
#ifdef __cplusplus
}
#endif
