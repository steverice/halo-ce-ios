/* A texture of several tiles gets its 2x level from each tile's own 4x context (texture_upscale_generate.m,
   write_x2). With a translation-equivariant stand-in for S4g, that must equal down2 of the whole 4x image exactly,
   wrapped and clamped, and a 4x top kept as half must equal the float prediction rounded once. */
#include "texture_upscale_generate.h"
#include "texture_refine.h"
#include "texture_upscale_big.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct texture_upscale_model { int unused; };
const struct texture_policy_table texture_policy_embedded = { .low = 12, .high = 30, .sigma = 1, .iterations = 3,
    .damage_share = -1, .damage_structure_loss = -1, .damage_bpf_structure_loss = -1 };
int texture_upscale_model_available(void) { return 1; }
struct texture_upscale_model *texture_upscale_model_create(char *e, size_t n) { (void)e; (void)n; return calloc(1, sizeof(struct texture_upscale_model)); }
void texture_upscale_model_destroy(struct texture_upscale_model *m) { free(m); }
void texture_upscale_model_get_stats(struct texture_upscale_model *m, struct texture_upscale_model_stats *s) { (void)m; memset(s, 0, sizeof(*s)); }
static long addressed(long i, long size, int wrap) { return wrap ? (i % size + size) % size : i < 0 ? 0 : i >= size ? size - 1 : i; }
/* 4x output pixel (x, y): a smooth function of the source texels around it, as a model would see them. */
static float fake(const float *s, uint32_t w, uint32_t h, uint32_t c, uint32_t i, int wrap, long x, long y)
{
    long sx = (x >= 0 ? x : x - 3) / 4, sy = (y >= 0 ? y : y - 3) / 4;
    float a = s[(addressed(sy, h, wrap) * w + addressed(sx, w, wrap)) * c + i];
    float b = s[(addressed(sy, h, wrap) * w + addressed(sx + 1, w, wrap)) * c + i];
    float t = (float)((x % 4 + 4) % 4) / 4;
    return a * (1 - t) + b * t;
}
int texture_upscale_model_predict_tiles(struct texture_upscale_model *m, const float *s, uint32_t w, uint32_t h,
    uint32_t c, int wrap, texture_upscale_tile_consumer consumer, void *context, char *e, size_t n)
{
    (void)m; (void)e; (void)n;
    for (uint32_t ty = 0; ty < h; ty += 256) for (uint32_t tx = 0; tx < w; tx += 256) {
        uint32_t cw = w - tx < 256 ? w - tx : 256, ch = h - ty < 256 ? h - ty : 256;
        size_t sx = cw * 4 + 128, sy = ch * 4 + 128;
        float *tile = malloc(sx * sy * sizeof(float));
        assert(tile);
        for (uint32_t i = 0; i < c; i++) {
            /* Context comes from wrapped or clamped texels, as pack_tile gives the model. */
            for (size_t y = 0; y < sy; y++) for (size_t x = 0; x < sx; x++) {
                long gx = (long)tx * 4 + (long)x - 64, gy = (long)ty * 4 + (long)y - 64;
                tile[y * sx + x] = fake(s, w, h, c, i, wrap, gx, gy);
            }
            struct texture_upscale_tile view = { tx, ty, cw, ch, tile + 64 * sx + 64, (ptrdiff_t)sx, 1, 0 };
            if (consumer(context, &view, i)) { free(tile); return -1; }
        }
        free(tile);
    }
    return 0;
}
int texture_upscale_model_predict4x(struct texture_upscale_model *m, const float *s, uint32_t w, uint32_t h, uint32_t c,
    int wrap, float *d, char *e, size_t n)
{
    (void)m; (void)e; (void)n;
    for (uint32_t y = 0; y < 4 * h; y++) for (uint32_t x = 0; x < 4 * w; x++) for (uint32_t i = 0; i < c; i++)
        d[((size_t)y * w * 4 + x) * c + i] = fake(s, w, h, c, i, wrap, x, y);
    return 0;
}
int texture_upscale_model_predict4x_strided(struct texture_upscale_model *m, const float *s, uint32_t w, uint32_t h,
    uint32_t c, int wrap, float *d, uint32_t stride, char *e, size_t n)
{
    (void)m; (void)e; (void)n;
    for (uint32_t y = 0; y < 4 * h; y++) for (uint32_t x = 0; x < 4 * w; x++) for (uint32_t i = 0; i < c; i++)
        d[((size_t)y * w * 4 + x) * stride + i] = fake(s, w, h, c, i, wrap, x, y);
    return 0;
}

int main(void)
{
    uint32_t w = 600, h = 300;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    float *unit = malloc((size_t)w * h * 3 * sizeof(float));
    assert(rgba && unit);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        for (int c = 0; c < 3; c++) rgba[i * 4 + c] = (uint8_t)((i * (c + 3) * 2654435761u) >> 24);
        rgba[i * 4 + 3] = 255;
        for (int c = 0; c < 3; c++) unit[i * 3 + c] = rgba[i * 4 + c] / 255.0f;
    }
    char error[256];
    struct texture_upscale_generator *g = texture_upscale_generate_create(error, sizeof(error));
    assert(g);
    for (int wrap = 0; wrap < 2; wrap++) {
        struct texture_upscale_rgba_level level = { rgba, w, h };
        struct texture_upscale_generate_input in = { .decision = { .width = w, .height = h, .kind = TEXTURE_POLICY_SURFACE,
            .result = TEXTURE_POLICY_S4G, .treatment = TEXTURE_POLICY_MULTIPURPOSE, .scale = 4 },
            .levels = &level, .level_count = 1, .alpha = TEXTURE_UPSCALE_ALPHA_NONE, .wrap = wrap };
        struct texture_upscale_generate_output out = { 0 };
        assert(!texture_upscale_generate(g, &in, &out, error, sizeof(error)));
        assert(out.level_count == 3 && out.levels[0].width == 4 * w && out.levels[1].width == 2 * w);
        float *x4 = malloc((size_t)w * h * 16 * 3 * sizeof(float)), *x2 = malloc((size_t)w * h * 4 * 3 * sizeof(float));
        assert(x4 && x2);
        texture_upscale_model_predict4x(NULL, unit, w, h, 3, wrap, x4, NULL, 0);
        assert(!texture_upscale_down2(x4, w * 4, h * 4, 3, wrap, x2));
        size_t wrong2 = 0, wrong4 = 0;
        for (size_t i = 0; i < (size_t)w * h * 4; i++) for (int c = 0; c < 3; c++)
            wrong2 += out.levels[1].pixels[i * 4 + c] != (uint8_t)lrintf(fminf(1, fmaxf(0, x2[i * 3 + c])) * 255);
        for (size_t i = 0; i < (size_t)w * h * 16; i++) for (int c = 0; c < 3; c++)
            wrong4 += out.levels[0].pixels[i * 4 + c] !=
                (uint8_t)lrintf(fminf(1, fmaxf(0, (float)(_Float16)x4[i * 3 + c])) * 255);
        printf("tiled 2x and half 4x, %s: %zu and %zu texel channels differ\n", wrap ? "wrapped" : "clamped", wrong2, wrong4);
        assert(!wrong2 && !wrong4);
        free(x4); free(x2);
        texture_upscale_generate_free_output(&out);
    }
    texture_upscale_generate_destroy(g);
    free(rgba); free(unit);
    /* the stop for gameplay ends a refinement between rows, and only while it is set */
    {
        float reference[8 * 8 * 3], top[16 * 16 * 3];
        struct texture_refine_params params = { 12, 30, 1, 3 };
        volatile int stop = 1;
        for (size_t i = 0; i < sizeof(reference) / sizeof(*reference); i++) reference[i] = (float)(i % 7) / 7;
        for (size_t i = 0; i < sizeof(top) / sizeof(*top); i++) top[i] = (float)(i % 5) / 5;
        texture_refine_set_cancel(&stop);
        assert(texture_refine_cancelled());
        assert(texture_refine_color_bounded(top, reference, 8, 8, 3, 2, 1, &params, 1, NULL) != 0);
        stop = 0;
        assert(texture_refine_color_bounded(top, reference, 8, 8, 3, 2, 1, &params, 1, NULL) == 0);
        texture_refine_set_cancel(NULL);
    }
    /* the big allocator: a freed mapping is reused for the same size, calloc stays zero after reuse, trim empties it */
    {
        unsigned char *a = texture_upscale_big_alloc(3u << 20);
        assert(a); memset(a, 7, 3u << 20); texture_upscale_big_free(a);
        assert(texture_upscale_big_cache_state.bytes > 0);
        unsigned char *b = texture_upscale_big_calloc(3u << 20, 1);
        assert(b == a && b[0] == 0 && b[(3u << 20) - 1] == 0);
        unsigned char *c = texture_upscale_big_shrink(b, 1u << 20);
        assert(c == b); texture_upscale_big_free(c);
        texture_upscale_big_trim();
        assert(texture_upscale_big_cache_state.bytes == 0);
        void *small = texture_upscale_big_alloc(100); assert(small); texture_upscale_big_free(small);
    }
    puts("texture_upscale_tiles_probe: ok");
    return 0;
}
