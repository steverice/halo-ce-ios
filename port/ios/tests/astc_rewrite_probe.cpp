#include "astc_rewrite.h"
#include "astcenc.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>

extern "C" void dxt_reference(int format, const uint8_t *block, unsigned long *pixels);
static astcenc_context *context;
static unsigned checked;
static unsigned random_state = 0x12345678;
static unsigned next_random()
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static void check(enum astc_dxt_format format, const uint8_t *source, unsigned bound = 2)
{
	uint8_t block[16], rgba[64];
	unsigned long reference[16];
	assert(astc_rewrite_block(format, source, block));
	void *slices[] = {rgba};
	astcenc_image image {4, 4, 1, ASTCENC_TYPE_U8, slices};
	const astcenc_swizzle swizzle {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
	assert(astcenc_decompress_image(context, block, sizeof(block), &image, &swizzle, 0) == ASTCENC_SUCCESS);
	assert(astcenc_decompress_reset(context) == ASTCENC_SUCCESS);
	dxt_reference(format, source, reference);
	for (unsigned pixel = 0; pixel < 16; ++pixel)
		for (unsigned channel = 0; channel < 4; ++channel)
		{
			unsigned shift = channel == 0 ? 16 : channel == 1 ? 8 : channel == 2 ? 0 : 24;
			int expected = (reference[pixel] >> shift) & 255;
			int error = std::abs((int)rgba[pixel * 4 + channel] - expected);
			if ((unsigned)error > (channel == 3 ? 0 : bound))
			{
				std::fprintf(stderr, "block %u format %u pixel %u channel %u: expected %d got %u error %d\n",
					checked, format, pixel, channel, expected, rgba[pixel * 4 + channel], error);
				for (unsigned j = 0; j < (format == ASTC_DXT1 ? 8u : 16u); ++j) std::fprintf(stderr, "%02x", source[j]);
				std::fprintf(stderr, " -> ");
				for (unsigned j = 0; j < 16; ++j) std::fprintf(stderr, "%02x", block[j]);
				std::fprintf(stderr, "\n");
				std::abort();
			}
		}
	++checked;
}

static void set16(uint8_t *bytes, unsigned value)
{
	bytes[0] = value; bytes[1] = value >> 8;
}

int main()
{
	astcenc_config config;
	assert(astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST,
		ASTCENC_FLG_DECOMPRESS_ONLY, &config) == ASTCENC_SUCCESS);
	assert(astcenc_context_alloc(&config, 1, &context, nullptr) == ASTCENC_SUCCESS);
	uint8_t source[32] = {}, output[64];
	// Four distinct selectors make transposed weights and endpoint swaps visible.
	set16(source, 0xffff); set16(source + 2, 0);
	for (unsigned i = 4; i < 8; ++i) source[i] = 0xe4;
	check(ASTC_DXT1, source);
	// Exercise every endpoint channel value, both endpoint orders, all selectors.
	for (unsigned a = 0; a < 256; ++a)
		for (unsigned b = 0; b < 256; ++b)
		{
			unsigned ca = ((a >> 3) << 11) | ((a >> 2) << 5) | (a >> 3);
			unsigned cb = ((b >> 3) << 11) | ((b >> 2) << 5) | (b >> 3);
			set16(source, ca); set16(source + 2, cb);
			std::memset(source + 4, ca > cb ? 0xe4 : 0x24, 4);
			check(ASTC_DXT1, source, ca > cb ? 2 : 1);
		}
	for (unsigned trial = 0; trial < 10000; ++trial)
	{
		for (unsigned i = 0; i < 16; ++i) source[i] = next_random();
		unsigned c0 = source[8] | (source[9] << 8), c1 = source[10] | (source[11] << 8);
		std::memcpy(source, source + 8, 8);
		if (c0 <= c1)
			for (unsigned i = 4; i < 8; ++i)
				for (unsigned shift = 0; shift < 8; shift += 2)
					if (((source[i] >> shift) & 3) == 3) source[i] &= ~(1 << shift);
		check(ASTC_DXT1, source, c0 > c1 ? 2 : 1);
		std::memcpy(source + 8, source, 8);
		unsigned alpha = next_random() & 15;
		std::memset(source, alpha * 17, 8);
		check(ASTC_DXT3, source);
		// A selector may use any one of the eight interpolated DXT5 alphas.
		source[0] = next_random(); source[1] = next_random();
		uint64_t selectors = 0;
		unsigned selector = next_random() & 7;
		for (unsigned i = 0; i < 16; ++i) selectors |= (uint64_t)selector << (i * 3);
		for (unsigned i = 0; i < 6; ++i) source[i + 2] = selectors >> (i * 8);
		check(ASTC_DXT5, source);
	}
	// All 3^5 trit groups, including the reserved combinations that carry
	// high trits, with both endpoint sum orders and partial final groups.
	for (unsigned reverse = 0; reverse < 2; ++reverse)
		for (unsigned combination = 0; combination < 243; ++combination)
		{
			std::memset(source, 0, sizeof(source));
			set16(source, reverse ? 0x07ff : 0);
			set16(source + 2, reverse ? 0xf800 : 0xffff);
			unsigned digits = combination;
			const unsigned selectors[3] = {0, 2, 1};
			for (unsigned i = 0; i < 16; ++i)
			{
				if (i % 5 == 0) digits = combination;
				unsigned selector = selectors[digits % 3];
				source[4 + i / 4] |= selector << (2 * (i % 4));
				digits /= 3;
			}
			check(ASTC_DXT1, source, 1);
		}
	for (unsigned alpha = 0; alpha < 256; ++alpha)
	{
		std::memset(source, 0, sizeof(source));
		source[0] = alpha;
		check(ASTC_DXT5, source);
	}
	// Equal decoded alphas need not use equal DXT5 selectors or endpoints.
	std::memset(source, 0, sizeof(source));
	source[0] = source[1] = 117;
	for (unsigned i = 2; i < 8; ++i) source[i] = i & 1 ? 0 : 0x49;
	check(ASTC_DXT5, source);
	// Reject an entire level before writing output, even if only its last block fails.
	std::memset(source, 0, sizeof(source));
	std::memset(output, 0x5a, sizeof(output));
	assert(astc_rewrite_level_supported(ASTC_DXT1, source, 8, 4));
	source[12] = 3;
	assert(!astc_rewrite_level_supported(ASTC_DXT1, source, 8, 4));
	assert(!astc_rewrite_level(ASTC_DXT1, source, 8, 4, output));
	for (uint8_t byte : output) assert(byte == 0x5a);
	std::memset(source, 0, sizeof(source)); source[16] = 0x10;
	assert(!astc_rewrite_level_supported(ASTC_DXT3, source, 8, 4));
	assert(!astc_rewrite_level(ASTC_DXT3, source, 8, 4, output));
	std::memset(source, 0, sizeof(source)); source[16] = 255; source[18] = 1;
	assert(!astc_rewrite_level_supported(ASTC_DXT5, source, 8, 4));
	// Small and nonmultiple mips keep complete ASTC edge blocks and guard bytes.
	std::memset(source, 0, sizeof(source));
	for (unsigned width : {1u, 2u, 3u, 4u, 5u, 8u})
	{
		std::memset(output, 0x5a, sizeof(output));
		assert(astc_rewrite_level(ASTC_DXT1, source, width, 1, output));
		for (unsigned i = ((width + 3) / 4) * 16; i < sizeof(output); ++i) assert(output[i] == 0x5a);
	}
	assert(!astc_rewrite_level_supported(ASTC_DXT1, source, 0, 4));
	assert(!astc_rewrite_block((astc_dxt_format)99, source, output));
	astcenc_context_free(context);
	std::printf("ASTC direct rewrite: %u independently decoded blocks passed\n", checked);
}
