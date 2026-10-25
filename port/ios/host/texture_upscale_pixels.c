#include "texture_upscale_pixels.h"
#include <Accelerate/Accelerate.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int dimensions(uint32_t w, uint32_t h, uint32_t c)
{
    return w && h && c && c <= 4 && (size_t)w <= SIZE_MAX / h / c / sizeof(float);
}

static float *image(uint32_t w, uint32_t h, uint32_t c)
{
    return dimensions(w, h, c) ? malloc((size_t)w * h * c * sizeof(float)) : NULL;
}

static uint32_t address(int64_t i, uint32_t size, int wrap)
{
    if (wrap) return (uint32_t)((i % size + size) % size);
    return i < 0 ? 0 : i >= size ? size - 1 : (uint32_t)i;
}

static double sinc(double x) { return x == 0 ? 1 : sin(M_PI * x) / (M_PI * x); }
static double lanczos(double x) { return fabs(x) < 3 ? sinc(x) * sinc(x / 3) : 0; }

/* NumPy uses double weights in downsample2, then casts the result after both axes. */
int texture_upscale_down2(const float *src, uint32_t w, uint32_t h, uint32_t c, int wrap, float *dst)
{
    uint32_t ow = w > 1 ? w / 2 : 1, oh = h > 1 ? h / 2 : 1;
    double weights[12], sum = 0;
    double *rows;
    if (!src || !dst || !dimensions(w, h, c)) return -1;
    rows = malloc((size_t)w * c * sizeof(double));
    if (!rows) return -1;
    for (int k = 0; k < 12; k++) sum += weights[k] = lanczos((k - 5 - 0.5) / 2);
    for (int k = 0; k < 12; k++) weights[k] /= sum;
    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < w; x++)
            for (uint32_t ch = 0; ch < c; ch++) {
                double value = 0;
                if (h == 1) value = src[x * c + ch];
                else for (int k = 0; k < 12; k++)
                    value += weights[k] * src[((size_t)address((int64_t)y * 2 + k - 5, h, wrap) * w + x) * c + ch];
                rows[(size_t)x * c + ch] = value;
            }
        for (uint32_t x = 0; x < ow; x++)
            for (uint32_t ch = 0; ch < c; ch++) {
                double value = 0;
                if (w == 1) value = rows[ch];
                else for (int k = 0; k < 12; k++)
                    value += weights[k] * rows[(size_t)address((int64_t)x * 2 + k - 5, w, wrap) * c + ch];
                dst[((size_t)y * ow + x) * c + ch] = (float)value;
            }
    }
    free(rows);
    return 0;
}

int texture_upscale_resize(const float *src, uint32_t w, uint32_t h, uint32_t c, uint32_t scale, int wrap, float *dst)
{
    float *rows;
    double *weights;
    uint32_t ow, oh;
    if (!src || !dst || !dimensions(w, h, c) || !scale || w > UINT32_MAX / scale || h > UINT32_MAX / scale) return -1;
    ow = w * scale; oh = h * scale;
    if (!dimensions(ow, oh, c)) return -1;
    rows = image(w, oh, c);
    weights = malloc((size_t)scale * 7 * sizeof(double));
    if (!rows || !weights) { free(rows); free(weights); return -1; }
    for (uint32_t p = 0; p < scale; p++) {
        double sum = 0, frac = (p + 0.5) / scale - 0.5;
        for (int k = 0; k < 7; k++) sum += weights[p * 7 + k] = lanczos(k - 3 - frac);
        for (int k = 0; k < 7; k++) weights[p * 7 + k] = (float)(weights[p * 7 + k] / sum);
    }
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < w; x++)
            for (uint32_t ch = 0; ch < c; ch++) {
                float value = 0;
                for (int k = 0; k < 7; k++) value += (float)weights[(y % scale) * 7 + k] *
                    src[((size_t)address(y / scale + k - 3LL, h, wrap) * w + x) * c + ch];
                rows[((size_t)y * w + x) * c + ch] = value;
            }
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++)
            for (uint32_t ch = 0; ch < c; ch++) {
                float value = 0;
                for (int k = 0; k < 7; k++) value += (float)weights[(x % scale) * 7 + k] *
                    rows[((size_t)y * w + address(x / scale + k - 3LL, w, wrap)) * c + ch];
                dst[((size_t)y * ow + x) * c + ch] = value;
            }
    free(rows); free(weights);
    return 0;
}

int texture_upscale_down2_channel(const float *src, uint32_t w, uint32_t h, uint32_t stride, uint32_t channel, int wrap, float *dst)
{
    uint32_t ow = w > 1 ? w / 2 : 1, oh = h > 1 ? h / 2 : 1;
    double weights[12], sum = 0, *row;
    if (!src || !dst || !dimensions(w, h, stride) || channel >= stride) return -1;
    row = malloc((size_t)w * sizeof(double));
    if (!row) return -1;
    for (int k = 0; k < 12; k++) sum += weights[k] = lanczos((k - 5 - 0.5) / 2);
    for (int k = 0; k < 12; k++) weights[k] /= sum;
    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < w; x++) {
            double value = 0;
            if (h == 1) value = src[(size_t)x * stride + channel];
            else for (int k = 0; k < 12; k++) value += weights[k] *
                src[((size_t)address((int64_t)y * 2 + k - 5, h, wrap) * w + x) * stride + channel];
            row[x] = value;
        }
        for (uint32_t x = 0; x < ow; x++) {
            double value = 0;
            if (w == 1) value = row[0];
            else for (int k = 0; k < 12; k++) value += weights[k] * row[address((int64_t)x * 2 + k - 5, w, wrap)];
            dst[(size_t)y * ow + x] = (float)value;
        }
    }
    free(row);
    return 0;
}

int texture_upscale_pad_region(const float *src, uint32_t w, uint32_t h, uint32_t c, int32_t x, int32_t y,
    uint32_t ow, uint32_t oh, int wrap, float *dst)
{
    if (!src || !dst || !dimensions(w, h, c) || !dimensions(ow, oh, c)) return -1;
    for (uint32_t j = 0; j < oh; j++)
        for (uint32_t i = 0; i < ow; i++)
            memcpy(dst + ((size_t)j * ow + i) * c,
                src + ((size_t)address((int64_t)y + j, h, wrap) * w + address((int64_t)x + i, w, wrap)) * c,
                c * sizeof(float));
    return 0;
}

int texture_upscale_dilate(float *rgba, uint32_t w, uint32_t h)
{
    float *pyramid[33] = {0};
    uint32_t widths[33], heights[33], levels = 1;
    size_t visible = 0, count = (size_t)w * h;
    if (!rgba || !dimensions(w, h, 4) || (w & (w - 1)) || (h & (h - 1))) return -1;
    for (size_t i = 0; i < count; i++) visible += rgba[i * 4 + 3] > 0;
    if (!visible || visible == count) return 0;
    widths[0] = w; heights[0] = h; pyramid[0] = image(w, h, 4);
    if (!pyramid[0]) return -1;
    for (size_t i = 0; i < count; i++) {
        float weight = rgba[i * 4 + 3] > 0;
        for (int c = 0; c < 3; c++) pyramid[0][i * 4 + c] = rgba[i * 4 + c] * weight;
        pyramid[0][i * 4 + 3] = weight;
    }
    while (w > 1 || h > 1) {
        uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        float *next = image(nw, nh, 4), *prev = pyramid[levels - 1];
        if (!next) goto fail;
        pyramid[levels] = next; widths[levels] = nw; heights[levels] = nh; levels++;
        for (uint32_t y = 0; y < nh; y++) for (uint32_t x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
            /* _halve_sum adds rows first, then columns, in float32. */
            float a = prev[((size_t)(h > 1 ? y * 2 : y) * w + (w > 1 ? x * 2 : x)) * 4 + c];
            float b = w > 1 ? prev[((size_t)(h > 1 ? y * 2 : y) * w + x * 2 + 1) * 4 + c] : 0;
            if (h > 1) {
                a += prev[((size_t)(y * 2 + 1) * w + (w > 1 ? x * 2 : x)) * 4 + c];
                if (w > 1) b += prev[((size_t)(y * 2 + 1) * w + x * 2 + 1) * 4 + c];
            }
            next[((size_t)y * nw + x) * 4 + c] = a + b;
        }
        w = nw; h = nh;
    }
    for (int level = (int)levels - 1; level >= 0; level--) {
        float *p = pyramid[level];
        w = widths[level]; h = heights[level];
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)y * w + x) * 4;
            if (p[i + 3] > 0) for (int c = 0; c < 3; c++) p[i + c] /= p[i + 3];
            else if (level + 1 < (int)levels) {
                uint32_t pw = widths[level + 1], ph = heights[level + 1];
                size_t parent = ((size_t)(y / (h / ph)) * pw + x / (w / pw)) * 4;
                memcpy(p + i, pyramid[level + 1] + parent, 3 * sizeof(float));
            }
            if (level == 0 && rgba[i + 3] == 0) memcpy(rgba + i, p + i, 3 * sizeof(float));
        }
    }
    for (uint32_t i = 0; i < levels; i++) free(pyramid[i]);
    return 0;
fail:
    for (uint32_t i = 0; i < levels; i++) free(pyramid[i]);
    return -1;
}

static float unit(float x) { return fminf(1, fmaxf(0, x)); }

void texture_upscale_finish(const float *rgba, uint32_t w, uint32_t h, enum texture_upscale_alpha alpha, uint8_t *dst)
{
    for (size_t i = 0; i < (size_t)w * h; i++) for (int c = 0; c < 4; c++) {
        float v = unit(rgba[i * 4 + c]);
        if (c == 3 && alpha == TEXTURE_UPSCALE_ALPHA_NONE) v = 1;
        if (c == 3 && alpha == TEXTURE_UPSCALE_ALPHA_BINARY) v = v >= 0.5f;
        /* nearbyintf follows NumPy rint's ties-to-even with the default floating-point environment. */
        dst[i * 4 + c] = (uint8_t)nearbyintf(v * 255);
    }
}

static int blur(const float *src, uint32_t w, uint32_t h, uint32_t c, int wrap, float *dst)
{
    float *rows = image(w, h, c), weights[7];
    double total = 0;
    if (!rows) return -1;
    for (int k = 0; k < 7; k++) total += exp(-0.5 * (k - 3) * (k - 3));
    for (int k = 0; k < 7; k++) weights[k] = (float)(exp(-0.5 * (k - 3) * (k - 3)) / total);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) for (uint32_t ch = 0; ch < c; ch++) {
        float v = 0;
        for (int k = 0; k < 7; k++) v += weights[k] * src[((size_t)address(y + k - 3LL, h, wrap) * w + x) * c + ch];
        rows[((size_t)y * w + x) * c + ch] = v;
    }
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) for (uint32_t ch = 0; ch < c; ch++) {
        float v = 0;
        for (int k = 0; k < 7; k++) v += weights[k] * rows[((size_t)y * w + address(x + k - 3LL, w, wrap)) * c + ch];
        dst[((size_t)y * w + x) * c + ch] = v;
    }
    free(rows);
    return 0;
}

int texture_upscale_damage(const float *top, uint32_t tc, const float *ref, uint32_t w, uint32_t h, uint32_t scale,
    int wrap, int alpha_visible, double *share, double *loss)
{
    float *rgb = NULL, *wanted = NULL, *smoothed = NULL, *difference = NULL;
    size_t count = (size_t)w * h, visible = 0, damaged = 0;
    double numerator = 0, denominator = 0;
    int result = -1;
    if (!top || !ref || !share || !loss || (tc != 3 && tc != 4) || !dimensions(w, h, 4) ||
        !scale || (scale & (scale - 1)) || w > UINT32_MAX / scale || h > UINT32_MAX / scale) return -1;
    rgb = image(w, h, 3); wanted = image(w, h, 3); smoothed = image(w, h, 3); difference = image(w, h, 1);
    if (!rgb || !wanted || !smoothed || !difference) goto done;
    for (uint32_t channel = 0; channel < 3; channel++) {
        const float *current = top;
        float *owned = NULL;
        uint32_t remaining = scale, cw = w * scale, ch = h * scale;
        while (remaining > 1) {
            uint32_t nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
            float *next = image(nw, nh, 1);
            if (!next || texture_upscale_down2_channel(current, cw, ch, owned ? 1 : tc,
                owned ? 0 : channel, wrap, next)) { free(next); free(owned); goto done; }
            free(owned); current = owned = next; cw = nw; ch = nh; remaining /= 2;
        }
        for (size_t i = 0; i < count; i++) rgb[i * 3 + channel] = owned ? owned[i] : top[i * tc + channel];
        free(owned);
    }
    for (size_t i = 0; i < count; i++) {
        float m = 0;
        memcpy(wanted + i * 3, ref + i * 4, 3 * sizeof(float));
        for (int c = 0; c < 3; c++) m = fmaxf(m, fabsf(wanted[i * 3 + c] - rgb[i * 3 + c]));
        difference[i] = m;
    }
    if (blur(difference, w, h, 1, wrap, smoothed)) goto done;
    for (size_t i = 0; i < count; i++) if (!alpha_visible || ref[i * 4 + 3] > 0) {
        visible++; damaged += smoothed[i] > 21.0 / 255;
    }
    if (blur(rgb, w, h, 3, wrap, smoothed)) goto done;
    for (size_t i = 0; i < count; i++) if (!alpha_visible || ref[i * 4 + 3] > 0)
        for (int c = 0; c < 3; c++) { double hp = rgb[i * 3 + c] - smoothed[i * 3 + c]; numerator += hp * hp; }
    if (blur(wanted, w, h, 3, wrap, smoothed)) goto done;
    for (size_t i = 0; i < count; i++) if (!alpha_visible || ref[i * 4 + 3] > 0)
        for (int c = 0; c < 3; c++) { double hp = wanted[i * 3 + c] - smoothed[i * 3 + c]; denominator += hp * hp; }
    *share = visible ? (double)damaged / visible : 0;
    *loss = denominator <= 1e-12 ? 0 : 1 - sqrt(numerator / denominator);
    result = 0;
done:
    free(rgb); free(wanted); free(smoothed); free(difference);
    return result;
}

/* All FFTs are complex double precision. Bluestein keeps the requested periodic domain for arbitrary sizes,
   rather than padding the height field itself, which would change its edge slopes. */
struct fft_plan {
    size_t n, m;
    vDSP_Length log2m;
    FFTSetupD setup;
    double *real, *imag, *kernel_real, *kernel_imag;
};

static void fft_destroy(struct fft_plan *p)
{
    if (p->setup) vDSP_destroy_fftsetupD(p->setup);
    free(p->real); free(p->imag); free(p->kernel_real); free(p->kernel_imag);
    memset(p, 0, sizeof(*p));
}

static int fft_create(struct fft_plan *p, size_t n)
{
    p->n = n; p->m = 1;
    while (p->m < ((n & (n - 1)) ? 2 * n - 1 : n)) { p->m *= 2; p->log2m++; }
    p->real = calloc(p->m, sizeof(double)); p->imag = calloc(p->m, sizeof(double));
    if (!p->real || !p->imag) goto fail;
    if (n == 1) return 0;
    p->setup = vDSP_create_fftsetupD(p->log2m, kFFTRadix2);
    if (!p->setup) goto fail;
    if (p->m != n) {
        DSPDoubleSplitComplex kernel;
        p->kernel_real = calloc(p->m, sizeof(double)); p->kernel_imag = calloc(p->m, sizeof(double));
        if (!p->kernel_real || !p->kernel_imag) goto fail;
        for (size_t j = 0; j < n; j++) {
            double a = M_PI * (double)j * j / n;
            p->kernel_real[j] = cos(a); p->kernel_imag[j] = sin(a);
            if (j) { p->kernel_real[p->m - j] = cos(a); p->kernel_imag[p->m - j] = sin(a); }
        }
        kernel = (DSPDoubleSplitComplex){ p->kernel_real, p->kernel_imag };
        vDSP_fft_zipD(p->setup, &kernel, 1, p->log2m, FFT_FORWARD);
    }
    return 0;
fail:
    fft_destroy(p);
    return -1;
}

static void fft_line(struct fft_plan *p, double *real, double *imag, size_t stride, int inverse)
{
    DSPDoubleSplitComplex work = { p->real, p->imag };
    size_t n = p->n, m = p->m;
    if (n == 1) return;
    memset(p->real, 0, m * sizeof(double)); memset(p->imag, 0, m * sizeof(double));
    for (size_t j = 0; j < n; j++) {
        double r = real[j * stride], i = imag[j * stride] * (inverse ? -1 : 1);
        if (m == n) { p->real[j] = r; p->imag[j] = i; }
        else {
            double a = M_PI * (double)j * j / n, c = cos(a), s = sin(a);
            p->real[j] = r * c + i * s; p->imag[j] = i * c - r * s;
        }
    }
    vDSP_fft_zipD(p->setup, &work, 1, p->log2m, FFT_FORWARD);
    if (m != n) {
        for (size_t j = 0; j < m; j++) {
            double r = p->real[j], i = p->imag[j];
            p->real[j] = r * p->kernel_real[j] - i * p->kernel_imag[j];
            p->imag[j] = r * p->kernel_imag[j] + i * p->kernel_real[j];
        }
        vDSP_fft_zipD(p->setup, &work, 1, p->log2m, FFT_INVERSE);
    }
    for (size_t j = 0; j < n; j++) {
        double r = p->real[j], i = p->imag[j], factor = inverse ? 1.0 / n : 1;
        if (m != n) {
            double a = M_PI * (double)j * j / n, c = cos(a), s = sin(a);
            double t = (r * c + i * s) / m;
            i = (i * c - r * s) / m; r = t;
        }
        real[j * stride] = r * factor; imag[j * stride] = i * factor * (inverse ? -1 : 1);
    }
}

static void fft2(struct fft_plan *x, struct fft_plan *y, double *real, double *imag, uint32_t w, uint32_t h, int inverse)
{
    for (uint32_t row = 0; row < h; row++) fft_line(x, real + (size_t)row * w, imag + (size_t)row * w, 1, inverse);
    for (uint32_t col = 0; col < w; col++) fft_line(y, real + col, imag + col, w, inverse);
}

int texture_upscale_integrate_normals(const uint8_t *rgba, uint32_t w, uint32_t h, float *out, float *low, float *span)
{
    size_t count = (size_t)w * h;
    double *gx = NULL, *gxi = NULL, *gy = NULL, *gyi = NULL;
    struct fft_plan x = {0}, y = {0};
    float minimum = INFINITY, maximum = -INFINITY;
    int result = -1;
    if (!rgba || !out || !dimensions(w, h, 4) || count > SIZE_MAX / sizeof(double)) return -1;
    gx = calloc(count, sizeof(double)); gxi = calloc(count, sizeof(double));
    gy = calloc(count, sizeof(double)); gyi = calloc(count, sizeof(double));
    if (!gx || !gxi || !gy || !gyi || fft_create(&x, w) || fft_create(&y, h)) goto done;
    for (size_t i = 0; i < count; i++) {
        float nx = rgba[i * 4] / 127.5f - 1, ny = rgba[i * 4 + 1] / 127.5f - 1, nz = rgba[i * 4 + 2] / 127.5f - 1;
        float length = fmaxf(sqrtf(nx * nx + ny * ny + nz * nz), 1e-6f);
        nx /= length; ny /= length; nz /= length; nz = fmaxf(nz, 1e-3f);
        gx[i] = -nx / nz; gy[i] = -ny / nz;
    }
    fft2(&x, &y, gx, gxi, w, h, 0); fft2(&x, &y, gy, gyi, w, h, 0);
    for (uint32_t row = 0; row < h; row++) for (uint32_t col = 0; col < w; col++) {
        size_t i = (size_t)row * w + col;
        double xr = cos(2 * M_PI * col / w) - 1, xi = sin(2 * M_PI * col / w);
        double yr = cos(2 * M_PI * row / h) - 1, yi = sin(2 * M_PI * row / h);
        double denominator = xr * xr + xi * xi + yr * yr + yi * yi;
        double r = xr * gx[i] + xi * gxi[i] + yr * gy[i] + yi * gyi[i];
        double im = xr * gxi[i] - xi * gx[i] + yr * gyi[i] - yi * gy[i];
        gx[i] = i ? r / denominator : 0; gxi[i] = i ? im / denominator : 0;
    }
    fft2(&x, &y, gx, gxi, w, h, 1);
    for (size_t i = 0; i < count; i++) { out[i] = (float)gx[i]; minimum = fminf(minimum, out[i]); maximum = fmaxf(maximum, out[i]); }
    if (low) *low = minimum;
    if (span) *span = fmaxf(maximum - minimum, 1e-6f);
    result = 0;
done:
    fft_destroy(&x); fft_destroy(&y); free(gx); free(gxi); free(gy); free(gyi);
    return result;
}

void texture_upscale_derive_normals(const float *height, uint32_t w, uint32_t h, float factor, float *normals)
{
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        size_t i = (size_t)y * w + x;
        float c = height[i] * factor;
        float l = height[(size_t)y * w + (x + w - 1) % w] * factor, r = height[(size_t)y * w + (x + 1) % w] * factor;
        float u = height[(size_t)((y + h - 1) % h) * w + x] * factor, d = height[(size_t)((y + 1) % h) * w + x] * factor;
        int px = c > l && c > r, tl = !px && l > r, py = c > u && c > d, tu = !py && u > d;
        float xi = tl ? -1 : 1, yj = tu ? -1 : 1, xk = px ? 0 : (tl ? l - c : r - c), yk = py ? 0 : (tu ? u - c : d - c);
        float nx = -xk * yj, ny = -xi * yk, nz = xi * yj, sign = nz < 0 ? -1 : 1;
        float length = sqrtf(nx * nx + ny * ny + nz * nz);
        normals[i * 3] = nx * sign / length; normals[i * 3 + 1] = ny * sign / length; normals[i * 3 + 2] = nz * sign / length;
    }
}
