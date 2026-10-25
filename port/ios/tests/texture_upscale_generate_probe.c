/* Isolate orchestration from learned weights and Metal; pixel accuracy has its own probe. */
#include "texture_refine.h"
#include "texture_upscale_generate.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct texture_upscale_model {
    int unused;
};
struct texture_refine_context {
    int unused;
};
static unsigned calls, max_width;
static int prediction_failure;
static float first_refine, second_pass_input;
static double damage_share;
const struct texture_policy_table texture_policy_embedded = { .low = 12,
    .high = 30,
    .sigma = 1,
    .iterations = 3,
    .damage_share = -1,
    .damage_structure_loss = -1,
    .damage_bpf_structure_loss = -1 };
int texture_upscale_model_available(void) { return 1; }
struct texture_upscale_model* texture_upscale_model_create(char* e, size_t n)
{
    return calloc(1, sizeof(struct texture_upscale_model));
}
void texture_upscale_model_destroy(struct texture_upscale_model* m) { free(m); }
void texture_upscale_model_get_stats(struct texture_upscale_model* m, struct texture_upscale_model_stats* s)
{
    memset(s, 0, sizeof(*s));
}
int texture_upscale_model_predict4x(struct texture_upscale_model* m, const float* s, uint32_t w, uint32_t h, uint32_t c,
    int wrap, float* d, char* e, size_t n)
{
    calls++;
    if (prediction_failure)
        return -1;
    if (w == 8)
        second_pass_input = s[0];
    if (w > max_width)
        max_width = w;
    for (unsigned y = 0; y < 4 * h; y++)
        for (unsigned x = 0; x < 4 * w; x++)
            for (unsigned i = 0; i < c; i++)
                d[((size_t)y * w * 4 + x) * c + i] = s[((size_t)(y / 4) * w + x / 4) * c + i] * .91f + .013f;
    return 0;
}
int texture_upscale_model_predict4x_strided(struct texture_upscale_model* m, const float* source, uint32_t width,
    uint32_t height, uint32_t channels, int wrap, float* dest, uint32_t stride, char* error, size_t error_size)
{
    float* packed = malloc((size_t)width * height * 16 * channels * sizeof(float));
    assert(packed);
    int result = texture_upscale_model_predict4x(m, source, width, height, channels, wrap, packed, error, error_size);
    if (!result)
        for (size_t i = 0; i < (size_t)width * height * 16; i++)
            memcpy(dest + i * stride, packed + i * channels, channels * sizeof(float));
    free(packed);
    return result;
}
struct texture_refine_context* texture_refine_create(void* d, char* e, size_t n)
{
    return calloc(1, sizeof(struct texture_refine_context));
}
void texture_refine_destroy(struct texture_refine_context* r) { free(r); }
int texture_refine_color_bounded(float* t, const float* o, uint32_t w, uint32_t h, uint32_t c, uint32_t z, int wrap,
    const struct texture_refine_params* p, int f, float* mask)
{
    first_refine = t[0];
    for (size_t i = 0; i < (size_t)w * h * z * z * c; i++)
        t[i] += .1f;
    return 0;
}
int texture_refine_bump_bounded(float* t, const float* o, uint32_t w, uint32_t h,
    uint32_t z, const struct texture_refine_params* p, int f, float* n, float* mask)
{
    for (size_t i = 0; i < (size_t)w * h * z * z; i++) {
        n[3 * i] = n[3 * i + 1] = 0;
        n[3 * i + 2] = 1;
    }
    return 0;
}
int texture_upscale_dilate(float* r, uint32_t w, uint32_t h) { return 0; }
int texture_upscale_down2(const float* s, uint32_t w, uint32_t h, uint32_t c, int wrap, float* d)
{
    unsigned ow = w > 1 ? w / 2 : 1, oh = h > 1 ? h / 2 : 1;
    for (unsigned y = 0; y < oh; y++)
        for (unsigned x = 0; x < ow; x++)
            for (unsigned i = 0; i < c; i++)
                d[((size_t)y * ow + x) * c + i] = s[((size_t)(2 * y) * w + 2 * x) * c + i];
    return 0;
}
int texture_upscale_resize(const float* s, uint32_t w, uint32_t h, uint32_t c, uint32_t z, int wrap, float* d)
{
    for (unsigned y = 0; y < h * z; y++)
        for (unsigned x = 0; x < w * z; x++)
            for (unsigned i = 0; i < c; i++)
                d[((size_t)y * w * z + x) * c + i] = s[((size_t)(y / z) * w + x / z) * c + i];
    return 0;
}
void texture_upscale_finish(const float* s, uint32_t w, uint32_t h, enum texture_upscale_alpha a, uint8_t* d)
{
    for (size_t i = 0; i < (size_t)w * h * 4; i++)
        d[i] = (uint8_t)lrintf(fminf(1, fmaxf(0, s[i])) * 255);
    for (size_t i = 0; i < (size_t)w * h; i++)
        if (a == TEXTURE_UPSCALE_ALPHA_NONE)
            d[4 * i + 3] = 255;
        else if (a == TEXTURE_UPSCALE_ALPHA_BINARY)
            d[4 * i + 3] = s[4 * i + 3] >= .5f ? 255 : 0;
}
int texture_upscale_damage(const float* t, uint32_t c, const float* r, uint32_t w, uint32_t h, uint32_t z, int wrap,
    int a, double* s, double* l)
{
    *s = damage_share;
    *l = 0;
    return 0;
}
int texture_upscale_integrate_normals(const uint8_t* r, uint32_t w, uint32_t h, float* o, float* l, float* s)
{
    memset(o, 0, (size_t)w * h * sizeof(float));
    *l = 0;
    *s = 1;
    return 0;
}
void texture_upscale_derive_normals(const float* h, uint32_t w, uint32_t hh, float f, float* n)
{
    for (size_t i = 0; i < (size_t)w * hh; i++) {
        n[3 * i] = n[3 * i + 1] = 0;
        n[3 * i + 2] = 1;
    }
}
int main(void)
{
    uint8_t a[64], b[16], c[4];
    memset(a, 100, sizeof(a));
    memset(b, 77, sizeof(b));
    memset(c, 55, sizeof(c));
    for (unsigned i = 0; i < 16; i++)
        a[4 * i + 3] = 255;
    struct texture_upscale_rgba_level levels[] = { { a, 4, 4 }, { b, 2, 2 }, { c, 1, 1 } };
    struct texture_upscale_generate_input in = { .decision = { .width = 4,
                                                     .height = 4,
                                                     .kind = TEXTURE_POLICY_GRAPHIC,
                                                     .result = TEXTURE_POLICY_S4G,
                                                     .treatment = TEXTURE_POLICY_COLOR,
                                                     .scale = 2 },
        .levels = levels,
        .level_count = 3,
        .alpha = TEXTURE_UPSCALE_ALPHA_NONE };
    struct texture_upscale_generate_output out = { 0 };
    char error[256];
    struct texture_upscale_generator* g = texture_upscale_generate_create(error, sizeof(error));
    assert(g);
    in.decision.result = TEXTURE_POLICY_ORIGINAL;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(calls == 0 && out.level_count == 0);
    texture_upscale_generate_free_output(&out);
    in.decision.result = TEXTURE_POLICY_S4G;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.level_count == 4 && out.levels[0].width == 8 && !memcmp(out.levels[1].pixels, a, 64));
    texture_upscale_generate_free_output(&out);
    in.decision.scale = 4;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.level_count == 5 && out.levels[0].width == 16 && out.levels[1].width == 8
        && !memcmp(out.levels[2].pixels, a, 64));
    texture_upscale_generate_free_output(&out);
    in.decision.treatment = TEXTURE_POLICY_FIRST_PERSON;
    in.decision.scale = 8;
    max_width = 0;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.level_count == 6 && out.levels[0].width == 32 && out.levels[1].width == 16 && out.levels[2].width == 8
        && max_width == 8 && fabsf(second_pass_input - 94 / 255.0f) < 1e-6f && !memcmp(out.levels[3].pixels, a, 64));
    texture_upscale_generate_free_output(&out);
    in.decision.result = TEXTURE_POLICY_BPF;
    in.decision.scale = 4;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.result == TEXTURE_POLICY_BPF && out.levels[0].pixels[0] == 120
        && fabsf(first_refine - 94 / 255.0f) < 1e-6f);
    texture_upscale_generate_free_output(&out);
    in.decision.treatment = TEXTURE_POLICY_COLOR;
    in.decision.scale = 2;
    in.decision.result = TEXTURE_POLICY_S4G;
    struct texture_policy_table policy = texture_policy_embedded;
    policy.damage_share = .1f;
    in.policy = &policy;
    damage_share = .1;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.result == TEXTURE_POLICY_BPF);
    texture_upscale_generate_free_output(&out);
    in.pinned = 1;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.result == TEXTURE_POLICY_S4G);
    texture_upscale_generate_free_output(&out);
    in.pinned = 0;
    damage_share = 0;
    in.level_count = 1;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.level_count == 2 && out.levels[1].width == 4 && !memcmp(out.levels[1].pixels, a, 64));
    texture_upscale_generate_free_output(&out);
    float* raw = NULL;
    unsigned channels = 0;
    assert(!texture_upscale_generate_raw4x(g, &in, &raw, &channels, error, sizeof(error)));
    assert(raw && channels == 3);
    free(raw);
    prediction_failure = 1;
    assert(texture_upscale_generate(g, &in, &out, error, sizeof(error)) == -1);
    assert(out.levels == NULL && out.level_count == 0 && error[0]);
    prediction_failure = 0;
    in.decision.treatment = TEXTURE_POLICY_BUMP_MAP;
    in.decision.result = TEXTURE_POLICY_BPF;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    assert(out.levels[0].pixels[0] == 128 && out.levels[0].pixels[1] == 128 && out.levels[0].pixels[2] == 255);
    texture_upscale_generate_free_output(&out);
    in.alpha = TEXTURE_UPSCALE_ALPHA_BINARY;
    a[3] = 0;
    assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
    for (size_t i = 0; i < (size_t)out.levels[0].width * out.levels[0].height; i++) {
        assert(out.levels[0].pixels[4 * i] == 128);
        assert(out.levels[0].pixels[4 * i + 1] == 128);
        assert(out.levels[0].pixels[4 * i + 2] == 255);
    }
    assert(out.levels[0].pixels[3] == 0 && out.levels[0].pixels[2 * 4 + 3] == 255);
    texture_upscale_generate_free_output(&out);
    texture_upscale_generate_destroy(g);
    puts("generator orchestration: PASS");
    return 0;
}
