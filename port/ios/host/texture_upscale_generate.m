/* Apache-2.0. The stored-level boundaries follow texture-s4gx-set.py,
   texture-s4gbp-set.py, and texture-resolution-set.py. */
#include "texture_upscale_generate.h"
#include "texture_refine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

struct texture_upscale_generator {
    struct texture_upscale_model *model;
    pthread_mutex_t model_lock;    /* begin and end may run on two threads; the model takes one call at a time */
    uint64_t peak_footprint, texture_peak;    /* sampled phys_footprint: the whole run, and the current texture */
    char trace[384];    /* development: the footprint at each step of the last texture begun */
};
/* pixels, or half for a large top kept as _Float16 (S4g's own output precision) */
struct image {
    float *pixels;
    uint32_t width, height, channels;
    _Float16 *half;
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
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &size) == KERN_SUCCESS) {
        if (info.phys_footprint > generator->peak_footprint) generator->peak_footprint = info.phys_footprint;
        if (info.phys_footprint > generator->texture_peak) generator->texture_peak = info.phys_footprint;
    }
#else
    (void)generator;
#endif
}
static void trace(struct texture_upscale_generator *generator, const char *step)
{
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t size = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &size) != KERN_SUCCESS) return;
    size_t used = strlen(generator->trace);
    if (used < sizeof(generator->trace))
        snprintf(generator->trace + used, sizeof(generator->trace) - used, "%s %llu ", step,
            (unsigned long long)(info.phys_footprint >> 20));
#else
    (void)generator; (void)step;
#endif
}
const char *texture_upscale_generate_trace(struct texture_upscale_generator *generator) { return generator->trace; }
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
    return (struct image){n ? malloc(n * sizeof(float)) : NULL, width, height, channels, NULL};
}
static struct image allocate_half(uint32_t width, uint32_t height, uint32_t channels)
{
    size_t n = count(width, height, channels);
    return (struct image){NULL, width, height, channels, n ? malloc(n * sizeof(_Float16)) : NULL};
}
static int allocated(const struct image *image) { return image->pixels || image->half; }
static void release(struct image *image)
{
    free(image->pixels); free(image->half);
    memset(image, 0, sizeof(*image));
}
static float value_at(const struct image *image, size_t i) { return image->half ? (float)image->half[i] : image->pixels[i]; }
static void set_value(struct image *image, size_t i, float x)
{
    if (image->half) image->half[i] = (_Float16)x;
    else image->pixels[i] = x;
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
static int half_into(const struct image *image, int wrap, struct image *out)
{
    *out = allocate(image->width > 1 ? image->width / 2 : 1, image->height > 1 ? image->height / 2 : 1, image->channels);
    if (!out->pixels || texture_upscale_down2(image->pixels, image->width, image->height, image->channels, wrap,
        out->pixels)) { release(out); return -1; }
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
            float x = value_at(image, i * image->channels + c);
            if (c == 3 && alpha == TEXTURE_UPSCALE_ALPHA_BINARY) x = x >= .5f;
            else if (c == 3 && alpha == TEXTURE_UPSCALE_ALPHA_NONE) x = 1;
            set_value(image, i * image->channels + c, quantize(x) / 255.0f);
        }
}
/* RGBA8 bytes. A half image is converted in its own allocation (four bytes per texel never overtake six or eight
   bytes of half values still to be read) and handed over, so a 4096 px top never exists twice. */
static uint8_t *finish(struct image *image, enum texture_upscale_alpha alpha)
{
    size_t n = (size_t)image->width * image->height;
    if (image->half) {
        uint8_t *out = (uint8_t *)image->half;
        for (size_t i = 0; i < n; i++) {
            float rgba[4] = {0, 0, 0, 1};
            for (unsigned c = 0; c < image->channels; c++) rgba[c] = (float)image->half[i * image->channels + c];
            for (unsigned c = 0; c < 4; c++) {
                float v = fminf(1, fmaxf(0, rgba[c]));
                if (c == 3 && (alpha == TEXTURE_UPSCALE_ALPHA_NONE || image->channels == 3)) v = 1;
                else if (c == 3 && alpha == TEXTURE_UPSCALE_ALPHA_BINARY) v = v >= 0.5f;
                out[4 * i + c] = (uint8_t)nearbyintf(v * 255);
            }
        }
        uint8_t *shrunk = realloc(out, n * 4);
        image->half = NULL;
        release(image);
        return shrunk ? shrunk : out;
    }
    uint8_t *out = malloc(n * 4);
    if (!out) return NULL;
    if (image->channels == 4) texture_upscale_finish(image->pixels, image->width, image->height, alpha, out);
    else for (size_t i = 0; i < n; i++) {
        for (unsigned c = 0; c < 3; c++) out[4 * i + c] = quantize(image->pixels[image->channels * i + c]);
        out[4 * i + 3] = 255;
    }
    return out;
}
static int predict_tiles(struct texture_upscale_generator *generator, const float *input, uint32_t width, uint32_t height,
    uint32_t channels, int wrap, texture_upscale_tile_consumer consumer, void *context, char *error, size_t error_size)
{
    pthread_mutex_lock(&generator->model_lock);
    int result = texture_upscale_model_predict_tiles(generator->model, input, width, height, channels, wrap, consumer,
        context, error, error_size);
    pthread_mutex_unlock(&generator->model_lock);
    return result;
}
static int predict4x(struct texture_upscale_generator *generator, const float *input, uint32_t width, uint32_t height,
    uint32_t channels, int wrap, float *output, char *error, size_t error_size)
{
    pthread_mutex_lock(&generator->model_lock);
    int result = texture_upscale_model_predict4x(generator->model, input, width, height, channels, wrap, output,
        error, error_size);
    pthread_mutex_unlock(&generator->model_lock);
    return result;
}
static int finite_image(const struct image *image)
{
    for (size_t i = 0; i < (size_t)image->width * image->height * image->channels; i++)
        if (!isfinite(value_at(image, i))) return 0;
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
    pthread_mutex_init(&generator->model_lock, NULL);
    return generator;
}
void texture_upscale_generate_destroy(struct texture_upscale_generator *generator)
{
    if (!generator) return;
    texture_upscale_model_destroy(generator->model);
    pthread_mutex_destroy(&generator->model_lock);
    free(generator);
}
void texture_upscale_generate_stats(struct texture_upscale_generator *generator, struct texture_upscale_model_stats *stats)
{
    if (stats) {
        memset(stats, 0, sizeof(*stats));
        if (generator) {
            texture_upscale_model_get_stats(generator->model, stats);
            stats->sampled_peak = generator->peak_footprint;
            stats->texture_peak = generator->texture_peak;
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

/* Where S4g's tiles go. x4 receives the 4x prediction (float or half storage); x2 receives Lanczos down2 of it, as
   half() on the whole 4x image computes it. A texture of more than one tile computes x2 inside each tile, from the
   4x pixels S4g predicted around that tile's core with real neighbors, so no 4x image is needed for a 2x result. */
struct targets {
    struct image *x4, *x2;
    uint32_t width, height, channel;    /* source size; first channel this prediction fills */
    int wrap, tiled;
    double weights[12];
};
static void write_x4(struct targets *t, const struct texture_upscale_tile *tile, uint32_t c)
{
    struct image *x4 = t->x4;
    for (uint32_t y = 0; y < tile->height * 4; y++) {
        size_t row = ((size_t)tile->y * 4 + y) * x4->width + (size_t)tile->x * 4;
        for (uint32_t x = 0; x < tile->width * 4; x++)
            set_value(x4, (row + x) * x4->channels + c, texture_upscale_tile_at(tile, (int32_t)x, (int32_t)y));
    }
}
static int32_t tile_local(int64_t global, uint32_t size, uint32_t origin, int wrap)
{
    if (!wrap) global = global < 0 ? 0 : global >= size ? size - 1 : global;
    return (int32_t)(global - origin);
}
static int write_x2(struct targets *t, const struct texture_upscale_tile *tile, uint32_t c)
{
    struct image *x2 = t->x2;
    uint32_t w4 = t->width * 4, h4 = t->height * 4, ox = tile->x * 4, oy = tile->y * 4;
    int64_t first = (int64_t)ox - 5, last = (int64_t)(tile->x + tile->width) * 4 + 4;    /* 4x columns read */
    double *rows = malloc((size_t)(last - first + 1) * sizeof(double));
    if (!rows) return -1;
    for (uint32_t y = tile->y * 2; y < (tile->y + tile->height) * 2; y++) {
        int32_t ly[12];
        for (int k = 0; k < 12; k++) ly[k] = tile_local((int64_t)y * 2 + k - 5, h4, oy, t->wrap);
        for (int64_t g = first; g <= last; g++) {
            int32_t lx = tile_local(g, w4, ox, t->wrap);
            double value = 0;
            for (int k = 0; k < 12; k++) value += t->weights[k] * texture_upscale_tile_at(tile, lx, ly[k]);
            rows[g - first] = value;
        }
        for (uint32_t x = tile->x * 2; x < (tile->x + tile->width) * 2; x++) {
            double value = 0;
            for (int k = 0; k < 12; k++) {
                int64_t g = (int64_t)x * 2 + k - 5;
                if (!t->wrap) g = g < 0 ? 0 : g >= w4 ? w4 - 1 : g;
                value += t->weights[k] * rows[g - first];
            }
            x2->pixels[((size_t)y * x2->width + x) * x2->channels + c] = (float)value;
        }
    }
    free(rows);
    return 0;
}
static int consume_tile(void *context, const struct texture_upscale_tile *tile, uint32_t channel)
{
    struct targets *t = context;
    uint32_t c = t->channel + channel;
    if (t->x4) write_x4(t, tile, c);
    return t->tiled && t->x2 ? write_x2(t, tile, c) : 0;
}
static double lanczos2(double x) { return x == 0 ? 1 : fabs(x) < 3 ? 3 * sin(M_PI * x) * sin(M_PI * x / 3) / (M_PI * M_PI * x * x) : 0; }
static void down_weights(double weights[12])
{
    double sum = 0;
    for (int k = 0; k < 12; k++) sum += weights[k] = lanczos2((k - 5 - 0.5) / 2);
    for (int k = 0; k < 12; k++) weights[k] /= sum;
}

/* S4g of a level, each channel predicted once: x4 and x2 as wanted (NULL for neither), allocated here with the
   texture's channel count. x4_half stores x4 as _Float16. Alpha (or a multipurpose map's fourth channel) is its own
   single-channel pass, which packs three tiles into one call. */
static int infer_color(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, const struct texture_upscale_rgba_level *source,
    struct image *x4, int x4_half, struct image *x2, struct texture_upscale_generate_times *times,
    char *error, size_t error_size)
{
    double start = seconds();
    unsigned channels = channels_for(input);
    uint32_t w = source->width, h = source->height;
    int tiled = w > 256 || h > 256;
    struct image reference = unit(source), plane = {0}, full = {0}, alpha = {0};
    struct targets t = {x4, x2, w, h, 0, input->wrap, tiled, {0}};
    int result = -1;
    down_weights(t.weights);
    if (x4) *x4 = x4_half ? allocate_half(w * 4, h * 4, channels) : allocate(w * 4, h * 4, channels);
    if (x2) *x2 = allocate(w * 2, h * 2, channels);
    if (!reference.pixels || (x4 && !allocated(x4)) || (x2 && !x2->pixels)) goto done;
    /* One tile: the whole 4x image is small, and half() on it is exactly the Mac's down2. */
    if (x2 && !tiled) {
        if (x4 && !x4_half) t.x4 = x4;
        else { full = allocate(w * 4, h * 4, channels); if (!full.pixels) goto done; t.x4 = &full; }
    }
    if (input->dilate && input->decision.treatment != TEXTURE_POLICY_MULTIPURPOSE &&
        texture_upscale_dilate(reference.pixels, w, h)) goto done;
    /* Only the planes the model reads stay alive while it runs. */
    int constant_opaque = 1;
    if (channels == 4) {
        for (size_t i = 0; i < (size_t)w * h; i++) if (source->pixels[4 * i + 3] != 255) { constant_opaque = 0; break; }
        if (!constant_opaque) {
            alpha = allocate(w, h, 1);
            if (!alpha.pixels) goto done;
            for (size_t i = 0; i < (size_t)w * h; i++) alpha.pixels[i] = reference.pixels[4 * i + 3];
        }
    }
    plane = select_channels(&reference, 3);
    release(&reference);
    if (!plane.pixels || predict_tiles(generator, plane.pixels, w, h, 3, input->wrap,
        consume_tile, &t, error, error_size)) goto done;
    sample_footprint(generator);
    trace(generator, "rgb");
    release(&plane);
    if (channels == 4) {
        if (constant_opaque) {
            if (t.x4) for (size_t i = 0; i < (size_t)w * h * 16; i++) set_value(t.x4, i * 4 + 3, 1);
            if (x2 && tiled) for (size_t i = 0; i < (size_t)w * h * 4; i++) x2->pixels[i * 4 + 3] = 1;
        } else {
            struct targets fourth = t;
            fourth.channel = 3;
            if (predict_tiles(generator, alpha.pixels, w, h, 1, input->wrap, consume_tile, &fourth, error, error_size)) goto done;
            release(&alpha);
        }
    }
    /* A half-storage x4 of a one-tile texture is filled from the float image half() reads. */
    if (x4 && t.x4 == &full)
        for (size_t i = 0; i < count(w * 4, h * 4, channels); i++) set_value(x4, i, full.pixels[i]);
    if (x2 && !tiled && texture_upscale_down2(t.x4->pixels, w * 4, h * 4, channels, input->wrap, x2->pixels)) goto done;
    if ((x4 && !finite_image(x4)) || (x2 && !finite_image(x2))) { fail(error, error_size, "model produced nonfinite color"); goto done; }
    sample_footprint(generator);
    result = 0;
done:
    release(&reference); release(&plane); release(&full); release(&alpha);
    if (result) {
        if (x4) release(x4);
        if (x2) release(x2);
        if (!error || !error_size || !error[0]) fail(error, error_size, "color inference failed");
    }
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
    result = predict4x(generator, unit_height.pixels, source->width, source->height, 1, 1,
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
    if (predict4x(generator, reference.pixels, source->width, source->height, 1, 1,
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
    int result = !reference.pixels ? -1 : top->half ? texture_refine_color_bounded_half(top->half, reference.pixels,
        reference.width, reference.height, top->channels, scale, wrap, params, 1, NULL) :
        texture_refine_color_bounded(top->pixels, reference.pixels,
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
static int append_level(struct texture_upscale_generate_output *output, struct image *image,
    enum texture_upscale_alpha alpha)
{
    uint32_t width = image->width, height = image->height;
    uint8_t *bytes = finish(image, alpha);
    if (!bytes) return -1;
    output->levels[output->level_count++] = (struct texture_upscale_rgba_level){bytes, width, height};
    return 0;
}
static int prepend_level(struct texture_upscale_generate_output *output, struct image *image,
    enum texture_upscale_alpha alpha)
{
    uint32_t width = image->width, height = image->height;
    uint8_t *bytes = finish(image, alpha);
    if (!bytes) return -1;
    memmove(output->levels + 1, output->levels, output->level_count * sizeof(*output->levels));
    output->levels[0] = (struct texture_upscale_rgba_level){bytes, width, height};
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

/* Generation in two steps: begin runs the texture's first predictions on the Neural Engine, end does the CPU work
   (refinement, damage, mips) and any later prediction. A caller may run end for one texture while begin predicts
   the next; the model itself is serialized by the generator. */
struct texture_upscale_job {
    struct texture_upscale_generate_input input;
    struct texture_upscale_generate_output output;
    const struct texture_policy_table *policy;
    struct texture_refine_params params;
    struct image top, reference, raw_height, high, mip;
    struct bump_source bump;
    unsigned base, requested;
    int is_bump;
};

static void job_release(struct texture_upscale_job *job)
{
    if (!job) return;
    release(&job->top); release(&job->reference); release(&job->raw_height); release(&job->high); release(&job->mip);
    free(job->bump.height);
    texture_upscale_generate_free_output(&job->output);
    free(job);
}

void texture_upscale_generate_abandon(struct texture_upscale_job *job) { job_release(job); }

int texture_upscale_generate_begin(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, struct texture_upscale_job **out, char *error, size_t error_size)
{
    if (error && error_size) error[0] = 0;
    if (out) *out = NULL;
    if (!generator || !out || !valid_input(input)) return fail(error, error_size, "invalid texture generator input");
    struct texture_upscale_job *job = calloc(1, sizeof(*job));
    if (!job) return fail(error, error_size, "could not allocate a texture job");
    job->input = *input;
    struct texture_upscale_generate_output *output = &job->output;
    generator->texture_peak = 0;
    generator->trace[0] = 0;
    trace(generator, "begin");
    sample_footprint(generator);
    output->result = input->decision.result;
    output->scale = input->decision.scale;
    output->damage_share = output->structure_loss_s4g = output->structure_loss_bpf = output->structure_loss_resize = -1;
    if (output->result == TEXTURE_POLICY_ORIGINAL) { output->scale = 1; *out = job; return 0; }
    job->base = input->decision.treatment == TEXTURE_POLICY_FIRST_PERSON ? 4 : 2;
    job->requested = input->decision.scale;
    if ((job->requested != job->base && job->requested != 2 * job->base) || input->decision.treatment == TEXTURE_POLICY_SKIP) {
        job_release(job);
        return fail(error, error_size, "unsupported texture treatment or scale");
    }
    job->policy = input->policy ? input->policy : &texture_policy_embedded;
    job->params = params_for(job->policy);
    job->is_bump = input->decision.treatment == TEXTURE_POLICY_BUMP_MAP;
    int result = -1;
    output->levels = calloc(input->level_count + 3, sizeof(*output->levels));
    if (!output->levels) goto done;
    job->reference = unit(input->levels);
    if (!job->reference.pixels) goto done;
    if (input->dilate && color_like(input) && texture_upscale_dilate(job->reference.pixels, job->reference.width, job->reference.height)) goto done;
    if (job->is_bump) {
        if (bump_original(input->levels, &job->bump, &output->seconds) ||
            infer_height(generator, input->levels, &job->bump, &job->raw_height, &output->seconds, error, error_size)) goto done;
    } else {
        /* One prediction of the original serves every level made from it: the 2x base and a 4x top (kept as half),
           or a 4x base and its S4g 2x mip. */
        if (job->base == 2 ? infer_color(generator, input, input->levels, job->requested == 4 ? &job->high : NULL, 1, &job->top,
                &output->seconds, error, error_size) :
            infer_color(generator, input, input->levels, &job->top, 0, &job->mip, &output->seconds, error, error_size)) goto done;
    }
    result = 0;
done:
    if (result) {
        job_release(job);
        if (!error || !error_size || !error[0]) fail(error, error_size, "texture generation failed");
        return -1;
    }
    *out = job;
    return 0;
}

int texture_upscale_generate_end(struct texture_upscale_generator *generator, struct texture_upscale_job *job,
    struct texture_upscale_generate_output *final, char *error, size_t error_size)
{
    if (error && error_size) error[0] = 0;
    if (final) memset(final, 0, sizeof(*final));
    if (!generator || !job || !final) { job_release(job); return fail(error, error_size, "invalid texture job"); }
    const struct texture_upscale_generate_input *input = &job->input;
    struct texture_upscale_generate_output *output = &job->output;
    if (output->result == TEXTURE_POLICY_ORIGINAL && !output->levels) { *final = *output; free(job); return 0; }
    const struct texture_policy_table *policy = job->policy;
    struct texture_refine_params params = job->params;
    unsigned base = job->base, requested = job->requested;
    int is_bump = job->is_bump;
    struct image top = job->top, reference = job->reference, resized = {0}, raw_height = job->raw_height,
        high = job->high, mip = job->mip;
    struct bump_source bump = job->bump;
    memset(&job->top, 0, sizeof(job->top)); memset(&job->reference, 0, sizeof(job->reference));
    memset(&job->raw_height, 0, sizeof(job->raw_height)); memset(&job->high, 0, sizeof(job->high));
    memset(&job->mip, 0, sizeof(job->mip)); memset(&job->bump, 0, sizeof(job->bump));
    int result = -1;
    double started;
    trace(generator, "end");
    /* (a stop for gameplay ends the texture between stages; refinement also between rows) */
#define STOPPED() (texture_refine_cancelled() && (fail(error, error_size, "stopped for gameplay"), 1))
    if (STOPPED()) goto done;
    if (is_bump) {
        if (bump_top(generator, input, &bump, &raw_height, base, output->result == TEXTURE_POLICY_BPF, &top, &params,
                &output->seconds, error, error_size)) goto done;
    } else {
        /* Every base refinement starts from the stored S4g top. */
        quantize_in_place(&top, input->alpha);
        int check_damage = color_like(input);
        if (check_damage && measure(&top, &reference, base, input->wrap, input->alpha != TEXTURE_UPSCALE_ALPHA_NONE,
            &output->damage_share, &output->structure_loss_s4g, &output->seconds)) goto done;
        int damage_fired = check_damage && !input->pinned && output->result == TEXTURE_POLICY_S4G &&
            ((policy->damage_share >= 0 && (float)output->damage_share >= policy->damage_share) ||
             (policy->damage_structure_loss >= 0 && (float)output->structure_loss_s4g >= policy->damage_structure_loss));
        if (damage_fired) output->result = TEXTURE_POLICY_BPF;
        if (STOPPED()) goto done;
        if (output->result == TEXTURE_POLICY_BPF) {
            if (refine_color(&top, &reference, base, input->wrap, &params, &output->seconds)) goto done;
            if (STOPPED()) goto done;
            trace(generator, "refined");
            /* A 4x base's 2x mip is downsampled from the unquantized refinement, so take it before the top is
               quantized for damage.stats, which measures the stored bpf bytes. Quantizing in place changes no stored
               byte, and needs no second full-size image. */
            if (base == 4) {
                release(&mip);
                if (half_into(&top, input->wrap, &mip)) goto done;
            }
            if (check_damage) {
                quantize_in_place(&top, input->alpha);
                sample_footprint(generator);
                double ignored;
                int measured = measure(&top, &reference, base, input->wrap, input->alpha != TEXTURE_UPSCALE_ALPHA_NONE,
                    &ignored, &output->structure_loss_bpf, &output->seconds);
                if (measured) goto done;
                if (STOPPED()) goto done;
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
    if (STOPPED()) goto done;
    started = seconds();
    if (append_level(output, &top, input->alpha)) goto done;
    sample_footprint(generator);
    if (base == 4) {
        /* The 4x PNG is quantized for selection, but its 2x mip was filtered before that quantization: S4g's own
           2x, or the refinement's (above). */
        release(&top);
        top = mip;
        memset(&mip, 0, sizeof(mip));
        if (!top.pixels) goto done;
        if (append_level(output, &top, input->alpha)) goto done;
    }
    release(&top);
    output->seconds.mips += seconds() - started;
    trace(generator, "base");
    if (requested > base) {
        if (STOPPED()) goto done;
        if (is_bump) {
            if (bump_top(generator, input, &bump, &raw_height, requested, output->result == TEXTURE_POLICY_BPF, &top,
                &params, &output->seconds, error, error_size)) goto done;
        } else {
            /* The 8x set infers from its approved 2x mip. The 4x set uses
               S4g's quantized cached 4x image; lower approved mips stay intact. */
            if (requested == 8) {
                if (infer_color(generator, input, &output->levels[1], &top, 1, NULL, &output->seconds, error, error_size)) goto done;
            } else {
                top = high;
                memset(&high, 0, sizeof(high));
                quantize_in_place(&top, input->alpha);
            }
            trace(generator, "high");
            if (STOPPED()) goto done;
            if (output->result == TEXTURE_POLICY_BPF &&
                refine_color(&top, &reference, requested, input->wrap, &params, &output->seconds)) goto done;
            trace(generator, "high-refined");
        }
        if (STOPPED()) goto done;
        started = seconds();
        if (prepend_level(output, &top, input->alpha)) goto done;
        sample_footprint(generator);
        trace(generator, "prepended");
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
    release(&top); release(&reference); release(&resized); release(&raw_height); release(&high); release(&mip); free(bump.height);
    if (result) {
        texture_upscale_generate_free_output(output);
        if (!error || !error_size || !error[0]) fail(error, error_size, "texture generation failed");
    } else if (output->result == TEXTURE_POLICY_ORIGINAL) {
        for (unsigned i = 0; i < output->level_count; i++) free((void *)output->levels[i].pixels);
        free(output->levels); output->levels = NULL; output->level_count = 0;
    }
    if (!result) *final = *output;
    memset(output, 0, sizeof(*output));
    job_release(job);
    return result;
#undef STOPPED
}

int texture_upscale_generate(struct texture_upscale_generator *generator,
    const struct texture_upscale_generate_input *input, struct texture_upscale_generate_output *output,
    char *error, size_t error_size)
{
    struct texture_upscale_job *job = NULL;
    if (output) memset(output, 0, sizeof(*output));
    if (texture_upscale_generate_begin(generator, input, &job, error, error_size)) return -1;
    return texture_upscale_generate_end(generator, job, output, error, error_size);
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
    } else result = infer_color(generator, input, input->levels, &raw, 0, NULL, &times, error, error_size);
    if (result) { release(&raw); return fail(error, error_size, "raw prediction failed"); }
    *pixels = raw.pixels; *channels = raw.channels;
    return 0;
}
