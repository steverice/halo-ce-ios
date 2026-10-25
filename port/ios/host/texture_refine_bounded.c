#include "texture_refine.h"
#include "texture_upscale_pixels.h"
#include <dispatch/dispatch.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t address(int64_t i, uint32_t size, int wrap)
{
    if (wrap) return (uint32_t)((i % size + size) % size);
    return i < 0 ? 0 : i >= size ? size - 1 : (uint32_t)i;
}

static float unit(float x) { return fminf(1, fmaxf(0, x)); }
static double sinc(double x) { return x == 0 ? 1 : sin(M_PI * x) / (M_PI * x); }
static double lanczos(double x) { return fabs(x) < 3 ? sinc(x) * sinc(x / 3) : 0; }
static double mitchell(double x)
{
    x = fabs(x);
    if (x < 1) return (7 * x * x * x - 12 * x * x + 16.0 / 3) / 6;
    if (x < 2) return ((-7.0 / 3) * x * x * x + 12 * x * x - 20 * x + 32.0 / 3) / 6;
    return 0;
}

struct up_filter { int support; uint32_t scale; float weight[8][7]; };

static struct up_filter make_filter(uint32_t scale, int smooth)
{
    struct up_filter f = { .support = smooth ? 2 : 3, .scale = scale };
    for (uint32_t phase = 0; phase < scale; phase++) {
        double raw[7], sum = 0, frac = (phase + 0.5) / scale - 0.5;
        for (int k = 0; k < 2 * f.support + 1; k++)
            sum += raw[k] = smooth ? mitchell(k - f.support - frac) : lanczos(k - f.support - frac);
        for (int k = 0; k < 2 * f.support + 1; k++) f.weight[phase][k] = (float)(raw[k] / sum);
    }
    return f;
}

/* consistency._upsample_axis accumulates a float32 row before filtering columns. This temporary is source-width,
   so the same rounding is retained without keeping a full-size upsampled residual or fallback image. */
static void up_row(const float *src, uint32_t w, uint32_t h, uint32_t channels, uint32_t y,
    const struct up_filter *f, int wrap, float *vertical, float *out)
{
    for (uint32_t x = 0; x < w; x++) for (uint32_t c = 0; c < channels; c++) {
        float value = 0;
        if (h == 1) value = src[x * channels + c];
        else for (int k = 0; k < 2 * f->support + 1; k++)
            value += f->weight[y % f->scale][k] *
                src[((size_t)address((int64_t)(y / f->scale) + k - f->support, h, wrap) * w + x) * channels + c];
        vertical[(size_t)x * channels + c] = value;
    }
    for (uint32_t x = 0; x < w * f->scale; x++) for (uint32_t c = 0; c < channels; c++) {
        float value = 0;
        if (w == 1) value = vertical[c];
        else for (int k = 0; k < 2 * f->support + 1; k++)
            value += f->weight[x % f->scale][k] *
                vertical[(size_t)address((int64_t)(x / f->scale) + k - f->support, w, wrap) * channels + c];
        out[(size_t)x * channels + c] = value;
    }
}

/* The first half-size plane is scalar even when top is RGBA. Extracting a whole full-resolution channel would
   take 64 MiB at 4096 squared; strided reads keep this temporary at 16 MiB instead. */
static float *down(const float *top, uint32_t w, uint32_t h, uint32_t c, uint32_t scale, int wrap)
{
    float *out = malloc((size_t)w * h * c * sizeof(float));
    if (!out) return NULL;
    for (uint32_t channel = 0; channel < c; channel++) {
        uint32_t bw = w * scale, bh = h * scale, remaining = scale;
        const float *src = top;
        float *owned = NULL;
        while (remaining > 1) {
            uint32_t nw = bw > 1 ? bw / 2 : 1, nh = bh > 1 ? bh / 2 : 1;
            float *next = malloc((size_t)nw * nh * sizeof(float));
            if (!next || texture_upscale_down2_channel(src, bw, bh, owned ? 1 : c, owned ? 0 : channel, wrap, next)) {
                free(next); free(owned); free(out); return NULL;
            }
            free(owned); owned = next; src = next; bw = nw; bh = nh; remaining /= 2;
        }
        for (size_t i = 0; i < (size_t)w * h; i++) out[i * c + channel] = owned[i];
        free(owned);
    }
    return out;
}

static int blur_mask(float *mask, uint32_t w, uint32_t h, float sigma, int wrap)
{
    float *rows, *weights;
    int radius;
    double sum = 0;
    if (sigma <= 0) return 0;
    if (sigma > (INT_MAX - 1) / 6.0) return -1;
    radius = (int)fmax(1, ceil(3 * sigma));
    rows = malloc((size_t)w * h * sizeof(float));
    weights = malloc((size_t)(radius * 2 + 1) * sizeof(float));
    if (!rows || !weights) { free(rows); free(weights); return -1; }
    for (int k = -radius; k <= radius; k++) sum += exp(-0.5 * (k / (double)sigma) * (k / (double)sigma));
    for (int k = -radius; k <= radius; k++) weights[k + radius] =
        (float)(exp(-0.5 * (k / (double)sigma) * (k / (double)sigma)) / sum);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        float value = 0;
        for (int k = -radius; k <= radius; k++) value += weights[k + radius] * mask[(size_t)address((int64_t)y + k, h, wrap) * w + x];
        rows[(size_t)y * w + x] = value;
    }
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        float value = 0;
        for (int k = -radius; k <= radius; k++) value += weights[k + radius] * rows[(size_t)y * w + address((int64_t)x + k, w, wrap)];
        mask[(size_t)y * w + x] = value;
    }
    free(rows); free(weights);
    return 0;
}

static void normal(float c, float l, float r, float u, float d, float *n);
/* Output rows are independent, so the row passes run in bands across the cores; each band owns its source-width
   and row temporaries (a few KiB), so the bounded memory stays bounded. */
#define REFINE_BANDS 32u
enum band_pass { BAND_ADD_COLOR, BAND_FALLBACK_COLOR, BAND_ADD_HEIGHT, BAND_FALLBACK_BUMP };
struct band_work
{
	enum band_pass pass;
	float *top, *normals;
	const float *small, *reference, *mask;
	uint32_t w, h, bw, bh, channels, scale;
	int wrap;
	const struct up_filter *filter, *smooth;
	_Atomic int failed;
};

static void band_rows(void *context, size_t band)
{
	struct band_work *work = context;
	uint32_t first = (uint32_t)((uint64_t)work->bh * band / REFINE_BANDS);
	uint32_t last = (uint32_t)((uint64_t)work->bh * (band + 1) / REFINE_BANDS);
	uint32_t channels = work->channels, w = work->w, bw = work->bw, bh = work->bh;
	float *vertical = malloc((size_t)w * channels * sizeof(float)), *row = malloc((size_t)bw * channels * sizeof(float));
	float *mask_vertical = malloc((size_t)w * sizeof(float)), *mask_row = malloc((size_t)bw * sizeof(float));
	float *up = malloc((size_t)bw * sizeof(float)), *below = malloc((size_t)bw * sizeof(float));
	if (!vertical || !row || !mask_vertical || !mask_row || !up || !below) { work->failed = 1; goto done; }
	for (uint32_t y = first; y < last; y++) switch (work->pass) {
	case BAND_ADD_COLOR: {
		float *out = work->top + (size_t)y * bw * channels;
		up_row(work->small, w, work->h, channels, y, work->filter, work->wrap, vertical, row);
		for (size_t i = 0; i < (size_t)bw * channels; i++) out[i] = unit(out[i] + row[i]);
		break;
	}
	case BAND_FALLBACK_COLOR: {
		float *out = work->top + (size_t)y * bw * channels;
		up_row(work->reference, w, work->h, channels, y, work->filter, work->wrap, vertical, row);
		up_row(work->mask, w, work->h, 1, y, work->smooth, work->wrap, mask_vertical, mask_row);
		for (uint32_t x = 0; x < bw; x++) {
			float s = unit(mask_row[x]);
			for (uint32_t c = 0; c < channels; c++) {
				size_t i = (size_t)x * channels + c;
				out[i] = unit(out[i] * (1 - s) + unit(row[i]) * s);
			}
		}
		break;
	}
	case BAND_ADD_HEIGHT:
		up_row(work->small, w, work->h, 1, y, work->filter, 1, vertical, row);
		for (uint32_t x = 0; x < bw; x++) work->top[(size_t)y * bw + x] += row[x];
		break;
	case BAND_FALLBACK_BUMP: {
		uint32_t scale = work->scale;
		up_row(work->reference, w, work->h, 1, (y + bh - 1) % bh, work->filter, 1, vertical, up);
		up_row(work->reference, w, work->h, 1, y, work->filter, 1, vertical, row);
		up_row(work->reference, w, work->h, 1, (y + 1) % bh, work->filter, 1, vertical, below);
		up_row(work->mask, w, work->h, 1, y, work->smooth, 1, mask_vertical, mask_row);
		for (uint32_t x = 0; x < bw; x++) {
			float target[3], *out = work->normals + ((size_t)y * bw + x) * 3, s = unit(mask_row[x]);
			normal(row[x] * scale, row[(x + bw - 1) % bw] * scale, row[(x + 1) % bw] * scale,
				up[x] * scale, below[x] * scale, target);
			float nx = out[0] * (1 - s) + target[0] * s, ny = out[1] * (1 - s) + target[1] * s;
			float nz = sqrtf(fmaxf(1 - nx * nx - ny * ny, 0));
			float length = fmaxf(sqrtf(nx * nx + ny * ny + nz * nz), 1e-6f);
			out[0] = nx / length; out[1] = ny / length; out[2] = nz / length;
		}
		break;
	}
	}
done:
	free(vertical); free(row); free(mask_vertical); free(mask_row); free(up); free(below);
}

static int run_bands(struct band_work *work)
{
	work->failed = 0;
	dispatch_apply_f(REFINE_BANDS, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), work, band_rows);
	return work->failed ? -1 : 0;
}


int texture_refine_color_bounded(float *top, const float *reference, uint32_t w, uint32_t h,
    uint32_t channels, uint32_t scale, int wrap, const struct texture_refine_params *params, int fallback, float *mask_out)
{
    float *small = NULL, *mask = NULL, *vertical = NULL, *row = NULL, *mask_vertical = NULL, *mask_row = NULL;
    size_t count, large_count;
    uint32_t bw, bh;
    struct up_filter filter, smooth;
    int result = -1;
    if (!top || !reference || !params || !w || !h || !channels || channels > 4 ||
        (scale != 2 && scale != 4 && scale != 8) || w > UINT32_MAX / scale || h > UINT32_MAX / scale ||
        !isfinite(params->low) || !isfinite(params->high) || !isfinite(params->sigma) || params->sigma < 0) return -1;
    bw = w * scale; bh = h * scale;
    if ((size_t)bw > SIZE_MAX / bh / channels / sizeof(float)) return -1;
    count = (size_t)w * h; large_count = (size_t)bw * bh * channels;
    filter = make_filter(scale, 0); smooth = make_filter(scale, 1);
    mask = malloc(count * sizeof(float));
    vertical = malloc((size_t)w * channels * sizeof(float)); row = malloc((size_t)bw * channels * sizeof(float));
    if (!mask || !vertical || !row) goto done;
    if (fallback) {
        mask_vertical = malloc((size_t)w * sizeof(float)); mask_row = malloc((size_t)bw * sizeof(float));
        if (!mask_vertical || !mask_row) goto done;
    }
    /* Capture the raw mismatch before clipping, as consistency.mismatch requires. Alpha contributes whenever the
       caller includes it among the channels, including the fallback mask on coverage edges. */
    small = down(top, w, h, channels, scale, wrap);
    if (!small) goto done;
    for (size_t i = 0; i < count; i++) {
        float value = 0;
        for (uint32_t c = 0; c < channels; c++) value = fmaxf(value, fabsf(reference[i * channels + c] - small[i * channels + c]));
        mask[i] = value;
    }
    free(small); small = NULL;
    if (blur_mask(mask, w, h, params->sigma, wrap)) goto done;
    for (size_t i = 0; i < count; i++) {
        float t = unit((mask[i] - params->low / 255) / fmaxf((params->high - params->low) / 255, 1e-9f));
        mask[i] = t * t * (3 - 2 * t);
    }
    for (size_t i = 0; i < large_count; i++) top[i] = unit(top[i]);
    for (uint32_t iteration = 0; iteration < params->iterations; iteration++) {
        small = down(top, w, h, channels, scale, wrap);
        if (!small) goto done;
        for (size_t i = 0; i < count * channels; i++) small[i] = reference[i] - small[i];
        {
            struct band_work work = { BAND_ADD_COLOR, top, NULL, small, reference, mask, w, h, bw, bh, channels, scale,
                wrap, &filter, &smooth, 0 };
            if (run_bands(&work)) goto done;
        }
        free(small); small = NULL;
    }
    if (fallback) {
        struct band_work work = { BAND_FALLBACK_COLOR, top, NULL, NULL, reference, mask, w, h, bw, bh, channels, scale,
            wrap, &filter, &smooth, 0 };
        if (run_bands(&work)) goto done;
    }
    if (mask_out) memcpy(mask_out, mask, count * sizeof(float));
    result = 0;
done:
    free(small); free(mask); free(vertical); free(row); free(mask_vertical); free(mask_row);
    return result;
}

static void normal(float c, float l, float r, float u, float d, float *n)
{
    int px = c > l && c > r, tl = !px && l > r, py = c > u && c > d, tu = !py && u > d;
    float xi = tl ? -1 : 1, yj = tu ? -1 : 1, xk = px ? 0 : (tl ? l - c : r - c), yk = py ? 0 : (tu ? u - c : d - c);
    float nx = -xk * yj, ny = -xi * yk, nz = xi * yj, sign = nz < 0 ? -1 : 1;
    float length = sqrtf(nx * nx + ny * ny + nz * nz);
    n[0] = nx * sign / length; n[1] = ny * sign / length; n[2] = nz * sign / length;
}

static void normal_at(const float *height, uint32_t w, uint32_t h, uint32_t x, uint32_t y, float *n)
{
    normal(height[(size_t)y * w + x], height[(size_t)y * w + (x + w - 1) % w],
        height[(size_t)y * w + (x + 1) % w], height[(size_t)((y + h - 1) % h) * w + x],
        height[(size_t)((y + 1) % h) * w + x], n);
}


int texture_refine_bump_bounded(float *top, const float *reference, uint32_t w, uint32_t h, uint32_t scale,
    const struct texture_refine_params *params, int fallback, float *normals, float *mask_out)
{
    float *small = NULL, *mask = NULL, *vertical = NULL, *row = NULL, *up = NULL, *below = NULL;
    float *mask_vertical = NULL, *mask_row = NULL;
    size_t count;
    uint32_t bw, bh;
    struct up_filter filter, smooth;
    int result = -1;
    if (!top || !reference || !normals || !params || !w || !h || (scale != 2 && scale != 4 && scale != 8) ||
        w > UINT32_MAX / scale || h > UINT32_MAX / scale || !isfinite(params->low) || !isfinite(params->high) ||
        !isfinite(params->sigma) || params->sigma < 0) return -1;
    bw = w * scale; bh = h * scale;
    if ((size_t)bw > SIZE_MAX / bh / 3 / sizeof(float)) return -1;
    count = (size_t)w * h;
    filter = make_filter(scale, 0); smooth = make_filter(scale, 1);
    mask = malloc(count * sizeof(float)); vertical = malloc((size_t)w * sizeof(float)); row = malloc((size_t)bw * sizeof(float));
    if (!mask || !vertical || !row) goto done;
    if (fallback) {
        up = malloc((size_t)bw * sizeof(float)); below = malloc((size_t)bw * sizeof(float));
        mask_vertical = malloc((size_t)w * sizeof(float)); mask_row = malloc((size_t)bw * sizeof(float));
        if (!up || !below || !mask_vertical || !mask_row) goto done;
    }
    small = down(top, w, h, 1, scale, 1);
    if (!small) goto done;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        float a[3], b[3];
        normal_at(small, w, h, x, y, a); normal_at(reference, w, h, x, y, b);
        mask[(size_t)y * w + x] = fmaxf(fabsf(a[0] - b[0]), fabsf(a[1] - b[1])) / 2;
    }
    free(small); small = NULL;
    if (blur_mask(mask, w, h, params->sigma, 1)) goto done;
    for (size_t i = 0; i < count; i++) {
        float t = unit((mask[i] - params->low / 255) / fmaxf((params->high - params->low) / 255, 1e-9f));
        mask[i] = t * t * (3 - 2 * t);
    }
    for (uint32_t iteration = 0; iteration < params->iterations; iteration++) {
        small = down(top, w, h, 1, scale, 1);
        if (!small) goto done;
        for (size_t i = 0; i < count; i++) small[i] = reference[i] - small[i];
        {
            struct band_work work = { BAND_ADD_HEIGHT, top, NULL, small, reference, mask, w, h, bw, bh, 1, scale, 1,
                &filter, &smooth, 0 };
            if (run_bands(&work)) goto done;
        }
        free(small); small = NULL;
    }
    texture_upscale_derive_normals(top, bw, bh, (float)scale, normals);
    if (fallback) {
        struct band_work work = { BAND_FALLBACK_BUMP, top, normals, NULL, reference, mask, w, h, bw, bh, 1, scale, 1,
            &filter, &smooth, 0 };
        if (run_bands(&work)) goto done;
    }
    if (mask_out) memcpy(mask_out, mask, count * sizeof(float));
    result = 0;
done:
    free(small); free(mask); free(vertical); free(row); free(up); free(below); free(mask_vertical); free(mask_row);
    return result;
}
