/* S4g, refinement, damage selection, and the Mac set's original mip tail. */
#pragma once
#include "texture_upscale_encode.h"
#include "texture_upscale_model.h"
#include "texture_upscale_pixels.h"
#include "texture_policy.h"
#ifdef __cplusplus
extern "C" {
#endif

struct texture_upscale_generator;
struct texture_upscale_generate_input {
    struct texture_policy_decision decision;
    const struct texture_policy_table *policy; /* NULL selects the embedded policy */
    const struct texture_upscale_rgba_level *levels; /* decoded originals, including their stored mip tail */
    unsigned level_count;
    enum texture_upscale_alpha alpha;
    int wrap, dilate, pinned;
};
struct texture_upscale_generate_times {
    double predict, refine, damage, mips, bump;
};
struct texture_upscale_generate_output {
    struct texture_upscale_rgba_level *levels; /* owned, including copies of original levels */
    unsigned level_count;
    enum texture_policy_result result;
    uint32_t scale;
    double damage_share, structure_loss_s4g, structure_loss_bpf, structure_loss_resize; /* -1 when inapplicable */
    struct texture_upscale_generate_times seconds;
};
struct texture_upscale_generator *texture_upscale_generate_create(char *error, size_t error_size);
void texture_upscale_generate_destroy(struct texture_upscale_generator *generator);
void texture_upscale_generate_stats(struct texture_upscale_generator *generator,
    struct texture_upscale_model_stats *stats);
/* A successful original decision has no output levels. An error leaves output empty.
   Access to one generator must be serialized. Output starts zeroed and is released with free_output. */
int texture_upscale_generate(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, struct texture_upscale_generate_output *output,
    char *error, size_t error_size);
void texture_upscale_generate_free_output(struct texture_upscale_generate_output *output);
/* Model output before quantization or refinement, for half-offset seam checks. RGB + independent alpha,
   except opaque color has RGB only; bump has unit height only. Caller frees *pixels. */
int texture_upscale_generate_raw4x(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, float **pixels, unsigned *channels,
    char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
