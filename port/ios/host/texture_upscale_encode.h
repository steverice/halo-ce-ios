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
    unsigned binary_alpha_max_error; /* binary alpha: endpoint alpha (0 or 255) only */
    double alpha_psnr; /* graded alpha: over every texel; infinite otherwise */
    double mean_angle; /* normal maps: mean degrees between source and decoded normals over nonzero alpha; -1 otherwise */
    int raw; /* no block encoding passed: the record is BGRA8 (TEXTURE_RECORD_BGRA8) */
    unsigned attempts;
    unsigned preset; /* BC7: 0 16 partitions, 1 64 and uber 1, 2 64 and the highest uber level; ASTC: 0 fastest, 1 fast, 2 medium */
};
/* which alpha test the gate applies: none, binary (endpoint texels within 8 levels) or graded (38 dB over every texel) */
enum { TEXTURE_UPSCALE_ENCODE_ALPHA_NONE, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, TEXTURE_UPSCALE_ENCODE_ALPHA_GRADED };
/* Or'd into alpha_mode for a bump map's normals: their RGB gate is the mean angle, at most this many degrees, instead
   of 38 dB. A small height difference turns into a large RGB one where normals are steep, and the device parity check
   holds bump maps to the same 2 degrees against the Mac set. */
enum { TEXTURE_UPSCALE_ENCODE_NORMALS = 4 };
#define TEXTURE_UPSCALE_ENCODE_NORMAL_ANGLE 2.0
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
/* the same gate with the alpha test chosen; texture_upscale_encode_gate is the binary one */
int texture_upscale_encode_gate_alpha(const uint8_t *source, const uint8_t *decoded, size_t texels, unsigned alpha_mode,
    struct texture_upscale_encode_metrics *metrics);
const char *texture_upscale_encoder_name(uint32_t format);
/* Require consecutive mip sizes and a 1x/2x/4x/8x source-size multiple. Preserve
   the supplied tail: original DXT chains may end at 4x4. On OK,
   *record is malloc'd and *size includes its header; the caller writes it through
   texture_cache_write_entry, then saves its manifest. Exclusion/error returns no
   record. Threads is capped at four to bound codec scratch; zero selects that cap.
   A quality miss retries level 0 before encoding the remaining levels; when every preset misses, the record is
   BGRA8 (metrics->raw), so EXCLUDED is no longer returned. alpha_mode is TEXTURE_UPSCALE_ENCODE_ALPHA_*. */
enum texture_upscale_encode_result texture_upscale_encode_record(
    const struct texture_upscale_rgba_level *levels, unsigned count,
    uint32_t source_width, uint32_t source_height, uint64_t hash, uint64_t recipe_key,
    uint32_t format, unsigned alpha_mode, unsigned threads, void **record, size_t *size,
    struct texture_upscale_encode_metrics *metrics);
#ifdef __cplusplus
}
#endif
