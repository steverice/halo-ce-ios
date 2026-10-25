#include "texture_upscale_pixels.h"
#include "texture_upscale_pixel_fixtures.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void damage_case(int test, uint32_t w, uint32_t h, uint32_t scale, int wrap, int transparent)
{
    float *ref = malloc((size_t)w * h * 4 * sizeof(float));
    float *top = malloc((size_t)w * h * scale * scale * 4 * sizeof(float));
    double share, loss;
    assert(ref && top);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        size_t i = ((size_t)y * w + x) * 4;
        for (int c = 0; c < 3; c++) ref[i + c] = ((x * 37 + y * 19 + c * 53 + (x * y) % 17) % 256) / 255.0f;
        ref[i + 3] = test == 3 ? 0 : transparent ? ((x + y) % 3 != 0) : 1;
    }
    for (uint32_t y = 0; y < h * scale; y++) for (uint32_t x = 0; x < w * scale; x++) {
        size_t i = ((size_t)y * w * scale + x) * 4;
        uint32_t sx = x / scale, sy = y / scale;
        for (int c = 0; c < 3; c++) {
            int base = (sx * 37 + sy * 19 + c * 53 + (sx * sy) % 17) % 256;
            int value = base + (int)((x * 11 + y * 7 + c * 17) % 91) - 45;
            top[i + c] = (value < 0 ? 0 : value > 255 ? 255 : value) / 255.0f;
        }
        top[i + 3] = 1;
    }
    if (transparent) assert(texture_upscale_dilate(ref, w, h) == 0);
    if (test == 2) for (size_t i = 0; i < (size_t)w * h * 4; i++) assert(fabsf(ref[i] - pixel_expected_dilated[i]) < 1e-6f);
    assert(texture_upscale_damage(top, 4, ref, w, h, scale, wrap, transparent, &share, &loss) == 0);
    printf("damage %d: share %.9f expected %.9f, structure %.9f expected %.9f\n", test, share,
        pixel_damage_expected[test][0], loss, pixel_damage_expected[test][1]);
    assert(fabs(share - pixel_damage_expected[test][0]) < 0.005);
    assert(fabs(loss - pixel_damage_expected[test][1]) < 0.005);
    free(ref); free(top);
}

static void bump_case(uint32_t w, uint32_t h, const float *expected, const float *expected_normals)
{
    uint8_t *rgba = malloc((size_t)w * h * 4);
    float *height = malloc((size_t)w * h * sizeof(float)), *normals = malloc((size_t)w * h * 3 * sizeof(float));
    float low, span, worst = 0;
    double mean = 0;
    assert(rgba && height && normals);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        size_t i = ((size_t)y * w + x) * 4;
        rgba[i] = 80 + (x * 19 + y * 7) % 96;
        rgba[i + 1] = 77 + (x * 11 + y * 23) % 101;
        rgba[i + 2] = 205 + (x * 3 + y * 5) % 50;
        rgba[i + 3] = 255;
    }
    assert(texture_upscale_integrate_normals(rgba, w, h, height, &low, &span) == 0);
    texture_upscale_derive_normals(height, w, h, 4, normals);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        worst = fmaxf(worst, fabsf(height[i] - expected[i])); mean += height[i];
        assert(height[i] >= low - 1e-6f && height[i] <= low + span + 1e-6f);
    }
    printf("bump %u x %u: worst height error %.9g\n", w, h, worst);
    assert(worst < 2e-5f && fabs(mean / (w * h)) < 1e-6);
    for (size_t i = 0; i < (size_t)w * h * 3; i++) assert(fabsf(normals[i] - expected_normals[i]) < 1e-4f);
    free(rgba); free(height); free(normals);
}

static void edges(void)
{
    const float src[] = {0, 1, 2, 3};
    float padded[16], resized[16], down[4];
    float rgba[] = { 0.5f, 1.5f / 255, 2.5f / 255, 0.5f };
    uint8_t bytes[4];
    assert(texture_upscale_pad_region(src, 2, 2, 1, -1, -1, 4, 4, 1, padded) == 0);
    assert(padded[0] == 3 && padded[1] == 2 && padded[4] == 1);
    assert(texture_upscale_pad_region(src, 2, 2, 1, -1, -1, 4, 4, 0, padded) == 0);
    assert(padded[0] == 0 && padded[15] == 3);
    assert(texture_upscale_resize(src, 2, 2, 1, 2, 1, resized) == 0);
    assert(texture_upscale_down2(resized, 4, 4, 1, 1, down) == 0);
    for (int i = 0; i < 16; i++) assert(isfinite(resized[i]));
    texture_upscale_finish(rgba, 1, 1, TEXTURE_UPSCALE_ALPHA_BINARY, bytes);
    assert(bytes[0] == 128 && bytes[1] == 2 && bytes[2] == 2 && bytes[3] == 255);
    texture_upscale_finish(rgba, 1, 1, TEXTURE_UPSCALE_ALPHA_NONE, bytes);
    assert(bytes[3] == 255);
}

static void filters(void)
{
    float down[4 * 2 * 3], up[16 * 8 * 3];
    float scalar[4 * 2];
    for (int wrap = 0; wrap <= 1; wrap++) {
        const float *expected_down = wrap ? pixel_down_expected_wrap : pixel_down_expected_clamp;
        const float *expected_up = wrap ? pixel_up_expected_wrap : pixel_up_expected_clamp;
        assert(texture_upscale_down2(pixel_filter_input, 8, 4, 3, wrap, down) == 0);
        assert(texture_upscale_resize(pixel_filter_input, 8, 4, 3, 2, wrap, up) == 0);
        for (size_t i = 0; i < 4 * 2 * 3; i++) assert(fabsf(down[i] - expected_down[i]) < 1e-6f);
        for (size_t i = 0; i < 16 * 8 * 3; i++) assert(fabsf(up[i] - expected_up[i]) < 1e-6f);
        for (uint32_t channel = 0; channel < 3; channel++) {
            assert(texture_upscale_down2_channel(pixel_filter_input, 8, 4, 3, channel, wrap, scalar) == 0);
            for (size_t i = 0; i < 4 * 2; i++) assert(fabsf(scalar[i] - expected_down[i * 3 + channel]) < 1e-6f);
        }
    }
}

int main(void)
{
    damage_case(0, 16, 8, 2, 1, 0);
    damage_case(1, 8, 4, 4, 0, 0);
    damage_case(2, 8, 8, 8, 1, 1);
    damage_case(3, 4, 4, 2, 0, 1);
    bump_case(16, 8, pixel_height_expected_0, pixel_normal_expected_0);
    bump_case(7, 5, pixel_height_expected_1, pixel_normal_expected_1);
    bump_case(1, 7, pixel_height_expected_2, pixel_normal_expected_2);
    bump_case(1, 1, pixel_height_expected_3, pixel_normal_expected_3);
    edges();
    filters();
    puts("texture_upscale_pixels_probe: ok");
    return 0;
}
