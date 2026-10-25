/* A fixed ASTC 4x4 LDR representation of DXT endpoints and selectors. There is
   no endpoint search or pixel buffer; varying alpha stays on the BGRA8 path. */
#include "astc_rewrite.h"
#include <stddef.h>
#include <string.h>

static unsigned read16(const uint8_t *p)
{
	return p[0] | ((unsigned)p[1] << 8);
}

static void endpoint(unsigned value, uint8_t rgb[3])
{
	unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
	rgb[0] = (r << 3) | (r >> 2);
	rgb[1] = (g << 2) | (g >> 4);
	rgb[2] = (b << 3) | (b >> 2);
}

static bool block_alpha(enum astc_dxt_format format, const uint8_t *source, uint8_t *alpha)
{
	unsigned i, first;
	if (format == ASTC_DXT1)
	{
		*alpha = 255;
		if (read16(source) <= read16(source + 2))
			for (i = 0; i < 16; ++i)
				if (((source[4 + i / 4] >> (2 * (i % 4))) & 3) == 3)
					return false;
		return true;
	}
	if (format == ASTC_DXT3)
	{
		first = source[0] & 15;
		for (i = 0; i < 8; ++i)
			if (source[i] != first * 17)
				return false;
		*alpha = first * 17;
		return true;
	}
	if (format == ASTC_DXT5)
	{
		unsigned palette[8] = {source[0], source[1]};
		uint64_t selectors = 0;
		for (i = 0; i < 6; ++i)
			selectors |= (uint64_t)source[2 + i] << (8 * i);
		if (palette[0] > palette[1])
			for (i = 2; i < 8; ++i)
				palette[i] = ((8 - i) * palette[0] + (i - 1) * palette[1]) / 7;
		else
		{
			for (i = 2; i < 6; ++i)
				palette[i] = ((6 - i) * palette[0] + (i - 1) * palette[1]) / 5;
			palette[6] = 0;
			palette[7] = 255;
		}
		first = palette[selectors & 7];
		for (i = 1; i < 16; ++i)
			if (palette[(selectors >> (i * 3)) & 7] != first)
				return false;
		*alpha = first;
		return true;
	}
	return false;
}

/* Bits in the ASTC weight stream run backward from bit 127. */
static void weight_bits(uint8_t output[16], unsigned value, unsigned count, unsigned offset)
{
	unsigned bit;
	for (bit = 0; bit < count; ++bit)
	{
		unsigned position = 127 - offset - bit;
		output[position / 8] |= ((value >> bit) & 1) << (position % 8);
	}
}

static void bits(uint8_t output[16], unsigned value, unsigned count, unsigned offset)
{
	unsigned bit;
	for (bit = 0; bit < count; ++bit)
		output[(offset + bit) / 8] |= ((value >> bit) & 1) << ((offset + bit) % 8);
}

/* Five base-3 digits share eight bits. The reserved low-bit combinations
   carry the high trits; the final group is truncated to its used bits. */
static unsigned pack_trits(const uint8_t t[5])
{
	static const unsigned third[3][3] = {{3, 7, 15}, {19, 23, 27}, {12, 13, 14}};
	static const unsigned last[3] = {0, 32, 96};
	unsigned low;
	if (t[3] == 2 && t[4] == 2)
	{
		if (t[2] < 2)
			return 28 + 128 * t[2] + 32 * t[1] + t[0];
		if (t[1] < 2)
			return 31 + 128 * t[1] + last[t[0]];
		return 252 + t[0];
	}
	if (t[2] < 2)
		low = 16 * t[2] + 4 * t[1] + t[0];
	else
		low = third[t[1]][t[0]];
	return low + (t[4] < 2 ? 128 * t[4] + 32 * t[3] : 96 + 128 * t[3]);
}

bool astc_rewrite_block(enum astc_dxt_format format, const uint8_t *source, uint8_t output[16])
{
	static const uint8_t four_weights[4] = {0, 3, 1, 2};
	static const uint8_t three_weights[3] = {0, 2, 1};
	uint8_t alpha, rgb[2][3], weights[16];
	const uint8_t *color;
	unsigned i, channel, swap, three;
	if (!source || !output || !block_alpha(format, source, &alpha))
		return false;
	color = source + (format == ASTC_DXT1 ? 0 : 8);
	three = format == ASTC_DXT1 && read16(color) <= read16(color + 2);
	endpoint(read16(color), rgb[0]);
	endpoint(read16(color + 2), rgb[1]);
	/* CEM 8/12 performs blue contraction if the first RGB sum is greater.
	   Reversing endpoints and weights keeps the DXT colors without contraction. */
	swap = rgb[0][0] + rgb[0][1] + rgb[0][2] > rgb[1][0] + rgb[1][1] + rgb[1][2];
	memset(output, 0, 16);
	/* 4x4 weight grid, one plane: mode 0x42 has 2-bit weights; 0x51 has trits.
	   A single partition leaves room for full 8-bit RGB(A) endpoint values. */
	bits(output, three ? 0x51 : 0x42, 11, 0);
	bits(output, format == ASTC_DXT1 ? 8 : 12, 4, 13);
	for (channel = 0; channel < 3; ++channel)
	{
		bits(output, rgb[swap][channel], 8, 17 + channel * 16);
		bits(output, rgb[1 - swap][channel], 8, 25 + channel * 16);
	}
	if (format != ASTC_DXT1)
	{
		bits(output, alpha, 8, 65);
		bits(output, alpha, 8, 73);
	}
	for (i = 0; i < 16; ++i)
	{
		unsigned selector = (color[4 + i / 4] >> (2 * (i % 4))) & 3;
		unsigned weight = three ? three_weights[selector] : four_weights[selector];
		weights[i] = swap ? (three ? 2 : 3) - weight : weight;
	}
	if (three)
	{
		for (i = 0; i < 16; i += 5)
		{
			uint8_t group[5] = {0};
			unsigned count = 16 - i < 5 ? 16 - i : 5;
			memcpy(group, weights + i, count);
			weight_bits(output, pack_trits(group), (count * 8 + 4) / 5, (i / 5) * 8);
		}
	}
	else
		for (i = 0; i < 16; ++i)
			weight_bits(output, weights[i], 2, i * 2);
	return true;
}

static size_t block_count(unsigned width, unsigned height)
{
	size_t x = width / 4 + (width % 4 != 0), y = height / 4 + (height % 4 != 0);
	if (!x || !y || x > SIZE_MAX / 16 / y)
		return 0;
	return x * y;
}

bool astc_rewrite_level_supported(enum astc_dxt_format format, const void *source, unsigned width, unsigned height)
{
	const uint8_t *bytes = source;
	size_t count = block_count(width, height), i;
	unsigned stride = format == ASTC_DXT1 ? 8 : 16;
	uint8_t alpha;
	if (!bytes || !count)
		return false;
	for (i = 0; i < count; ++i)
		if (!block_alpha(format, bytes + i * stride, &alpha))
			return false;
	return true;
}

bool astc_rewrite_level(enum astc_dxt_format format, const void *source, unsigned width, unsigned height, void *output)
{
	const uint8_t *bytes = source;
	uint8_t *result = output;
	size_t count = block_count(width, height), i;
	unsigned stride = format == ASTC_DXT1 ? 8 : 16;
	if (!result || !astc_rewrite_level_supported(format, source, width, height))
		return false;
	for (i = 0; i < count; ++i)
		if (!astc_rewrite_block(format, bytes + i * stride, result + i * 16))
			return false;
	return true;
}
