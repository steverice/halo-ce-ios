/* Apache-2.0. The stored-level boundaries follow texture-s4gx-set.py,
   texture-s4gbp-set.py, and texture-resolution-set.py. */
#include "texture_upscale_generate.h"
#include "texture_refine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

struct texture_upscale_generator {
    struct texture_upscale_model *model;
    uint64_t peak_footprint;
};
struct image {
    float *pixels;
    uint32_t width, height, channels;
};
struct bump_source {
    float *height;
    float low, span;
};

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void sample_footprint(struct texture_upscale_generator *generator)
{
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t size = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &size) == KERN_SUCCESS &&
        info.phys_footprint > generator->peak_footprint) generator->peak_footprint = info.phys_footprint;
#else
    (void)generator;
#endif
}
static int fail(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}
static size_t count(uint32_t width, uint32_t height, uint32_t channels)
{
    if (!width || !height || !channels || (size_t)width > SIZE_MAX / height / channels / sizeof(float)) return 0;
    return (size_t)width * height * channels;
}
static struct image allocate(uint32_t width, uint32_t height, uint32_t channels)
{
    size_t n = count(width, height, channels);
    return (struct image){n ? malloc(n * sizeof(float)) : NULL, width, height, channels};
}
static void release(struct image *image)
{
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}
static struct image unit(const struct texture_upscale_rgba_level *level)
{
    struct image result = allocate(level->width, level->height, 4);
    if (result.pixels)
        for (size_t i = 0; i < (size_t)level->width * level->height * 4; i++) result.pixels[i] = level->pixels[i] / 255.0f;
    return result;
}
static struct image select_channels(const struct image *source, uint32_t channels)
{
    struct image result = allocate(source->width, source->height, channels);
    if (result.pixels)
        for (size_t i = 0; i < (size_t)source->width * source->height; i++)
            memcpy(result.pixels + i * channels, source->pixels + i * source->channels, channels * sizeof(float));
    return result;
}
static int half(struct image *image, int wrap)
{
    struct image result = allocate(image->width > 1 ? image->width / 2 : 1,
        image->height > 1 ? image->height / 2 : 1, image->channels);
    if (!result.pixels || texture_upscale_down2(image->pixels, image->width, image->height, image->channels, wrap,
        result.pixels)) { release(&result); return -1; }
    release(image);
    *image = result;
    return 0;
}
static uint8_t quantize(float x)
{
    return (uint8_t)lrintf(fminf(1, fmaxf(0, x)) * 255);
}
/* The Mac bpf pass reads an RGBA8 file, including its thresholded binary alpha. */
static void quantize_in_place(struct image *image, enum texture_upscale_alpha alpha)
{
    size_t n = (size_t)image->width * image->height;
    for (size_t i = 0; i < n; i++)
        for (uint32_t c = 0; c < image->channels; c++) {
            float x = image->pixels[i * image->channels + c];
            if (c == 3 && alpha == TEXTURE_UPSCALE_ALPHA_BINARY) x = x >= .5f;
            else if (c == 3 && alpha == TEXTURE_UPSCALE_ALPHA_NONE) x = 1;
            image->pixels[i * image->channels + c] = quantize(x) / 255.0f;
        }
}
static uint8_t *finish(const struct image *image, enum texture_upscale_alpha alpha)
{
    size_t n = (size_t)image->width * image->height;
    uint8_t *out = malloc(n * 4);
    if (!out) return NULL;
    if (image->channels == 4) texture_upscale_finish(image->pixels, image->width, image->height, alpha, out);
    else for (size_t i = 0; i < n; i++) {
        for (unsigned c = 0; c < 3; c++) out[4 * i + c] = quantize(image->pixels[image->channels * i + c]);
        out[4 * i + 3] = 255;
    }
    return out;
}
static int finite_image(const struct image *image)
{
    for (size_t i = 0; i < (size_t)image->width * image->height * image->channels; i++)
        if (!isfinite(image->pixels[i])) return 0;
    return 1;
}
static unsigned channels_for(const struct texture_upscale_generate_input *input)
{
    return input->alpha != TEXTURE_UPSCALE_ALPHA_NONE || input->decision.treatment == TEXTURE_POLICY_MULTIPURPOSE ? 4 : 3;
}
static int color_like(const struct texture_upscale_generate_input *input)
{
    return input->decision.treatment == TEXTURE_POLICY_COLOR || input->decision.treatment == TEXTURE_POLICY_FIRST_PERSON ||
        input->decision.treatment == TEXTURE_POLICY_EMISSIVE;
}
static struct texture_refine_params params_for(const struct texture_policy_table *policy)
{
    return (struct texture_refine_params){policy->low, policy->high, policy->sigma, (uint32_t)policy->iterations};
}

struct texture_upscale_generator *texture_upscale_generate_create(char *error, size_t error_size)
{
    struct texture_upscale_generator *generator = calloc(1, sizeof(*generator));
    if (!generator) { fail(error, error_size, "could not allocate texture generator"); return NULL; }
    generator->model = texture_upscale_model_create(error, error_size);
    if (!generator->model) { free(generator); return NULL; }
    return generator;
}
void texture_upscale_generate_destroy(struct texture_upscale_generator *generator)
{
    if (!generator) return;
    texture_upscale_model_destroy(generator->model);
    free(generator);
}
void texture_upscale_generate_stats(struct texture_upscale_generator *generator, struct texture_upscale_model_stats *stats)
{
    if (stats) {
        memset(stats, 0, sizeof(*stats));
        if (generator) {
            texture_upscale_model_get_stats(generator->model, stats);
            if (generator->peak_footprint > stats->peak_footprint) stats->peak_footprint = generator->peak_footprint;
        }
    }
}
void texture_upscale_generate_free_output(struct texture_upscale_generate_output *output)
{
    if (!output) return;
    for (unsigned i = 0; i < output->level_count; i++) free((void *)output->levels[i].pixels);
    free(output->levels);
    memset(output, 0, sizeof(*output));
}

/* S4g has independent gray-image channels. Keeping only one channel's 4x
   prediction alive also avoids allocating a full 4x RGBA image for a 2x result. */
static int infer_color(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, const struct texture_upscale_rgba_level *source,
    unsigned target, struct image *out, struct texture_upscale_generate_times *times, char *error, size_t error_size)
{
    double start = seconds();
    struct image reference = unit(source), channel = {0}, prediction = {0};
    unsigned channels = channels_for(input);
    int result = -1;
    if (!reference.pixels) goto done;
    if (input->dilate && input->decision.treatment != TEXTURE_POLICY_MULTIPURPOSE &&
        texture_upscale_dilate(reference.pixels, source->width, source->height)) goto done;
    *out = allocate(source->width * target, source->height * target, channels);
    channel = allocate(source->width, source->height, 1);
    if (!out->pixels || !channel.pixels) goto done;
    for (unsigned c = 0; c < channels; c++) {
        int constant_opaque = c == 3;
        for (size_t i = 0; i < (size_t)source->width * source->height; i++) {
            channel.pixels[i] = reference.pixels[4 * i + c];
            if (c == 3 && source->pixels[4 * i + 3] != 255) constant_opaque = 0;
        }
        if (constant_opaque) {
            for (size_t i = 0; i < (size_t)out->width * out->height; i++) out->pixels[i * channels + c] = 1;
            continue;
        }
        if (target == 4) {
            if (texture_upscale_model_predict4x_strided(generator->model, channel.pixels, source->width, source->height,
                1, input->wrap, out->pixels + c, channels, error, error_size)) goto done;
        } else {
            prediction = allocate(source->width * 4, source->height * 4, 1);
            if (!prediction.pixels) goto done;
            if (texture_upscale_model_predict4x(generator->model, channel.pixels, source->width, source->height, 1,
                input->wrap, prediction.pixels, error, error_size) || half(&prediction, input->wrap)) goto done;
            for (size_t i = 0; i < (size_t)out->width * out->height; i++) out->pixels[i * channels + c] = prediction.pixels[i];
            release(&prediction);
        }
    }
    if (!finite_image(out)) { fail(error, error_size, "model produced nonfinite color"); goto done; }
    sample_footprint(generator);
    result = 0;
done:
    release(&reference); release(&channel); release(&prediction);
    if (result) { release(out); if (!error || !error_size || !error[0]) fail(error, error_size, "color inference failed"); }
    times->predict += seconds() - start;
    return result;
}
static int bump_original(const struct texture_upscale_rgba_level *source, struct bump_source *bump,
    struct texture_upscale_generate_times *times)
{
    double start = seconds();
    bump->height = malloc((size_t)source->width * source->height * sizeof(float));
    int result = !bump->height ? -1 : texture_upscale_integrate_normals(source->pixels, source->width, source->height,
        bump->height, &bump->low, &bump->span);
    times->bump += seconds() - start;
    return result;
}
static int infer_height(struct texture_upscale_generator *generator, const struct texture_upscale_rgba_level *source,
    const struct bump_source *bump, struct image *raw, struct texture_upscale_generate_times *times,
    char *error, size_t error_size)
{
    double start = seconds();
    struct image unit_height = allocate(source->width, source->height, 1);
    *raw = allocate(source->width * 4, source->height * 4, 1);
    int result = -1;
    if (!unit_height.pixels || !raw->pixels) goto done;
    for (size_t i = 0; i < (size_t)source->width * source->height; i++)
        unit_height.pixels[i] = (bump->height[i] - bump->low) / bump->span;
    result = texture_upscale_model_predict4x(generator->model, unit_height.pixels, source->width, source->height, 1, 1,
        raw->pixels, error, error_size);
    if (!result && !finite_image(raw)) result = fail(error, error_size, "model produced nonfinite height");
done:
    release(&unit_height);
    if (result) release(raw);
    times->predict += seconds() - start;
    return result;
}
static int infer_alpha(struct texture_upscale_generator *generator, const struct texture_upscale_rgba_level *source,
    unsigned scale, struct image *alpha, struct texture_upscale_generate_times *times, char *error, size_t error_size)
{
    double start = seconds();
    struct image reference = allocate(source->width, source->height, 1);
    *alpha = allocate(source->width * 4, source->height * 4, 1);
    int result = -1;
    if (!reference.pixels || !alpha->pixels) goto done;
    for (size_t i = 0; i < (size_t)source->width * source->height; i++) reference.pixels[i] = source->pixels[4 * i + 3] / 255.0f;
    if (texture_upscale_model_predict4x(generator->model, reference.pixels, source->width, source->height, 1, 1,
        alpha->pixels, error, error_size)) goto done;
    if (scale == 2 && half(alpha, 1)) goto done;
    if (!finite_image(alpha)) { fail(error, error_size, "model produced nonfinite alpha"); goto done; }
    result = 0;
done:
    release(&reference);
    if (result) release(alpha);
    times->predict += seconds() - start;
    return result;
}
static int refine_color(struct image *top, const struct image *reference4,
    unsigned scale, int wrap, const struct texture_refine_params *params, struct texture_upscale_generate_times *times)
{
    double start = seconds();
    struct image reference = select_channels(reference4, top->channels);
    int result = !reference.pixels ? -1 : texture_refine_color_bounded(top->pixels, reference.pixels,
        reference.width, reference.height, top->channels, scale, wrap, params, 1, NULL);
    release(&reference);
    times->refine += seconds() - start;
    return result;
}
static int measure(const struct image *top, const struct image *reference, unsigned scale, int wrap, int visible,
    double *share, double *loss, struct texture_upscale_generate_times *times)
{
    double start = seconds();
    int result = texture_upscale_damage(top->pixels, top->channels, reference->pixels, reference->width, reference->height,
        scale, wrap, visible, share, loss);
    /* The reference JSON rounds each measure to four decimals before selection. */
    if (!result) { *share = nearbyint(*share * 10000) / 10000; *loss = nearbyint(*loss * 10000) / 10000; }
    times->damage += seconds() - start;
    return result;
}
static int append_level(struct texture_upscale_generate_output *output, const struct image *image,
    enum texture_upscale_alpha alpha)
{
    uint8_t *bytes = finish(image, alpha);
    if (!bytes) return -1;
    output->levels[output->level_count++] = (struct texture_upscale_rgba_level){bytes, image->width, image->height};
    return 0;
}
static int prepend_level(struct texture_upscale_generate_output *output, const struct image *image,
    enum texture_upscale_alpha alpha)
{
    uint8_t *bytes = finish(image, alpha);
    if (!bytes) return -1;
    memmove(output->levels + 1, output->levels, output->level_count * sizeof(*output->levels));
    output->levels[0] = (struct texture_upscale_rgba_level){bytes, image->width, image->height};
    output->level_count++;
    return 0;
}

/* raw is unit height from S4g. Height remains in original-texel units through
   back-projection; derive_normals applies the final texel-size factor. */
static int bump_top(struct texture_upscale_generator *generator, const struct texture_upscale_generate_input *input,
    const struct bump_source *bump, const struct image *raw, unsigned scale, int fallback, struct image *out,
    const struct texture_refine_params *params, struct texture_upscale_generate_times *times, char *error, size_t error_size)
{
    const struct texture_upscale_rgba_level *source = input->levels;
    struct image height = allocate(raw->width, raw->height, 1), alpha = {0}, reference = {0};
    double start = seconds();
    int result = -1;
    if (!height.pixels) goto done;
    for (size_t i = 0; i < (size_t)raw->width * raw->height; i++) height.pixels[i] = raw->pixels[i] * bump->span + bump->low;
    if (scale == 2 && half(&height, 1)) goto done;
    *out = allocate(height.width, height.height, input->alpha == TEXTURE_UPSCALE_ALPHA_NONE ? 3 : 4);
    if (!out->pixels) goto done;
    /* Derive into the beginning of the final allocation, packed XYZ. Expand
       backward to RGBA after alpha is ready, avoiding a second normal image. */
    if (fallback) {
        if (texture_refine_bump_bounded(height.pixels, bump->height, source->width, source->height, scale,
            params, 1, out->pixels, NULL)) goto done;
    } else texture_upscale_derive_normals(height.pixels, height.width, height.height, (float)scale, out->pixels);
    times->refine += seconds() - start;
    release(&height);
    if (input->alpha != TEXTURE_UPSCALE_ALPHA_NONE) {
        if (infer_alpha(generator, source, scale, &alpha, times, error, error_size)) goto done;
        if (fallback) {
            /* Base bpf restores alpha from the already quantized S4g top;
               the newly prepended 4x level back-projects the raw alpha. */
            if (scale == 2) {
                for (size_t i = 0; i < (size_t)alpha.width * alpha.height; i++) {
                    float a = alpha.pixels[i];
                    alpha.pixels[i] = quantize(input->alpha == TEXTURE_UPSCALE_ALPHA_BINARY ? a >= .5f : a) / 255.0f;
                }
            }
            reference = allocate(source->width, source->height, 1);
            if (!reference.pixels) goto done;
            for (size_t i = 0; i < (size_t)source->width * source->height; i++) reference.pixels[i] = source->pixels[4 * i + 3] / 255.0f;
            start = seconds();
            if (texture_refine_color_bounded(alpha.pixels, reference.pixels, source->width, source->height, 1,
                scale, 1, params, 0, NULL)) goto done;
            times->refine += seconds() - start;
        }
    }
    for (size_t i = (size_t)out->width * out->height; i-- > 0;) {
        float rgb[3];
        for (unsigned c = 0; c < 3; c++) rgb[c] = (out->pixels[i * 3 + c] + 1) * .5f;
        for (unsigned c = 0; c < 3; c++) out->pixels[i * out->channels + c] = rgb[c];
        if (out->channels == 4) out->pixels[i * 4 + 3] = alpha.pixels[i];
    }
    sample_footprint(generator);
    if (!finite_image(out)) { fail(error, error_size, "bump refinement produced nonfinite normals"); goto done; }
    result = 0;
done:
    release(&height); release(&alpha); release(&reference);
    if (result) release(out);
    return result;
}

static int valid_input(const struct texture_upscale_generate_input *input)
{
    if (!input || !input->levels || !input->level_count || input->level_count > 32 ||
        !input->decision.width || !input->decision.height ||
        (input->decision.result != TEXTURE_POLICY_S4G && input->decision.result != TEXTURE_POLICY_BPF &&
         input->decision.result != TEXTURE_POLICY_ORIGINAL) || input->decision.width > UINT32_MAX / 8 ||
        input->decision.height > UINT32_MAX / 8 || (int)input->alpha < TEXTURE_UPSCALE_ALPHA_NONE || input->alpha > TEXTURE_UPSCALE_ALPHA_GRADED) return 0;
    uint32_t w = input->decision.width, h = input->decision.height;
    for (unsigned i = 0; i < input->level_count; i++) {
        if (!input->levels[i].pixels || input->levels[i].width != w || input->levels[i].height != h || !count(w, h, 4)) return 0;
        if (w == 1 && h == 1) return i + 1 == input->level_count;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return 1;
}

int texture_upscale_generate(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, struct texture_upscale_generate_output *output,
    char *error, size_t error_size)
{
    if (error && error_size) error[0] = 0;
    if (!generator || !output || !valid_input(input)) return fail(error, error_size, "invalid texture generator input");
    memset(output, 0, sizeof(*output));
    output->result = input->decision.result;
    output->scale = input->decision.scale;
    output->damage_share = output->structure_loss_s4g = output->structure_loss_bpf = output->structure_loss_resize = -1;
    if (output->result == TEXTURE_POLICY_ORIGINAL) { output->scale = 1; return 0; }
    unsigned base = input->decision.treatment == TEXTURE_POLICY_FIRST_PERSON ? 4 : 2;
    unsigned requested = input->decision.scale;
    if ((requested != base && requested != 2 * base) || input->decision.treatment == TEXTURE_POLICY_SKIP)
        return fail(error, error_size, "unsupported texture treatment or scale");
    const struct texture_policy_table *policy = input->policy ? input->policy : &texture_policy_embedded;
    struct texture_refine_params params = params_for(policy);
    struct image top = {0}, reference = {0}, resized = {0}, raw_height = {0};
    struct bump_source bump = {0};
    int is_bump = input->decision.treatment == TEXTURE_POLICY_BUMP_MAP;
    int result = -1;
    double started;
    output->levels = calloc(input->level_count + 3, sizeof(*output->levels));
    if (!output->levels) goto done;
    reference = unit(input->levels);
    if (!reference.pixels) goto done;
    if (input->dilate && color_like(input) && texture_upscale_dilate(reference.pixels, reference.width, reference.height)) goto done;
    if (is_bump) {
        if (bump_original(input->levels, &bump, &output->seconds) ||
            infer_height(generator, input->levels, &bump, &raw_height, &output->seconds, error, error_size) ||
            bump_top(generator, input, &bump, &raw_height, base, output->result == TEXTURE_POLICY_BPF, &top, &params,
                &output->seconds, error, error_size)) goto done;
    } else {
        if (infer_color(generator, input, input->levels, base, &top, &output->seconds, error, error_size)) goto done;
        /* Every base refinement starts from the stored S4g top. */
        quantize_in_place(&top, input->alpha);
        int check_damage = color_like(input);
        if (check_damage && measure(&top, &reference, base, input->wrap, input->alpha != TEXTURE_UPSCALE_ALPHA_NONE,
            &output->damage_share, &output->structure_loss_s4g, &output->seconds)) goto done;
        int damage_fired = check_damage && !input->pinned && output->result == TEXTURE_POLICY_S4G &&
            ((policy->damage_share >= 0 && (float)output->damage_share >= policy->damage_share) ||
             (policy->damage_structure_loss >= 0 && (float)output->structure_loss_s4g >= policy->damage_structure_loss));
        if (damage_fired) output->result = TEXTURE_POLICY_BPF;
        if (output->result == TEXTURE_POLICY_BPF) {
            if (refine_color(&top, &reference, base, input->wrap, &params, &output->seconds)) goto done;
            if (check_damage) {
                /* damage.stats measures the stored bpf bytes, while its 2x mip
                   (for a 4x base) is downsampled from the unquantized refinement. */
                struct image stored = allocate(top.width, top.height, top.channels);
                if (!stored.pixels) goto done;
                memcpy(stored.pixels, top.pixels, count(top.width, top.height, top.channels) * sizeof(float));
                quantize_in_place(&stored, input->alpha);
                sample_footprint(generator);
                double ignored;
                int measured = measure(&stored, &reference, base, input->wrap, input->alpha != TEXTURE_UPSCALE_ALPHA_NONE,
                    &ignored, &output->structure_loss_bpf, &output->seconds);
                release(&stored);
                if (measured) goto done;
                if (damage_fired && policy->damage_bpf_structure_loss >= 0) {
                    resized = allocate(reference.width * base, reference.height * base, 3);
                    struct image rgb = select_channels(&reference, 3);
                    started = seconds();
                    int resized_ok = resized.pixels && rgb.pixels && !texture_upscale_resize(rgb.pixels, rgb.width,
                        rgb.height, 3, base, input->wrap, resized.pixels);
                    release(&rgb);
                    output->seconds.damage += seconds() - started;
                    if (!resized_ok) goto done;
                    for (size_t i = 0; i < count(resized.width, resized.height, 3); i++)
                        resized.pixels[i] = fminf(1, fmaxf(0, resized.pixels[i]));
                    if (measure(&resized, &reference, base, input->wrap, input->alpha != TEXTURE_UPSCALE_ALPHA_NONE,
                        &ignored, &output->structure_loss_resize, &output->seconds)) goto done;
                    release(&resized);
                    if ((float)(output->structure_loss_bpf - output->structure_loss_resize) >= policy->damage_bpf_structure_loss) {
                        output->result = TEXTURE_POLICY_ORIGINAL;
                        output->scale = 1;
                        result = 0;
                        goto done;
                    }
                }
            }
        }
    }
    started = seconds();
    if (append_level(output, &top, input->alpha)) goto done;
    sample_footprint(generator);
    if (base == 4) {
        if (output->result == TEXTURE_POLICY_S4G) {
            /* The 4x PNG is quantized for selection, but S4g's 2x mip was
               filtered before that quantization. Recompute one channel at a
               time rather than retain a second full-size float image. */
            release(&top);
            output->seconds.mips += seconds() - started;
            if (infer_color(generator, input, input->levels, 2, &top, &output->seconds, error, error_size)) goto done;
            started = seconds();
        } else if (half(&top, input->wrap)) goto done;
        if (append_level(output, &top, input->alpha)) goto done;
    }
    release(&top);
    output->seconds.mips += seconds() - started;
    if (requested > base) {
        if (is_bump) {
            if (bump_top(generator, input, &bump, &raw_height, requested, output->result == TEXTURE_POLICY_BPF, &top,
                &params, &output->seconds, error, error_size)) goto done;
        } else {
            /* The 8x set infers from its approved 2x mip. The 4x set uses
               S4g's quantized cached 4x image; lower approved mips stay intact. */
            const struct texture_upscale_rgba_level *model_input = requested == 8 ? &output->levels[1] : input->levels;
            if (infer_color(generator, input, model_input, 4, &top, &output->seconds, error, error_size)) goto done;
            if (requested == 4) quantize_in_place(&top, input->alpha);
            if (output->result == TEXTURE_POLICY_BPF &&
                refine_color(&top, &reference, requested, input->wrap, &params, &output->seconds)) goto done;
        }
        started = seconds();
        if (prepend_level(output, &top, input->alpha)) goto done;
        sample_footprint(generator);
        release(&top);
        output->seconds.mips += seconds() - started;
    }
    started = seconds();
    for (unsigned i = 0; i < input->level_count; i++) {
        const struct texture_upscale_rgba_level *source = &input->levels[i];
        size_t bytes = (size_t)source->width * source->height * 4;
        uint8_t *copy = malloc(bytes);
        if (!copy) goto done;
        memcpy(copy, source->pixels, bytes);
        output->levels[output->level_count++] = (struct texture_upscale_rgba_level){copy, source->width, source->height};
    }
    output->seconds.mips += seconds() - started;
    result = 0;
done:
    release(&top); release(&reference); release(&resized); release(&raw_height); free(bump.height);
    if (result) {
        texture_upscale_generate_free_output(output);
        if (!error || !error_size || !error[0]) fail(error, error_size, "texture generation failed");
    } else if (output->result == TEXTURE_POLICY_ORIGINAL) {
        for (unsigned i = 0; i < output->level_count; i++) free((void *)output->levels[i].pixels);
        free(output->levels); output->levels = NULL; output->level_count = 0;
    }
    return result;
}

int texture_upscale_generate_raw4x(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, float **pixels, unsigned *channels, char *error, size_t error_size)
{
    if (error && error_size) error[0] = 0;
    if (pixels) *pixels = NULL;
    if (channels) *channels = 0;
    if (!generator || !pixels || !channels || !valid_input(input)) return fail(error, error_size, "invalid raw prediction input");
    struct texture_upscale_generate_times times = {0};
    struct image raw = {0};
    struct bump_source bump = {0};
    int result;
    if (input->decision.treatment == TEXTURE_POLICY_BUMP_MAP) {
        result = bump_original(input->levels, &bump, &times);
        if (!result) result = infer_height(generator, input->levels, &bump, &raw, &times, error, error_size);
        free(bump.height);
    } else result = infer_color(generator, input, input->levels, 4, &raw, &times, error, error_size);
    if (result) { release(&raw); return fail(error, error_size, "raw prediction failed"); }
    *pixels = raw.pixels; *channels = raw.channels;
    return 0;
}
