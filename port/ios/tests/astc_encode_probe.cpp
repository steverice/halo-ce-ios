#include "astc_encode.h"
#include "astcenc.h"
#include "astcenc_internal.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" void dxt_reference(int, const unsigned char *, unsigned long *);
static_assert(sizeof(astcenc_contexti) + sizeof(block_size_descriptor) + sizeof(compression_working_buffers) < ASTC_ENCODER_CONTEXT_RESERVE,
	"Update the worker memory reserve when changing the encoder");
static astcenc_context *decoder;
static const astcenc_swizzle swizzle { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
static unsigned accepted[3], excluded[3];

/* Independent DXT rounding and reference ASTC decode catch channel swaps,
   alpha-mode errors, missing direct overwrites, and relaxed quality gates. */
static bool check(astc_dxt_format format, const unsigned char *source, unsigned width, unsigned height)
{
	unsigned bx = (width + 3) / 4, by = (height + 3) / 4;
	std::vector<unsigned char> output(bx * by * 16, 0x55), rgba(width * height * 4);
	astc_encode_result result = astc_encode_level_result(format, source, width, height, output.data());
	assert(result != ASTC_ENCODE_ERROR);
	if (result == ASTC_ENCODE_EXCLUDED)
	{
		for (auto b : output) assert(b == 0x55);
		++excluded[format];
		return false;
	}
	std::vector<unsigned char> discardable(output.size());
	assert(astc_encode_level_discardable(format, source, width, height, discardable.data()) == ASTC_ENCODE_OK);
	assert(discardable == output);
	++accepted[format];
	void *slice = rgba.data();
	astcenc_image image { width, height, 1, ASTCENC_TYPE_U8, &slice };
	assert(astcenc_decompress_image(decoder, output.data(), output.size(), &image, &swizzle, 0) == ASTCENC_SUCCESS);
	double error = 0;
	unsigned channels = 0, crossings[256] = {};
	bool binary = true;
	for (unsigned yb = 0; yb < by; ++yb) for (unsigned xb = 0; xb < bx; ++xb)
	{
		unsigned long original[16];
		unsigned block = yb * bx + xb;
		const unsigned char *dxt = source + block * (format == ASTC_DXT1 ? 8 : 16);
		dxt_reference(format, dxt, original);
		unsigned char direct[16];
		if (astc_rewrite_block(format, dxt, direct)) assert(!memcmp(direct, output.data() + block * 16, 16));
		for (unsigned y = 0; y < 4 && yb * 4 + y < height; ++y)
		for (unsigned x = 0; x < 4 && xb * 4 + x < width; ++x)
		{
			unsigned long pixel = original[y * 4 + x];
			const unsigned char *actual = rgba.data() + ((yb * 4 + y) * width + xb * 4 + x) * 4;
			unsigned alpha = pixel >> 24;
			binary &= alpha == 0 || alpha == 255;
			for (unsigned r = 1; r < 256; ++r) crossings[r] += (alpha >= r) != (actual[3] >= r);
			if (alpha) for (unsigned c = 0; c < 3; ++c)
			{
				int delta = int((pixel >> ((2 - c) * 8)) & 255) - actual[c];
				error += delta * delta;
				++channels;
			}
			if (alpha == 0 || alpha == 255) assert(std::abs(int(alpha) - actual[3]) <= 8);
		}
	}
	assert(!channels || error / channels <= 65025.0 / std::pow(10.0, 3.8));
	if (!binary) for (unsigned r = 1; r < 256; ++r) assert(crossings[r] * 1000ull <= width * height);
	return true;
}
int main()
{
	printf("ASTC context structures: %zu bytes per context\n", sizeof(astcenc_contexti) + sizeof(block_size_descriptor) + sizeof(compression_working_buffers));
	astcenc_config config;
	assert(astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &config) == ASTCENC_SUCCESS);
	assert(astcenc_context_alloc(&config, 1, &decoder, nullptr) == ASTCENC_SUCCESS);
	const unsigned char punch[8] = { 0, 0, 255, 255, 0xcc, 0xcc, 0xcc, 0xcc };
	assert(check(ASTC_DXT1, punch, 4, 4));
	unsigned char mixed[16] = {};
	memcpy(mixed, punch, 8);
	mixed[8] = 255; mixed[9] = 255;
	assert(check(ASTC_DXT1, mixed, 8, 4));
	for (unsigned size : {1u, 2u, 3u, 4u}) assert(check(ASTC_DXT1, punch, size, size));
	std::vector<unsigned char> tiled(65 * 33 * 8);
	for (size_t i = 0; i < tiled.size(); i += 8) memcpy(tiled.data() + i, punch, 8);
	assert(check(ASTC_DXT1, tiled.data(), 259, 131));
	unsigned char alpha[16] = {};
	for (unsigned i = 0; i < 8; ++i) alpha[i] = 0xf0;
	assert(check(ASTC_DXT3, alpha, 4, 4));
	memset(alpha, 0, 16); alpha[1] = 255;
	unsigned long long alpha_indices = 0;
	for (unsigned i = 0; i < 16; ++i) alpha_indices |= (unsigned long long)(i & 1) << (i * 3);
	for (unsigned i = 0; i < 6; ++i) alpha[i + 2] = alpha_indices >> (8 * i);
	assert(check(ASTC_DXT5, alpha, 4, 4));
	/* Varying intermediate alpha crosses unused references after compression.
	 * A model/environment material only tests >0 and >127. */
	unsigned char intermediate[16] = {0x14,0x23,0x74,0x62,0x66,0x67,0x27,0x43,0xff,0xff,0xdb,0xde,0xdf,0xfe,0xda,0x7f};
	unsigned char selected_output[16];
	astc_alpha_policy selected = {};
	selected.known = true; selected.boundaries[0] = 2; selected.boundaries[16] = 1;
	assert(astc_encode_level_result(ASTC_DXT3, intermediate, 4, 4, selected_output) == ASTC_ENCODE_EXCLUDED);
	assert(astc_encode_level_policy(ASTC_DXT3, intermediate, 4, 4, selected_output, &selected) == ASTC_ENCODE_OK);
	selected.known = false;
	assert(astc_encode_level_policy(ASTC_DXT3, intermediate, 4, 4, selected_output, &selected) == ASTC_ENCODE_EXCLUDED);
	unsigned random = 0x329a710d;
	for (unsigned f = 0; f < 3; ++f) for (unsigned test = 0; test < 200; ++test)
	{
		unsigned char input[64];
		for (auto &b : input) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; b = random; }
		check(astc_dxt_format(f), input, 7, 5);
	}
	unsigned char output[16]; memset(output, 0x55, 16);
	assert(!astc_encode_level(ASTC_DXT1, punch, 0, 4, output));
	assert(!astc_encode_level(astc_dxt_format(3), punch, 4, 4, output));
	assert(!astc_encode_level(ASTC_DXT1, nullptr, 4, 4, output));
	assert(!astc_encode_level(ASTC_DXT1, punch, 4, 4, nullptr));
	assert(!astc_encode_level(ASTC_DXT1, punch, ~0u, ~0u, output));
	for (auto b : output) assert(b == 0x55);
	assert(excluded[1] && excluded[2]);
	for (unsigned f = 0; f < 3; ++f) printf("ASTC format %u: %u accepted, %u quality exclusions\n", f, accepted[f], excluded[f]);
	astcenc_context_free(decoder);
}
