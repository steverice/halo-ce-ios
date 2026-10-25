#include "texture_refine.h"
#include "texture_upscale_pixels.h"
#include "texture_upscale_pixel_fixtures.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static void check(const float *actual, const float *expected, size_t count)
{
    double worst = 0;
    for (size_t i = 0; i < count; i++) worst = fmax(worst, fabs(actual[i] - expected[i]));
    if (worst >= 1e-5) fprintf(stderr, "worst refinement error %.9g\n", worst);
    assert(worst < 1e-5);
}

static void case_check(uint32_t w, uint32_t h, uint32_t c, uint32_t scale, int wrap, float sigma, uint32_t iterations,
    const float *bp, const float *bpf, const float *expected_mask)
{
    size_t count = (size_t)w * h, large = count * scale * scale * c;
    float *ref = malloc(count * c * sizeof(float)), *top = malloc(large * sizeof(float)), *mask = malloc(count * sizeof(float));
    struct texture_refine_params params = { 12, 30, sigma, iterations };
    assert(ref && top && mask);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) for (uint32_t ch = 0; ch < c; ch++)
        ref[((size_t)y * w + x) * c + ch] = ((x * 13 + y * 29 + ch * 37 + 63) % 128) / 255.0f;
    for (int fallback = 0; fallback <= 1; fallback++) {
        for (uint32_t y = 0; y < h * scale; y++) for (uint32_t x = 0; x < w * scale; x++) for (uint32_t ch = 0; ch < c; ch++)
            top[((size_t)y * w * scale + x) * c + ch] = ((x * 11 + y * 7 + ch * 29 + 40) % 384) / 255.0f;
        assert(texture_refine_color_bounded(top, ref, w, h, c, scale, wrap, &params, fallback, mask) == 0);
        check(top, fallback ? bpf : bp, large); check(mask, expected_mask, count);
    }
    free(ref); free(top); free(mask);
}

static void bump_check(uint32_t w, uint32_t h, uint32_t scale, const float *reference, const float *input,
    const float *bp, const float *bpf, const float *expected_mask)
{
    size_t count = (size_t)w * h, large = count * scale * scale;
    float *top = malloc(large * sizeof(float)), *normals = malloc(large * 3 * sizeof(float)), *mask = malloc(count * sizeof(float));
    struct texture_refine_params params = { 12, 30, 1, 3 };
    assert(top && normals && mask);
    for (int fallback = 0; fallback <= 1; fallback++) {
        memcpy(top, input, large * sizeof(float));
        assert(texture_refine_bump_bounded(top, reference, w, h, scale, &params, fallback, normals, mask) == 0);
        check(normals, fallback ? bpf : bp, large * 3); check(mask, expected_mask, count);
    }
    free(top); free(normals); free(mask);
}

static void large_image(void)
{
    enum { W = 512, H = 512, S = 8, C = 4 };
    size_t count = (size_t)W * H, large = count * S * S * C;
    float *top = malloc(large * sizeof(float)), *reference = malloc(count * C * sizeof(float));
    struct texture_refine_params params = {12, 30, 1, 3};
    struct rusage usage;
    double share, loss;
    assert(top && reference);
    for (size_t i = 0; i < large; i++) top[i] = 0.6f;
    for (size_t i = 0; i < count * C; i++) reference[i] = 0.5f;
    assert(texture_refine_color_bounded(top, reference, W, H, C, S, 1, &params, 1, NULL) == 0);
    for (size_t i = 0; i < large; i++) assert(fabsf(top[i] - 0.5f) < 1e-5f);
    assert(texture_upscale_damage(top, C, reference, W, H, S, 1, 1, &share, &loss) == 0);
    /* Gaussian float32 rounding can leave tiny high-pass energy even in a constant field; structure_loss then
       compares those rounding residuals. Its parity is checked on nonconstant NumPy fixtures above. */
    assert(share == 0 && isfinite(loss));
    assert(getrusage(RUSAGE_SELF, &usage) == 0);
    printf("texture_refine_bounded_probe: 4096 x 4096 RGBA, peak RSS %.1f MiB\n", usage.ru_maxrss / (1024.0 * 1024));
    free(top); free(reference);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--large") == 0) { large_image(); return 0; }
    float top[32 * 16], mask[8];
    struct texture_refine_params params = { 12, 30, 1, 3 };
    for (int fallback = 0; fallback <= 1; fallback++) {
        memcpy(top, pixel_refine8_top, sizeof(top));
        assert(texture_refine_color_bounded(top, pixel_refine8_reference, 4, 2, 1, 8, 1, &params, fallback, mask) == 0);
        check(top, fallback ? pixel_refine8_bpf : pixel_refine8_bp, 32 * 16);
        check(mask, pixel_refine8_mask, 8);
    }
    case_check(4, 2, 4, 2, 0, 0, 1, bounded_bp_0, bounded_bpf_0, bounded_mask_0);
    case_check(4, 2, 4, 4, 1, 0.6f, 3, bounded_bp_1, bounded_bpf_1, bounded_mask_1);
    case_check(1, 4, 3, 8, 1, 1.4f, 3, bounded_bp_2, bounded_bpf_2, bounded_mask_2);
    case_check(4, 1, 4, 4, 0, 1, 0, bounded_bp_3, bounded_bpf_3, bounded_mask_3);
    bump_check(4, 4, 4, bounded_bump_reference_0, bounded_bump_top_0, bounded_bump_bp_0, bounded_bump_bpf_0, bounded_bump_mask_0);
    bump_check(1, 4, 2, bounded_bump_reference_1, bounded_bump_top_1, bounded_bump_bp_1, bounded_bump_bpf_1, bounded_bump_mask_1);
    bump_check(2, 1, 8, bounded_bump_reference_2, bounded_bump_top_2, bounded_bump_bp_2, bounded_bump_bpf_2, bounded_bump_mask_2);
    assert(texture_refine_color_bounded(top, top, 4, 2, 1, 3, 1, &params, 0, NULL) == -1);
    puts("texture_refine_bounded_probe: ok");
    return 0;
}
