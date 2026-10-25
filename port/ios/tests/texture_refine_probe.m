/* texture_refine.m against the Mac's NumPy reference (step-tools/upscale_lib/consistency.py, bump.refine):
   no argument: synthetic checks; a folder: every <name>.trf fixture there, PSNR over 40 dB */
#import <Foundation/Foundation.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texture_refine.h"
#include "texture_upscale_pixel_fixtures.h"

static double psnr(const float *a, const float *b, size_t n, double peak)
{
	double sum = 0;
	size_t i;

	for (i = 0; i < n; i++)
		sum += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
	return sum == 0 ? 200 : 10 * log10(peak * peak / (sum / n));
}

static void synthetic(struct texture_refine_context *context)
{
	enum { W = 16, H = 16, S = 2 };
	static float reference[W * H], top[W * S * H * S], mask[W * H];
	struct texture_refine_params params = { 12, 30, 1, 3 };
	size_t i;

	for (i = 0; i < W * H; i++)
		reference[i] = 0.5f;
	for (i = 0; i < W * S * H * S; i++)
		top[i] = 0.5f;                       /* consistent: bp is a no-op, nothing is masked */
	assert(texture_refine_color(context, top, reference, W, H, 1, S, 1, &params, 1, mask) == 0);
	for (i = 0; i < W * S * H * S; i++)
		assert(fabsf(top[i] - 0.5f) < 1e-4f);
	for (i = 0; i < W * H; i++)
		assert(mask[i] == 0);
	for (i = 0; i < W * S * H * S; i++)
		top[i] = 0.9f;                       /* 102 levels off everywhere: masked, and pulled to the original */
	assert(texture_refine_color(context, top, reference, W, H, 1, S, 1, &params, 1, mask) == 0);
	for (i = 0; i < W * H; i++)
		assert(mask[i] > 0.99f);
	for (i = 0; i < W * S * H * S; i++)
		assert(fabsf(top[i] - 0.5f) < 1e-3f);
	/* out-of-range input is clipped before the first round (consistency.back_project): a checker of 1.4 and 0.6 along
	   x over an original of 1.0. Clipped, down(top) is 0.8 and one round lifts the 0.6 texels to 0.8; unclipped,
	   down(top) is already 1.0 and they stay at 0.6 */
	params.iterations = 1;
	for (i = 0; i < W * H; i++)
		reference[i] = 1.0f;
	for (i = 0; i < W * S * H * S; i++)
		top[i] = i % 2 ? 0.6f : 1.4f;
	assert(texture_refine_color(context, top, reference, W, H, 1, S, 1, &params, 0, NULL) == 0);
	for (i = 0; i < W * S * H * S; i++)
		assert(fabsf(top[i] - (i % 2 ? 0.8f : 1.0f)) < 1e-4f);
	puts("texture_refine_probe: synthetic ok");
}

/* a 1-texel-wide original (8 x 1, scale 2, wrap, bpf): every upsampled buffer must be the full 16 x 2, as NumPy's
   broadcast of its skipped axis makes it. Expected values from step-tools' consistency.py and the bump rule's steps
   (bump.refine after its h_orig, which is given here); generated once and pasted */
static void one_wide(struct texture_refine_context *context)
{
	static const float reference[] = { 0.300000012f, 0.349999994f, 0.400000006f, 0.449999988f, 0.5f, 0.550000012f, 0.600000024f, 0.649999976f };
	static const float top[] = { 0.5f, 0.845283747f, 0.885423303f, 0.397783548f, 0.70620054f, 0.100030698f, 0.22489354f, 0.388233811f, 0.146618143f, 0.840174675f, 0.586048007f, 0.79375881f, 0.899417341f, 0.316985637f, 0.627639353f, 0.108328909f, 0.168869406f, 0.473471254f, 0.195206568f, 0.877478242f, 0.6680668f, 0.72847873f, 0.894708812f, 0.244757324f, 0.54310149f, 0.134967014f, 0.128350392f, 0.559950888f, 0.258066863f, 0.897106588f, 0.742215931f, 0.652500212f };
	static const float bpf[] = { 0.37264657f, 0.37264657f, 0.270245105f, 0.270245105f, 0.314242333f, 0.314242333f, 0.373400509f, 0.373647034f, 0.386050761f, 0.395419449f, 0.402636945f, 0.428505927f, 0.55221343f, 0.361814767f, 0.557783902f, 0.31771791f, 0.384360611f, 0.539856613f, 0.399274915f, 0.667114139f, 0.540537715f, 0.548121452f, 0.558558762f, 0.558558762f, 0.576445043f, 0.576445043f, 0.635757625f, 0.635757625f, 0.679754913f, 0.679754913f, 0.577353418f, 0.577353418f };
	static const float mask[] = { 1.0f, 1.0f, 0.969702721f, 0.576526761f, 0.485378206f, 1.0f, 1.0f, 1.0f };
	static const float original_height[] = { 0.0f, 1.56665385f, 1.94769526f, 0.854759753f, -0.885040879f, -1.95506024f, -1.54552901f, 0.0336278006f };
	static const float top_height[] = { 1.5f, 0.680394173f, 1.14726329f, -0.340803146f, 0.254950702f, -1.20171547f, -0.757269144f, -1.49744213f, -1.41333354f, -1.08889842f, -1.40468502f, -0.16822879f, -0.735391259f, 0.831561506f, 0.279768556f, 1.4402554f, 1.16334879f, 1.37157476f, 1.49978793f, 0.657821f, 1.13085341f, -0.365316242f, 0.230060786f, -1.21663952f, -0.778932989f, -1.49575818f, -1.42158246f, -1.0713985f, -1.39563942f, -0.143143281f, -0.713305414f, 0.852434456f };
	static const float bump_bpf[] = { 0.0f, -0.511704206f, 0.859161675f, 0.0f, -0.511704206f, 0.859161675f, 0.0f, -0.875002265f, 0.484118789f, 0.0f, -0.875002265f, 0.484118789f, 0.0f, -0.818267047f, 0.574838281f, 0.0f, -0.818267047f, 0.574838281f, 0.0f, -0.305180997f, 0.952294409f, 0.0f, -0.305180997f, 0.952294409f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.396035284f, 0.918235302f, 0.0f, 0.396035284f, 0.918235302f, 0.0f, 0.748257816f, 0.663408041f, 0.0f, 0.748257816f, 0.663408041f, 0.0f, 0.851261079f, 0.524742365f, 0.0f, 0.851261079f, 0.524742365f, 0.0f, 0.87363565f, 0.48658067f, 0.0f, 0.87363565f, 0.48658067f, 0.0f, 0.849348128f, 0.527833104f, 0.0f, 0.849348128f, 0.527833104f, 0.0f, 0.741229713f, 0.671251476f, 0.0f, 0.741229713f, 0.671251476f, 0.0f, 0.372726977f, 0.927941024f, 0.0f, 0.372726977f, 0.927941024f, 0.0f, -0.822264612f, 0.569105387f, 0.0f, -0.822264612f, 0.569105387f, 0.0f, -0.876412988f, 0.48156026f, 0.0f, -0.876412988f, 0.48156026f, 0.0f, -0.514774978f, 0.857325315f, 0.0f, -0.514774978f, 0.857325315f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
	static const float bump_mask[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	struct texture_refine_params params = { 12, 30, 1, 3 };
	float out[16 * 2], got_mask[8], normals[16 * 2 * 3];
	size_t i;

	memcpy(out, top, sizeof(out));
	assert(texture_refine_color(context, out, reference, 1, 8, 1, 2, 1, &params, 1, got_mask) == 0);
	for (i = 0; i < 16 * 2; i++)
		assert(fabsf(out[i] - bpf[i]) < 1e-4f);
	for (i = 0; i < 8; i++)
		assert(fabsf(got_mask[i] - mask[i]) < 1e-4f);
	assert(texture_refine_bump(context, top_height, original_height, 1, 8, 2, &params, 1, normals, got_mask) == 0);
	for (i = 0; i < 16 * 2 * 3; i++)
		assert(fabsf(normals[i] - bump_bpf[i]) < 1e-4f);
	for (i = 0; i < 8; i++)
		assert(fabsf(got_mask[i] - bump_mask[i]) < 1e-4f);
	puts("texture_refine_probe: one-wide ok");
}

static void scale_eight(struct texture_refine_context *context)
{
	float top[32 * 16], mask[4 * 2];
	struct texture_refine_params params = { 12, 30, 1, 3 };

	for (int fallback = 0; fallback <= 1; fallback++)
	{
		const float *expected = fallback ? pixel_refine8_bpf : pixel_refine8_bp;
		memcpy(top, pixel_refine8_top, sizeof(top));
		assert(texture_refine_color(context, top, pixel_refine8_reference, 4, 2, 1, 8, 1, &params, fallback, mask) == 0);
		for (size_t i = 0; i < 32 * 16; i++)
			assert(fabsf(top[i] - expected[i]) < 1e-4f);
		for (size_t i = 0; i < 4 * 2; i++)
			assert(fabsf(mask[i] - pixel_refine8_mask[i]) < 1e-4f);
	}
	puts("texture_refine_probe: scale eight ok");
}

static int fixture(struct texture_refine_context *context, NSString *path)
{
	NSData *data = [NSData dataWithContentsOfFile:path];
	const unsigned char *bytes = data.bytes;
	uint32_t header[6], iterations;
	float levels[3];
	const float *arrays;
	size_t w, h, c, W, H, at = 4;
	struct texture_refine_params params;
	double worst = 200;
	int fallback;

	if (data.length < 44 || memcmp(bytes, "TRF1", 4))
		return fprintf(stderr, "%s: not a fixture\n", path.UTF8String), 1;
	memcpy(header, bytes + at, sizeof(header)); at += sizeof(header);
	memcpy(levels, bytes + at, sizeof(levels)); at += sizeof(levels);
	memcpy(&iterations, bytes + at, 4); at += 4;
	params = (struct texture_refine_params){ levels[0], levels[1], levels[2], iterations };
	w = header[1]; h = header[2]; c = header[3]; W = w * header[4]; H = h * header[4];
	arrays = (const float *)(bytes + at);
	for (fallback = 0; fallback <= 1; fallback++)
	{
		float *mask = calloc(w * h, sizeof(float));
		double a, m;

		if (header[0] == 0)
		{
			const float *reference = arrays, *top = reference + w * h * c, *expected = top + W * H * c + (fallback ? W * H * c : 0);
			const float *expected_mask = top + 3 * W * H * c;
			float *out = malloc(W * H * c * sizeof(float));

			memcpy(out, top, W * H * c * sizeof(float));
			if (texture_refine_color(context, out, reference, (uint32_t)w, (uint32_t)h, (uint32_t)c, header[4],
				(int)header[5], &params, fallback, mask))
				return 1;
			a = psnr(out, expected, W * H * c, 1);
			m = psnr(mask, expected_mask, w * h, 1);
			free(out);
		}
		else
		{
			const float *reference = arrays, *top = reference + w * h, *expected = top + W * H + (fallback ? W * H * 3 : 0);
			const float *expected_mask = top + W * H + 2 * W * H * 3;
			float *normals = malloc(W * H * 3 * sizeof(float));

			if (texture_refine_bump(context, top, reference, (uint32_t)w, (uint32_t)h, header[4], &params, fallback,
				normals, mask))
				return 1;
			a = psnr(normals, expected, W * H * 3, 2);   /* normals span -1..1: the same scale as encoded levels */
			m = psnr(mask, expected_mask, w * h, 1);
			free(normals);
		}
		printf("%s %s: output %.1f dB, mask %.1f dB\n", path.lastPathComponent.UTF8String, fallback ? "bpf" : "bp", a, m);
		worst = fmin(worst, fmin(a, m));
		free(mask);
	}
	return worst > 40 ? 0 : 1;
}

int main(int argc, char **argv)
{
	@autoreleasepool
	{
		char error[256];
		struct texture_refine_context *context = texture_refine_create(NULL, error, sizeof(error));
		int failures = 0;

		if (!context)
		{
			printf("texture_refine_probe: skipped (%s)\n", error);
			return 0;
		}
		synthetic(context);
		one_wide(context);
		scale_eight(context);
		if (argc > 1)
			for (NSString *name in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:@(argv[1]) error:nil]
				sortedArrayUsingSelector:@selector(compare:)])
				if ([name hasSuffix:@".trf"])
					failures += fixture(context, [@(argv[1]) stringByAppendingPathComponent:name]);
		texture_refine_destroy(context);
		printf("texture_refine_probe: %s\n", failures ? "FAILED" : "ok");
		return failures ? 1 : 0;
	}
}
