#include "astc_encode.h"
#include "astc_quality.h"
#include "astcenc.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace {
struct buffers_free { void operator()(uint8_t *p) const { free(p); } };
using buffer = std::unique_ptr<uint8_t, buffers_free>;
struct encoder_contexts
{
	astcenc_context *context = nullptr;
	bool binary_mode = false;
	~encoder_contexts() { astcenc_context_free(context); }
	astcenc_context *get(bool binary)
	{
		if (context && binary_mode != binary)
		{
			astcenc_context_free(context);
			context = nullptr;
		}
		binary_mode = binary;
		if (!context)
		{
			astcenc_config config;
			if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FAST, 0, &config) != ASTCENC_SUCCESS)
				return nullptr;
			if (binary) config.cw_a_weight = 8.0f;
			if (astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS) return nullptr;
		}
		return context;
	}
};
thread_local encoder_contexts encoders;
const astcenc_swizzle swizzle { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

static void color565(unsigned value, uint8_t *color)
{
	unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
	color[0] = (r << 3) | (r >> 2);
	color[1] = (g << 2) | (g >> 4);
	color[2] = (b << 3) | (b >> 2);
	color[3] = 255;
}

/* Match xbox_textures.c's integer DXT interpolation, including black RGB for
   DXT1 transparent pixels and the unconditional four-color DXT3/5 palette. */
static void decode_block(astc_dxt_format format, const uint8_t *source, uint8_t pixels[64])
{
	const uint8_t *color = source + (format == ASTC_DXT1 ? 0 : 8);
	unsigned c0 = color[0] | (color[1] << 8), c1 = color[2] | (color[3] << 8);
	uint8_t palette[4][4] = {};
	color565(c0, palette[0]); color565(c1, palette[1]);
	bool four = format != ASTC_DXT1 || c0 > c1;
	for (unsigned c = 0; c < 3; ++c)
	{
		palette[2][c] = four ? (2 * palette[0][c] + palette[1][c]) / 3 : (palette[0][c] + palette[1][c]) / 2;
		palette[3][c] = four ? (palette[0][c] + 2 * palette[1][c]) / 3 : 0;
	}
	palette[2][3] = 255; palette[3][3] = four ? 255 : 0;
	uint8_t alpha[8] = { source[0], source[1] };
	uint64_t alpha_bits = 0;
	if (format == ASTC_DXT5)
	{
		for (unsigned b = 0; b < 6; ++b) alpha_bits |= uint64_t(source[b + 2]) << (8 * b);
		if (alpha[0] > alpha[1])
			for (unsigned i = 2; i < 8; ++i) alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7;
		else
		{
			for (unsigned i = 2; i < 6; ++i) alpha[i] = ((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5;
			alpha[6] = 0; alpha[7] = 255;
		}
	}
	for (unsigned i = 0; i < 16; ++i)
	{
		unsigned index = (color[4 + i / 4] >> (2 * (i % 4))) & 3;
		memcpy(pixels + i * 4, palette[index], 4);
		if (format == ASTC_DXT3) pixels[i * 4 + 3] = ((source[i / 2] >> (4 * (i % 2))) & 15) * 17;
		if (format == ASTC_DXT5) pixels[i * 4 + 3] = alpha[(alpha_bits >> (3 * i)) & 7];
	}
}

}

static astc_encode_result encode_level(astc_dxt_format format, const void *source, unsigned width, unsigned height, void *output, bool discardable, const astc_alpha_policy *policy)
{
	/* Xbox texture dimensions are at most 4096. Bound scratch allocations before
	   touching source bytes; malformed map dimensions must not exhaust memory. */
	if (format < ASTC_DXT1 || format > ASTC_DXT5 || !source || !output || !width || !height || width > 4096 || height > 4096)
		return ASTC_ENCODE_ERROR;
	size_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
	size_t texels = size_t(width) * height, bytes = blocks_x * blocks_y * 16;
	buffer candidate(discardable ? nullptr : static_cast<uint8_t *>(malloc(bytes)));
	uint8_t *encoded = discardable ? static_cast<uint8_t *>(output) : candidate.get();
	if (!encoded) return ASTC_ENCODE_ERROR;
	if (astc_rewrite_level(format, source, width, height, encoded))
	{
		if (output != encoded) memcpy(output, encoded, bytes);
		return ASTC_ENCODE_OK;
	}
	/* ASTC blocks have no inter-block dependencies. Tile the input so worker
	   memory remains bounded even for a large texture on a two-core device. */
	const unsigned tile_size = 128;
	buffer pixels(static_cast<uint8_t *>(malloc(tile_size * tile_size * 4)));
	buffer decoded(static_cast<uint8_t *>(malloc(tile_size * tile_size * 4)));
	buffer tile(static_cast<uint8_t *>(malloc(tile_size * tile_size)));
	if (!pixels || !decoded || !tile) return ASTC_ENCODE_ERROR;
	unsigned stride = format == ASTC_DXT1 ? 8 : 16;
	const uint8_t *dxt = static_cast<const uint8_t *>(source);
	bool binary = true;
	for (size_t by = 0; by < blocks_y && binary; ++by) for (size_t bx = 0; bx < blocks_x && binary; ++bx)
	{
		uint8_t block[64];
		decode_block(format, dxt + (by * blocks_x + bx) * stride, block);
		for (unsigned y = 0; y < 4 && by * 4 + y < height; ++y)
		for (unsigned x = 0; x < 4 && bx * 4 + x < width; ++x)
		{
			unsigned alpha = block[(y * 4 + x) * 4 + 3];
			binary &= alpha == 0 || alpha == 255;
		}
	}
	astcenc_context *context = encoders.get(binary);
	if (!context) return ASTC_ENCODE_ERROR;
	astc_quality_metrics metrics;
	for (unsigned top = 0; top < height; top += tile_size)
	for (unsigned left = 0; left < width; left += tile_size)
	{
		unsigned tw = width - left < tile_size ? width - left : tile_size;
		unsigned th = height - top < tile_size ? height - top : tile_size;
		unsigned tbx = (tw + 3) / 4, tby = (th + 3) / 4;
		size_t tile_bytes = size_t(tbx) * tby * 16;
		for (unsigned by = 0; by < tby; ++by) for (unsigned bx = 0; bx < tbx; ++bx)
		{
			uint8_t block[64];
			decode_block(format, dxt + ((top / 4 + by) * blocks_x + left / 4 + bx) * stride, block);
			for (unsigned y = 0; y < 4 && by * 4 + y < th; ++y)
			for (unsigned x = 0; x < 4 && bx * 4 + x < tw; ++x)
				memcpy(pixels.get() + ((by * 4 + y) * tw + bx * 4 + x) * 4, block + (y * 4 + x) * 4, 4);
		}
		void *slice = pixels.get();
		astcenc_image image { tw, th, 1, ASTCENC_TYPE_U8, &slice };
		if (astcenc_compress_image(context, &image, &swizzle, tile.get(), tile_bytes, 0) != ASTCENC_SUCCESS) return ASTC_ENCODE_ERROR;
		for (unsigned by = 0; by < tby; ++by) for (unsigned bx = 0; bx < tbx; ++bx)
		{
			uint8_t direct[16];
			if (astc_rewrite_block(format, dxt + ((top / 4 + by) * blocks_x + left / 4 + bx) * stride, direct))
				memcpy(tile.get() + (by * tbx + bx) * 16, direct, 16);
		}
		slice = decoded.get();
		if (astcenc_decompress_image(context, tile.get(), tile_bytes, &image, &swizzle, 0) != ASTCENC_SUCCESS) return ASTC_ENCODE_ERROR;
		metrics.add(pixels.get(), decoded.get(), size_t(tw) * th, binary);
		for (unsigned by = 0; by < tby; ++by)
			memcpy(encoded + ((top / 4 + by) * blocks_x + left / 4) * 16, tile.get() + by * tbx * 16, tbx * 16);
	}
	if (!metrics.valid(texels, policy)) return ASTC_ENCODE_EXCLUDED;
	if (output != encoded) memcpy(output, encoded, bytes);
	return ASTC_ENCODE_OK;
}

astc_encode_result astc_encode_level_result(astc_dxt_format format, const void *source, unsigned width, unsigned height, void *output)
{
	try { return encode_level(format, source, width, height, output, false, nullptr); }
	catch (const std::bad_alloc &) { return ASTC_ENCODE_ERROR; }
}

astc_encode_result astc_encode_level_discardable(astc_dxt_format format, const void *source, unsigned width, unsigned height, void *output)
{
	try { return encode_level(format, source, width, height, output, true, nullptr); }
	catch (const std::bad_alloc &) { return ASTC_ENCODE_ERROR; }
}

bool astc_encode_level(astc_dxt_format format, const void *source, unsigned width, unsigned height, void *output)
{
	return astc_encode_level_result(format, source, width, height, output) == ASTC_ENCODE_OK;
}

astc_encode_result astc_encode_level_policy(astc_dxt_format format, const void *source, unsigned width, unsigned height, void *output, const astc_alpha_policy *policy)
{
	try { return encode_level(format, source, width, height, output, true, policy); }
	catch (const std::bad_alloc &) { return ASTC_ENCODE_ERROR; }
}

static thread_local const volatile int *rgba_cancel = nullptr;

void astc_encode_set_cancel(const volatile int *flag)
{
	rgba_cancel = flag;
}

astc_encode_result astc_encode_rgba_level(const uint8_t *rgba, unsigned width, unsigned height,
	void *output, unsigned preset, unsigned threads)
{
	const volatile int *cancel = rgba_cancel;
	if (!rgba || !output || !width || !height || width > 8192 || height > 8192 || preset > 2)
		return ASTC_ENCODE_ERROR;
	const float qualities[] = { ASTCENC_PRE_FASTEST, ASTCENC_PRE_FAST, ASTCENC_PRE_MEDIUM };
	const unsigned tile_size = 128, blocks_x = (width + 3) / 4;
	threads = std::min(std::min(threads ? threads : 4, 4u), (height + tile_size - 1) / tile_size);
	std::atomic<unsigned> next_band { 0 };
	std::atomic<bool> failed { false };
	auto work = [&]() {
		astcenc_config config;
		astcenc_context *context = nullptr;
		if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, qualities[preset], 0, &config) != ASTCENC_SUCCESS ||
			astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS)
		{
			failed = true;
			return;
		}
		buffer pixels(static_cast<uint8_t *>(malloc(tile_size * tile_size * 4)));
		buffer compressed(static_cast<uint8_t *>(malloc(tile_size * tile_size)));
		if (!pixels || !compressed) failed = true;
		while (!failed)
		{
			unsigned top = next_band.fetch_add(1) * tile_size;
			if (top >= height) break;
			unsigned th = std::min(tile_size, height - top);
			for (unsigned left = 0; left < width && !failed; left += tile_size)
			{
				if (cancel && *cancel)
				{
					failed = true;
					break;
				}
				unsigned tw = std::min(tile_size, width - left), tbx = (tw + 3) / 4, tby = (th + 3) / 4;
				for (unsigned y = 0; y < th; ++y)
					memcpy(pixels.get() + size_t(y) * tw * 4, rgba + (size_t(top + y) * width + left) * 4, size_t(tw) * 4);
				void *slice = pixels.get();
				astcenc_image image { tw, th, 1, ASTCENC_TYPE_U8, &slice };
				if (astcenc_compress_image(context, &image, &swizzle, compressed.get(), size_t(tbx) * tby * 16, 0) != ASTCENC_SUCCESS)
				{
					failed = true;
					break;
				}
				/* Contexts own per-image state even though their worker count is one. */
				astcenc_compress_reset(context);
				for (unsigned by = 0; by < tby; ++by)
					memcpy(static_cast<uint8_t *>(output) + ((size_t(top / 4 + by) * blocks_x + left / 4) * 16),
						compressed.get() + size_t(by) * tbx * 16, size_t(tbx) * 16);
			}
		}
		astcenc_context_free(context);
	};
	std::vector<std::thread> workers;
	try
	{
		for (unsigned i = 1; i < threads; ++i) workers.emplace_back(work);
		work();
	}
	catch (...)
	{
		failed = true;
	}
	for (auto &worker : workers) worker.join();
	return failed ? ASTC_ENCODE_ERROR : ASTC_ENCODE_OK;
}
