/*
XBOX_TEXTURES.C

Xbox texture decoding and the OpenGL texture cache.

An Xbox texture is a Direct3D header - Common, Data (physical address),
Lock, Format and Size - over texels in guest memory. Power-of-two textures
are swizzled (Morton order, one level after another); textures with a Size
field are linear, with a pitch, and are addressed with texel coordinates.
DXT textures are stored as plain 4x4 blocks. Everything except DXT is
converted to 32-bit BGRA on upload.

A cached texture stays valid until any page it was read from is written;
memory_watch.c detects that by write-protecting the pages.
*/

#include "xgpu.h"
#include "astc_rewrite.h"
#ifdef HALO_IOS
#include "astc_store.h"
#endif
#include "hud_hires.h"
#include "menu_files.h"
#include "text_hires.h"
#include "port_config.h"
#include "../game/cache_file_formats.h"
#include "texture_override.h"
#include "texture_override_cache.h"
#include "texture_record.h"
#include "texture_upscale_state.h"
/* render validation's name for a texture this file cannot convert, in a
build configured with it (gpu_validate.h); nothing otherwise */
#include "gpu_validate.h"
#if HALO_RENDER_VALIDATION
#define VALIDATION_NOTE(reason, detail, value) gpu_validation_note(GPU_VALIDATION_##reason, (uint32_t)(detail), (uint32_t)(value))
#else
#define VALIDATION_NOTE(reason, detail, value) ((void)0)
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(HALO_IOS) && !defined(_WIN32)
#include <unistd.h>
#endif

/* ---------- formats */

enum texel_kind
{
	_texel_unknown,
	_texel_a8r8g8b8, _texel_x8r8g8b8, _texel_r5g6b5, _texel_a1r5g5b5, _texel_x1r5g5b5, _texel_a4r4g4b4,
	_texel_l8, _texel_al8, _texel_a8, _texel_a8l8, _texel_p8, _texel_g8b8, _texel_r8b8, _texel_r6g5b5,
	_texel_l16, _texel_v16u16, _texel_a8b8g8r8, _texel_b8g8r8a8, _texel_r8g8b8a8, _texel_r5g5b5a1,
	_texel_r4g4b4a4, _texel_yuy2, _texel_uyvy, _texel_d24s8, _texel_d16,
	_texel_dxt1, _texel_dxt3, _texel_dxt5,
};

struct format_information
{
	unsigned char kind;
	unsigned char bytes; /* per texel; per 4x4 block for DXT */
	unsigned char linear;
};

static struct format_information format_information(DWORD format)
{
	static const struct format_information table[0x42] =
	{
		[0x00] = { _texel_l8, 1, 0 },
		[0x01] = { _texel_al8, 1, 0 },
		[0x02] = { _texel_a1r5g5b5, 2, 0 },
		[0x03] = { _texel_x1r5g5b5, 2, 0 },
		[0x04] = { _texel_a4r4g4b4, 2, 0 },
		[0x05] = { _texel_r5g6b5, 2, 0 },
		[0x06] = { _texel_a8r8g8b8, 4, 0 },
		[0x07] = { _texel_x8r8g8b8, 4, 0 },
		[0x0b] = { _texel_p8, 1, 0 },
		[0x0c] = { _texel_dxt1, 8, 0 },
		[0x0e] = { _texel_dxt3, 16, 0 },
		[0x0f] = { _texel_dxt5, 16, 0 },
		[0x10] = { _texel_a1r5g5b5, 2, 1 },
		[0x11] = { _texel_r5g6b5, 2, 1 },
		[0x12] = { _texel_a8r8g8b8, 4, 1 },
		[0x13] = { _texel_l8, 1, 1 },
		[0x16] = { _texel_r8b8, 2, 1 },
		[0x17] = { _texel_g8b8, 2, 1 },
		[0x19] = { _texel_a8, 1, 0 },
		[0x1a] = { _texel_a8l8, 2, 0 },
		[0x1b] = { _texel_al8, 1, 1 },
		[0x1c] = { _texel_x1r5g5b5, 2, 1 },
		[0x1d] = { _texel_a4r4g4b4, 2, 1 },
		[0x1e] = { _texel_x8r8g8b8, 4, 1 },
		[0x1f] = { _texel_a8, 1, 1 },
		[0x20] = { _texel_a8l8, 2, 1 },
		[0x24] = { _texel_yuy2, 2, 1 },
		[0x25] = { _texel_uyvy, 2, 1 },
		[0x27] = { _texel_r6g5b5, 2, 0 },
		[0x28] = { _texel_g8b8, 2, 0 },
		[0x29] = { _texel_r8b8, 2, 0 },
		[0x2a] = { _texel_d24s8, 4, 0 },
		[0x2b] = { _texel_d24s8, 4, 0 },
		[0x2c] = { _texel_d16, 2, 0 },
		[0x2d] = { _texel_d16, 2, 0 },
		[0x2e] = { _texel_d24s8, 4, 1 },
		[0x2f] = { _texel_d24s8, 4, 1 },
		[0x30] = { _texel_d16, 2, 1 },
		[0x31] = { _texel_d16, 2, 1 },
		[0x32] = { _texel_l16, 2, 0 },
		[0x33] = { _texel_v16u16, 4, 0 },
		[0x35] = { _texel_l16, 2, 1 },
		[0x36] = { _texel_v16u16, 4, 1 },
		[0x37] = { _texel_r6g5b5, 2, 1 },
		[0x38] = { _texel_r5g5b5a1, 2, 0 },
		[0x39] = { _texel_r4g4b4a4, 2, 0 },
		[0x3a] = { _texel_a8b8g8r8, 4, 0 },
		[0x3b] = { _texel_b8g8r8a8, 4, 0 },
		[0x3c] = { _texel_r8g8b8a8, 4, 0 },
		[0x3d] = { _texel_r5g5b5a1, 2, 1 },
		[0x3e] = { _texel_r4g4b4a4, 2, 1 },
		[0x3f] = { _texel_a8b8g8r8, 4, 1 },
		[0x40] = { _texel_b8g8r8a8, 4, 1 },
		[0x41] = { _texel_r8g8b8a8, 4, 1 },
	};
	struct format_information unknown = { _texel_a8r8g8b8, 4, 0 };

	if (format < sizeof(table) / sizeof(table[0]) && table[format].kind != _texel_unknown)
		return table[format];
	/* read as A8R8G8B8, whatever it is */
	VALIDATION_NOTE(FRONT_TEXTURE_FORMAT_UNKNOWN, format, 0);
	return unknown;
}

static BOOL kind_compressed(unsigned char kind)
{
	return kind == _texel_dxt1 || kind == _texel_dxt3 || kind == _texel_dxt5;
}

/* ---------- geometry of a texture in memory */

static unsigned long floor_log2(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

static unsigned long level_dimension(unsigned long base, unsigned long level)
{
	unsigned long value = base >> level;

	return value ? value : 1;
}

void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description)
{
	struct format_information information;

	memset(description, 0, sizeof(*description));
	description->format = (format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT;
	information = format_information(description->format);
	description->cube_map = (format_word & D3DFORMAT_CUBEMAP) != 0;
	description->compressed = kind_compressed(information.kind);
	if (size_word)
	{
		description->width = (size_word & D3DSIZE_WIDTH_MASK) + 1;
		description->height = ((size_word & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
		description->depth = 1;
		description->levels = 1;
		description->pitch = (((size_word & D3DSIZE_PITCH_MASK) >> D3DSIZE_PITCH_SHIFT) + 1) * D3DTEXTURE_PITCH_ALIGNMENT;
		description->linear = TRUE;
	}
	else
	{
		description->width = 1UL << ((format_word & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT);
		description->height = 1UL << ((format_word & D3DFORMAT_VSIZE_MASK) >> D3DFORMAT_VSIZE_SHIFT);
		description->depth = 1UL << ((format_word & D3DFORMAT_PSIZE_MASK) >> D3DFORMAT_PSIZE_SHIFT);
		description->levels = (format_word & D3DFORMAT_MIPMAP_MASK) >> D3DFORMAT_MIPMAP_SHIFT;
		if (!description->levels)
			description->levels = 1;
		description->linear = information.linear;
		description->pitch = description->width * information.bytes;
	}
	if ((format_word & D3DFORMAT_DIMENSION_MASK) >> D3DFORMAT_DIMENSION_SHIFT != 3)
		description->depth = 1;
}

static unsigned long level_bytes(const struct xgpu_texture_description *description, unsigned long level)
{
	struct format_information information = format_information(description->format);
	unsigned long width = level_dimension(description->width, level);
	unsigned long height = level_dimension(description->height, level);
	unsigned long depth = level_dimension(description->depth, level);

	if (description->compressed)
		return ((width + 3) / 4) * ((height + 3) / 4) * information.bytes * depth;
	if (description->linear)
		return description->pitch * height;
	return width * height * depth * information.bytes;
}

unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level)
{
	unsigned long offset = 0;
	unsigned long index;

	for (index = 0; index < level && index < description->levels; index++)
		offset += level_bytes(description, index);
	return offset;
}

unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description)
{
	unsigned long size = xgpu_texture_level_offset(description, description->levels);

	if (description->cube_map)
		size = (size + D3DTEXTURE_CUBEFACE_ALIGNMENT - 1) & ~(unsigned long)(D3DTEXTURE_CUBEFACE_ALIGNMENT - 1);
	return size;
}

/* whether the size is one D3DDevice_GetDeviceCaps allows (d3d8_gl.c): up
to 4096 by 4096, and 512 each way for a volume */
static BOOL texture_size_supported(const struct xgpu_texture_description *description)
{
	if (description->depth > 1)
		return description->width <= 512 && description->height <= 512 && description->depth <= 512;
	return description->width <= 4096 && description->height <= 4096;
}

unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level)
{
	struct format_information information = format_information(description->format);

	if (description->linear)
		return description->pitch;
	if (description->compressed)
		return ((level_dimension(description->width, level) + 3) / 4) * information.bytes;
	return level_dimension(description->width, level) * information.bytes;
}

/* ---------- swizzling */

struct swizzle_masks
{
	unsigned long x, y, z;
};

static struct swizzle_masks swizzle_masks(unsigned long width, unsigned long height, unsigned long depth)
{
	struct swizzle_masks masks = { 0, 0, 0 };
	unsigned long bit = 1, mask_bit = 1;
	BOOL done;

	/* bits of x, y and z alternate until each dimension runs out */
	do
	{
		done = TRUE;
		if (bit < width)
		{
			masks.x |= mask_bit;
			mask_bit <<= 1;
			done = FALSE;
		}
		if (bit < height)
		{
			masks.y |= mask_bit;
			mask_bit <<= 1;
			done = FALSE;
		}
		if (bit < depth)
		{
			masks.z |= mask_bit;
			mask_bit <<= 1;
			done = FALSE;
		}
		bit <<= 1;
	} while (!done);
	return masks;
}

static unsigned long spread(unsigned long mask, unsigned long value)
{
	unsigned long result = 0, bit = 1;

	while (value && bit)
	{
		if (mask & bit)
		{
			if (value & 1)
				result |= bit;
			value >>= 1;
		}
		bit <<= 1;
	}
	return result;
}

/* ---------- texel conversion */

static unsigned long expand5(unsigned long v) { return (v << 3) | (v >> 2); }
static unsigned long expand6(unsigned long v) { return (v << 2) | (v >> 4); }
static unsigned long expand4(unsigned long v) { return v * 0x11; }

static unsigned long argb(unsigned long a, unsigned long r, unsigned long g, unsigned long b)
{
	return (a << 24) | (r << 16) | (g << 8) | b;
}

static unsigned char clamp_byte(long value)
{
	return (unsigned char)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static unsigned long yuv_to_argb(long y, long u, long v)
{
	long c = y - 16, d = u - 128, e = v - 128;

	return argb(255, clamp_byte((298 * c + 409 * e + 128) >> 8),
		clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
		clamp_byte((298 * c + 516 * d + 128) >> 8));
}

/* a texel's 16 and 32 bits, read only for the kinds that have them (the last
texel of a 1-byte texture read 3 bytes past it) */
#define TEXEL16(source) ((unsigned long)(source)[0] | ((unsigned long)(source)[1] << 8))
#define TEXEL32(source) (TEXEL16(source) | ((unsigned long)(source)[2] << 16) | ((unsigned long)(source)[3] << 24))

static unsigned long convert_texel(unsigned char kind, const unsigned char *source, const D3DCOLOR *palette,
	unsigned long x, const unsigned char *row)
{
	switch (kind)
	{
	case _texel_a8r8g8b8: return TEXEL32(source);
	case _texel_x8r8g8b8: return TEXEL32(source) | 0xff000000UL;
	case _texel_r5g6b5: return argb(255, expand5(TEXEL16(source) >> 11), expand6((TEXEL16(source) >> 5) & 0x3f), expand5(TEXEL16(source) & 0x1f));
	case _texel_a1r5g5b5: return argb((TEXEL16(source) & 0x8000) ? 255 : 0, expand5((TEXEL16(source) >> 10) & 0x1f), expand5((TEXEL16(source) >> 5) & 0x1f), expand5(TEXEL16(source) & 0x1f));
	case _texel_x1r5g5b5: return argb(255, expand5((TEXEL16(source) >> 10) & 0x1f), expand5((TEXEL16(source) >> 5) & 0x1f), expand5(TEXEL16(source) & 0x1f));
	case _texel_a4r4g4b4: return argb(expand4(TEXEL16(source) >> 12), expand4((TEXEL16(source) >> 8) & 0xf), expand4((TEXEL16(source) >> 4) & 0xf), expand4(TEXEL16(source) & 0xf));
	case _texel_l8: return argb(255, source[0], source[0], source[0]);
	case _texel_al8: return argb(source[0], source[0], source[0], source[0]);
	case _texel_a8: return argb(source[0], 255, 255, 255);
	case _texel_a8l8: return argb(source[1], source[0], source[0], source[0]);
	case _texel_p8: return palette ? palette[source[0]] : argb(255, source[0], source[0], source[0]);
	/* V8U8 shares this format: U (the low byte) reads as red, V as green */
	case _texel_g8b8: return argb(255, source[0], source[1], 0);
	case _texel_r8b8: return argb(255, source[1], 0, source[0]);
	case _texel_r6g5b5: return argb(255, expand6(TEXEL16(source) >> 10), expand5((TEXEL16(source) >> 5) & 0x1f), expand5(TEXEL16(source) & 0x1f));
	case _texel_l16: return argb(255, source[1], source[1], source[1]);
	case _texel_v16u16: return argb(255, source[1], source[3], 0);
	case _texel_a8b8g8r8: return argb(source[3], source[0], source[1], source[2]);
	case _texel_b8g8r8a8: return argb(source[0], source[1], source[2], source[3]);
	case _texel_r8g8b8a8: return argb(source[0], source[3], source[2], source[1]);
	case _texel_r5g5b5a1: return argb((TEXEL16(source) & 1) ? 255 : 0, expand5(TEXEL16(source) >> 11), expand5((TEXEL16(source) >> 6) & 0x1f), expand5((TEXEL16(source) >> 1) & 0x1f));
	case _texel_r4g4b4a4: return argb(expand4(TEXEL16(source) & 0xf), expand4(TEXEL16(source) >> 12), expand4((TEXEL16(source) >> 8) & 0xf), expand4((TEXEL16(source) >> 4) & 0xf));
	case _texel_yuy2:
	{
		const unsigned char *pair = row + (x & ~1UL) * 2;

		return yuv_to_argb(pair[(x & 1) ? 2 : 0], pair[1], pair[3]);
	}
	case _texel_uyvy:
	{
		const unsigned char *pair = row + (x & ~1UL) * 2;

		return yuv_to_argb(pair[(x & 1) ? 3 : 1], pair[0], pair[2]);
	}
	case _texel_d24s8: return argb(255, source[3], source[3], source[3]);
	case _texel_d16: return argb(255, source[1], source[1], source[1]);
	default: return TEXEL32(source);
	}
}

/* one level (or 3D slice set) of an uncompressed texture into BGRA; FALSE
when out of memory */
static BOOL decode_level(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, const D3DCOLOR *palette, unsigned long *destination)
{
	struct format_information information = format_information(description->format);
	unsigned long width = level_dimension(description->width, level);
	unsigned long height = level_dimension(description->height, level);
	unsigned long depth = level_dimension(description->depth, level);
	unsigned long x, y, z;

	if (description->linear)
	{
		/* only the texels a row's pitch holds: a Size word whose pitch is
		narrower than its width (a map's bitmap) read past the texture's
		pitch * height bytes; the rest of such a row is black (a YUV texel
		reads its pair's four bytes) */
		unsigned long row_texels = information.bytes ? description->pitch / information.bytes : 0;

		if (information.kind == _texel_yuy2 || information.kind == _texel_uyvy)
			row_texels &= ~1UL;
		for (y = 0; y < height; y++)
		{
			const unsigned char *row = source + y * description->pitch;

			for (x = 0; x < width; x++)
				destination[y * width + x] = x < row_texels ?
					convert_texel(information.kind, row + x * information.bytes, palette, x, row) : 0;
		}
		return TRUE;
	}
	{
		struct swizzle_masks masks = swizzle_masks(width, height, depth);
		unsigned long *x_offsets = malloc(width * sizeof(unsigned long));

		if (!x_offsets)
			return FALSE;
		for (x = 0; x < width; x++)
			x_offsets[x] = spread(masks.x, x);
		for (z = 0; z < depth; z++)
		{
			unsigned long z_offset = spread(masks.z, z);

			for (y = 0; y < height; y++)
			{
				unsigned long y_offset = spread(masks.y, y) | z_offset;

				for (x = 0; x < width; x++)
				{
					const unsigned char *texel = source + (x_offsets[x] | y_offset) * information.bytes;

					destination[(z * height + y) * width + x] = convert_texel(information.kind, texel, palette, x, texel);
				}
			}
		}
		free(x_offsets);
	}
	return TRUE;
}

/* ---------- DXT decoding, for drivers without S3TC (gpu_capabilities.s3tc) */

static unsigned long color565(unsigned long value)
{
	return argb(255, expand5(value >> 11), expand6((value >> 5) & 0x3f), expand5(value & 0x1f));
}

static unsigned long mix(unsigned long a, unsigned long b, unsigned long weight_a, unsigned long weight_b,
	unsigned long divisor)
{
	unsigned long result = 0;
	int shift;

	for (shift = 0; shift < 24; shift += 8)
	{
		unsigned long channel = (((a >> shift) & 0xff) * weight_a + ((b >> shift) & 0xff) * weight_b) / divisor;

		result |= channel << shift;
	}
	return result | 0xff000000UL;
}

/* one 4x4 block's colors; dxt1 selects the punch-through alpha mode */
static void dxt_color_block(const unsigned char *block, BOOL dxt1, unsigned long colors[16])
{
	unsigned long c0 = block[0] | (block[1] << 8);
	unsigned long c1 = block[2] | (block[3] << 8);
	unsigned long palette[4];
	unsigned long bits = block[4] | (block[5] << 8) | ((unsigned long)block[6] << 16) | ((unsigned long)block[7] << 24);
	int index;

	palette[0] = color565(c0);
	palette[1] = color565(c1);
	if (c0 > c1 || !dxt1)
	{
		palette[2] = mix(palette[0], palette[1], 2, 1, 3);
		palette[3] = mix(palette[0], palette[1], 1, 2, 3);
	}
	else
	{
		palette[2] = mix(palette[0], palette[1], 1, 1, 2);
		palette[3] = 0;
	}
	for (index = 0; index < 16; index++)
		colors[index] = palette[(bits >> (index * 2)) & 3];
}

static void dxt_decode_level(unsigned char kind, const unsigned char *source, unsigned long width, unsigned long height,
	unsigned long depth, unsigned long *destination)
{
	unsigned long blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
	unsigned long block_bytes = kind == _texel_dxt1 ? 8 : 16;
	unsigned long z, bx, by, x, y;

	for (z = 0; z < depth; z++)
	{
		for (by = 0; by < blocks_y; by++)
		{
			for (bx = 0; bx < blocks_x; bx++)
			{
				const unsigned char *block = source + ((z * blocks_y + by) * blocks_x + bx) * block_bytes;
				unsigned long colors[16];
				unsigned long alpha[16];
				int index;

				if (kind == _texel_dxt1)
				{
					dxt_color_block(block, TRUE, colors);
					for (index = 0; index < 16; index++)
						alpha[index] = colors[index] >> 24;
				}
				else
				{
					dxt_color_block(block + 8, FALSE, colors);
					if (kind == _texel_dxt3)
					{
						for (index = 0; index < 16; index++)
							alpha[index] = expand4((block[index / 2] >> ((index & 1) * 4)) & 0xf);
					}
					else
					{
						unsigned long a0 = block[0], a1 = block[1], values[8];
						unsigned long long bits = 0;
						int bit;

						for (bit = 0; bit < 6; bit++)
							bits |= (unsigned long long)block[2 + bit] << (bit * 8);
						values[0] = a0;
						values[1] = a1;
						if (a0 > a1)
						{
							for (index = 2; index < 8; index++)
								values[index] = ((8 - index) * a0 + (index - 1) * a1) / 7;
						}
						else
						{
							for (index = 2; index < 6; index++)
								values[index] = ((6 - index) * a0 + (index - 1) * a1) / 5;
							values[6] = 0;
							values[7] = 255;
						}
						for (index = 0; index < 16; index++)
							alpha[index] = values[(bits >> (index * 3)) & 7];
					}
				}
				for (y = 0; y < 4; y++)
				{
					for (x = 0; x < 4; x++)
					{
						unsigned long px = bx * 4 + x, py = by * 4 + y;

						if (px < width && py < height)
						{
							destination[(z * height + py) * width + px] =
								(colors[y * 4 + x] & 0x00ffffffUL) | (alpha[y * 4 + x] << 24);
						}
					}
				}
			}
		}
	}
}

/* ---------- upload */

/* debug.texture_dump_directory writes level 0 of every upload as a TGA, read
back through gpu_texture_read (on ES, uncompressed textures only) */
static void texture_dump(gpu_texture texture, uint32_t type, const struct xgpu_texture_description *description)
{
	static unsigned long dump_index = 0;
	const char *directory = *config_string("debug.texture_dump_directory") ?
		config_string("debug.texture_dump_directory") : NULL;
	unsigned long width = description->width, height = description->height;
	unsigned char header[18];
	unsigned char *pixels;
	char path[512];
	FILE *file;

	if (!directory || type != GPU_TEXTURE_2D)
		return;
	pixels = malloc(width * height * 4);
	if (!pixels)
		return;
	if (!gpu_texture_read(texture, pixels, (uint32_t)(width * height * 4)))
	{
		free(pixels);
		return;
	}
	snprintf(path, sizeof(path), "%s/tex%05lu_fmt%02x_%lux%lu.tga", directory, dump_index++,
		(unsigned)description->format, width, height);
	file = fopen(path, "wb");
	if (file)
	{
		memset(header, 0, sizeof(header));
		header[2] = 2;
		header[12] = (unsigned char)width; header[13] = (unsigned char)(width >> 8);
		header[14] = (unsigned char)height; header[15] = (unsigned char)(height >> 8);
		header[16] = 32; header[17] = 0x28;
		fwrite(header, 1, sizeof(header), file);
		fwrite(pixels, 4, width * height, file);
		fclose(file);
	}
	free(pixels);
}

/* ---------- Custom Edition channel orders

Halo PC keeps what some textures hold in other channels than the game reads
it from (enum custom_edition_channel_order): a model shader's multipurpose
masks, and a HUD meter's shape and fill order. The Custom Edition map
loading says which texels hold which order as they arrive
(port/linux/game/custom_edition_bitmaps.c), and textures made of them are
sampled with each channel taken from where Halo PC keeps it. (port: gpu.h
has no sampling swizzle, so such textures are decoded to BGRA and their
channels moved on the CPU as they upload: see channels_reorder.) Addresses stay listed until other texels
arrive there, which the loading also says, or the map goes; the game and the
renderer share a thread. */

/* for each order, the channel (red, green, blue, alpha) of the texels each
channel is sampled from */
static const unsigned char custom_edition_channel_sources[NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS][4] =
{
	{ 0, 1, 2, 3 },
	/* specular, self-illumination, color change and the auxiliary mask */
	{ 2, 1, 3, 0 },
	/* the fill order in color, the shape in alpha */
	{ 3, 3, 3, 0 },
};

struct custom_edition_texels
{
	unsigned long address;
	unsigned char channel_order;
};

static struct custom_edition_texels *custom_edition_texels;
static unsigned long custom_edition_texel_count;
static unsigned long custom_edition_texel_capacity;

/* the order of the texels at address */
static unsigned char custom_edition_texels_order(unsigned long address)
{
	unsigned long index;

	for (index = 0; index < custom_edition_texel_count; index++)
	{
		if (custom_edition_texels[index].address == address)
			return custom_edition_texels[index].channel_order;
	}
	return _custom_edition_channels_xbox;
}

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	unsigned long address = (unsigned long)texels;
	unsigned long index;

	if (channel_order >= NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS)
		channel_order = _custom_edition_channels_xbox;
	for (index = 0; index < custom_edition_texel_count && custom_edition_texels[index].address != address; index++)
	{
	}
	if (index < custom_edition_texel_count)
	{
		if (channel_order == _custom_edition_channels_xbox)
			custom_edition_texels[index] = custom_edition_texels[--custom_edition_texel_count];
		else
			custom_edition_texels[index].channel_order = channel_order;
	}
	else if (channel_order != _custom_edition_channels_xbox)
	{
		if (custom_edition_texel_count == custom_edition_texel_capacity)
		{
			unsigned long capacity = custom_edition_texel_capacity ? custom_edition_texel_capacity * 2 : 64;
			struct custom_edition_texels *grown = realloc(custom_edition_texels, capacity * sizeof(*grown));

			if (!grown)
			{
				platform_log("no memory to list the texels at %08lx: they are sampled in Halo PC's channel order",
					address);
				return;
			}
			custom_edition_texels = grown;
			custom_edition_texel_capacity = capacity;
		}
		custom_edition_texels[custom_edition_texel_count].address = address;
		custom_edition_texels[custom_edition_texel_count].channel_order = channel_order;
		custom_edition_texel_count++;
	}
}

void halo_custom_edition_texels_forget(void)
{
	free(custom_edition_texels);
	custom_edition_texels = NULL;
	custom_edition_texel_count = 0;
	custom_edition_texel_capacity = 0;
}

/* moves each texel's channels to where the game samples them (port: in place
of upstream's GL_TEXTURE_SWIZZLE_*); texels are 32-bit ARGB words */
static void channels_reorder(unsigned long *texels, unsigned long count, unsigned char channel_order)
{
	const unsigned char *sources = custom_edition_channel_sources[channel_order];
	/* each channel's shift in an ARGB word, by red, green, blue, alpha */
	static const unsigned char shifts[4] = { 16, 8, 0, 24 };
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		unsigned long texel = texels[index], reordered = 0, channel;

		for (channel = 0; channel < 4; channel++)
			reordered |= ((texel >> shifts[sources[channel]]) & 0xff) << shifts[channel];
		texels[index] = reordered;
	}
}

/* decode: the texture is BGRA8 although its texels are DXT (no S3TC, or
texels in another channel order) */
static void upload(gpu_texture texture, uint32_t type, const struct xgpu_texture_description *description,
	const unsigned char *base, const D3DCOLOR *palette, BOOL decode, unsigned char channel_order)
{
	struct format_information information = format_information(description->format);
	unsigned long face_count = description->cube_map ? 6 : 1;
	unsigned long face_size = xgpu_texture_face_size(description);
	unsigned long largest = description->width * description->height * description->depth;
	BOOL decode_compressed = description->compressed && decode;
	unsigned long *converted = description->compressed && !decode_compressed ? NULL :
		malloc(largest * sizeof(unsigned long));
	unsigned long face, level;

	if (!converted && !(description->compressed && !decode_compressed))
	{
		platform_log("textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
			description->width, description->height, description->depth);
		return;
	}
	for (face = 0; face < face_count; face++)
	{
		for (level = 0; level < description->levels; level++)
		{
			const unsigned char *source = base + face * face_size + xgpu_texture_level_offset(description, level);
			unsigned long width = level_dimension(description->width, level);
			unsigned long height = level_dimension(description->height, level);
			unsigned long depth = level_dimension(description->depth, level);

			if (description->compressed && !decode_compressed)
			{
				gpu_texture_upload(texture, (uint32_t)face, (uint32_t)level, source, (uint32_t)level_bytes(description, level));
				continue;
			}
			if (decode_compressed)
				dxt_decode_level(information.kind, source, width, height, depth, converted);
			else if (!decode_level(description, level, source, palette, converted))
			{
				platform_log("textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
					description->width, description->height, description->depth);
				free(converted);
				return;
			}
			if (channel_order != _custom_edition_channels_xbox)
				channels_reorder(converted, width * height * depth, channel_order);
			gpu_texture_upload(texture, (uint32_t)face, (uint32_t)level, converted, (uint32_t)(width * height * depth * 4));
		}
	}
	free(converted);
	texture_dump(texture, type, description);
}

int xgpu_texture_mean_alpha(const DWORD *resource, float *alpha)
{
	struct xgpu_texture_description description;
	struct format_information information;
	const unsigned char *source;
	unsigned long *texels, count, index, sum = 0;

	if (!resource || !resource[1])
		return 0;
	xgpu_texture_describe(resource[3], resource[4], &description);
	information = format_information(description.format);
	count = description.width * description.height;
	if (description.cube_map || description.depth != 1 || count == 0 || count > 4096 ||
		((resource[3] & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) == 0x0b)
		return 0;
	source = (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(resource[1]);
	/* (whole 4x4 blocks) */
	texels = malloc(((description.width + 3) & ~3ul) * ((description.height + 3) & ~3ul) * sizeof(unsigned long));
	if (!texels)
		return 0;
	if (description.compressed)
		dxt_decode_level(information.kind, source, description.width, description.height, 1, texels);
	else
		decode_level(&description, 0, source, NULL, texels);
	for (index = 0; index < count; index++)
		sum += texels[index] >> 24;
	free(texels);
	*alpha = (float)sum / (255.0f * (float)count);
	return 1;
}

/* ---------- cache */

struct texture_entry
{
	struct texture_entry *next;
	DWORD data, format_word, size_word;
	unsigned long palette_hash;
	gpu_texture texture;
	uint32_t type;
	struct xgpu_texture_description description;
	unsigned long address, size;
	unsigned long generation;
	unsigned long last_used_frame;
	/* the high-res HUD texture drawn in its place (hud_hires.h), or -1 */
	long override;
	/* debug.texture_override_directory's texture drawn in its place
	(texture_override.h): whether the bitmap here may have one, the hash of
	its first level, and where the replacement cache has it
	(texture_override_cache.h) */
	int texture_override_eligible;
	uint64_t texture_override_hash;
	struct texture_override_reference texture_override_reference;
	/* the newest generation of its pages (memory_watch_generation) as of the
	memory watch serial read before it was found: the same while no watched
	page has been written since (0: never found) */
	unsigned long watched_serial, watched_generation;
	/* texture holds BGRA8 although the texels are DXT: the device has no
	S3TC, or the texels were once in a Custom Edition channel order */
	BOOL decoded;
	/* Menu registration can replace a handle without changing the bitmap. */
	BOOL result_valid;
	unsigned long result_menu_generation;
	gpu_texture result_texture;
	uint32_t result_type;
	struct xgpu_texture_description result_description;
	uint32_t gpu_format;
#ifdef HALO_IOS
	struct astc_store_key astc_key;
	BOOL astc_key_valid, astc_pending;
#endif
};

#define TEXTURE_BUCKET_COUNT 4096
#define TEXTURE_SWEEP_SECONDS 10
/* display.texture_idle_seconds goes up to 600: the most sweeps it can look back */
#define TEXTURE_SWEEP_COUNT 60
#define MAXIMUM_PALETTE_VARIANTS 8

static struct texture_entry *texture_buckets[TEXTURE_BUCKET_COUNT];

/* Draws mostly bind the textures the draws before them bound. A lookup of a
texture that is not palettized is remembered with the memory watch serial it
started at: while no watched page has been written since, and no texture
has been dropped, the same lookup finds the same current texture. */
#define RECENT_TEXTURE_COUNT 512

static struct
{
	DWORD data, format_word, size_word;
	struct texture_entry *entry;
	unsigned long watch_serial;
	unsigned long drop_serial;
} recent_textures[RECENT_TEXTURE_COUNT];
static unsigned long texture_drop_serial = 1;
static unsigned long texture_frame = 0;
/* the frame of each of the last sweeps, a sweep apart, and when the next
is due */
static unsigned long texture_sweep_frames[TEXTURE_SWEEP_COUNT];
static unsigned long texture_sweep_count = 0;
static Uint64 texture_next_sweep = 0;
/* A memory warning is noted by the host, on whatever thread it arrives; the
first begin_frame after it drops the textures not drawn since the frame a
second or two before, found from the frame each of the last two seconds began
at. */
#ifdef HALO_IOS
int host_memory_pressure_poll(void);
int host_memory_available_megabytes(void);
#endif
static int texture_trim_requested = 0;
static unsigned long texture_trim_frames[2];
static Uint64 texture_trim_next = 0;

/* (every bit of the three mixed into the top ones: textures are aligned,
and few sizes and formats are common) */
static unsigned long bucket_index(DWORD data, DWORD format_word, DWORD size_word)
{
	unsigned long hash = (unsigned long)data * 2654435761UL ^ (unsigned long)format_word * 2246822519UL ^
		(unsigned long)size_word * 3266489917UL;

	return ((hash & 0xffffffffUL) >> 20) % TEXTURE_BUCKET_COUNT;
}

/* palettized textures are cached per palette contents: the game rewrites
palettes freely, and often cycles a texture through a few of them */
static unsigned long palette_hash(const D3DCOLOR *palette)
{
	unsigned long hash = 2166136261UL, index;

	if (!palette)
		return 0;
	for (index = 0; index < 256; index++)
		hash = (hash ^ palette[index]) * 16777619UL;
	return hash ? hash : 1;
}

/* ---------- debug.texture_override_directory (texture_override.h) */

/* source/bitmaps/bitmaps.c: the palette the game draws every P8 bump map with */
extern D3DCOLOR global_vector_palette[256];

/* ---------- the replacements' memory (texture_override_cache.h) */

#ifdef HALO_IOS
/* port/ios/host (guest_host.h): the device's memory, in MB (0: not known);
host_memory_available_megabytes is declared above */
unsigned int host_memory_physical_megabytes(void);
#endif

static double replacement_megabytes(uint64_t bytes)
{
	return (double)bytes / (1024.0 * 1024.0);
}

/* makes the replacement cache with display.upscaled_textures_memory's budget,
or one from the device's memory (texture_replacement_default_budget) */
static void texture_replacements_start(void)
{
	long megabytes = config_integer("display.upscaled_textures_memory");
	uint64_t available = 0, physical = 0, budget;

	if (texture_override_cache())
		return;
#ifdef HALO_IOS
	{
		int left = host_memory_available_megabytes();

		available = left > 0 ? (uint64_t)left << 20 : 0;
	}
	physical = (uint64_t)host_memory_physical_megabytes() << 20;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
	{
		long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);

		if (pages > 0 && page > 0)
			physical = (uint64_t)pages * (uint64_t)page;
	}
#endif
	budget = megabytes > 0 ? (uint64_t)megabytes << 20 : texture_replacement_default_budget(available, physical);
	texture_override_cache_start(budget);
	if (megabytes > 0)
		platform_log("texture replacements: %.0f MB of memory at most (display.upscaled_textures_memory)",
			replacement_megabytes(budget));
	else
		platform_log("texture replacements: %.0f MB of memory at most (%.0f MB left to the game, %.0f MB in the device)",
			replacement_megabytes(budget), replacement_megabytes(available), replacement_megabytes(physical));
}

/* "texture replacements, WHEN: ...", in the manner of texture_cache_debug.c's line */
static void texture_replacements_log(const char *when)
{
	const struct texture_replacement_cache *cache = texture_override_cache();

	if (!cache)
		return;
	platform_log("texture replacements, %s: %.1f of %.1f MB in use by %lu textures, %.1f MB at most; %lu read, "
		"%lu read again, %lu evictions, %lu without room", when, replacement_megabytes(cache->bytes),
		replacement_megabytes(cache->budget), cache->resident, replacement_megabytes(cache->statistics.peak_bytes),
		cache->statistics.loads, cache->statistics.reloads, cache->statistics.evictions, cache->statistics.refusals);
}

/* ---------- upscaled textures, per level (texture_upscale_state.h) */
/* port_config.c: the setting's value with nobody's choice over it (its default and the device's profile) */
int config_default(const char *name, char *text, size_t size);
#ifdef HALO_IOS
/* port/ios/host/host_texture_upscale.m (guest_host.h) */
int host_texture_upscale_level_ready(void);
void host_texture_cache_directory(char *text, unsigned long size);
#endif
/* where the upscaled textures' records are: on Apple devices the host's cache folder (Library/Caches/texture-cache,
which the system may purge; port/ios/host/host_texture_upscale.m), elsewhere texture-cache/ under the data root */
static const char *texture_record_directory(void)
{
#ifdef HALO_IOS
	static char directory[1024];

	if (!directory[0])
		host_texture_cache_directory(directory, sizeof(directory));
	if (directory[0])
		return directory;
#endif
	return TEXTURE_RECORD_DIRECTORY;
}
static struct texture_upscale_state texture_upscale;
/* the upscaled textures' cache (texture_record.h) is this level's first set
when nonzero: the records its manifest lists */
static size_t texture_records_listed;

/* the two sets' names in the comparison's readout and log lines
(debug.texture_override_name, debug.texture_compare_name) */
static const char *texture_set_name(int second)
{
	const char *name = config_string(second ? "debug.texture_compare_name" : "debug.texture_override_name");

	return *name ? name : second ? "SET B" : "SET A";
}

/* "Textures: NAME", and what stands in where the second set has no file */
static const char *texture_compare_label(void)
{
	static char label[160];

	if (texture_upscale.higher && texture_upscale.fallback)
		snprintf(label, sizeof(label), "Textures: %s (%s where it has none)", texture_set_name(1), texture_set_name(0));
	else
		snprintf(label, sizeof(label), "Textures: %s", texture_set_name(texture_upscale.higher));
	return label;
}

/* port: called from cache_files.c when a map has loaded, before its textures upload; the switch is read again
   from config.toml here (the Settings app may have changed it), so it takes effect at a level load only. The
   replacements the level before drew stay until the new one has shown which of them it draws
   (texture_replacement_cache.h) */
void texture_upscale_map_loaded(void)
{
	const char *override = config_string("debug.texture_override_directory");
	const char *compare = config_string("debug.texture_compare_directory");
	uint32_t format = device_capabilities.upscaled_format == GPU_FORMAT_BC7 ? TEXTURE_RECORD_BC7 :
		device_capabilities.upscaled_format == GPU_FORMAT_ASTC4X4 ? TEXTURE_RECORD_ASTC4X4 : 0;
	static int source = -1;
	int now;

	char profile[16] = "true";
	int switch_on;

	/* the player's switch turns them off, never on where the device's profile has them off (rule 1 of the
	on-device design: the resolved value is the profile's AND the player's) */
	config_default("display.upscaled_textures", profile, sizeof(profile));
	switch_on = config_reload_boolean("display.upscaled_textures") && !strcmp(profile, "true");
#ifdef HALO_IOS
	/* a level plays upscaled only if every texture its policy upscales is made: never some of each */
	if (switch_on && !*override && !host_texture_upscale_level_ready())
		texture_upscale_state_request_original(&texture_upscale);
#endif
	texture_upscale_state_map_loaded(&texture_upscale, switch_on);
	/* debug.texture_override_directory wins over the cache, whose manifest is read again at each level load */
	texture_records_listed = 0;
	if (*override || !texture_upscale_state_enabled(&texture_upscale) || !format)
		texture_override_cache_directories(override, compare);
	else
		texture_records_listed = texture_override_cache_records(texture_record_directory(), format, compare);
	texture_upscale_state_compare(&texture_upscale, (*override || texture_records_listed) && *compare);
	texture_replacements_log("at the level's end");
	/* (a new source of the first set: none of the old one's replacements stay) */
	now = *override ? 1 : texture_records_listed ? 2 : 0;
	if (source >= 0 && now != source)
		texture_override_cache_map_loaded(0);
	source = now;
	texture_override_cache_map_loaded(texture_upscale_state_enabled(&texture_upscale));
	if (texture_records_listed)
		platform_log("texture cache: %lu upscaled textures listed in %s, %s", (unsigned long)texture_records_listed,
			texture_record_directory(), format == TEXTURE_RECORD_BC7 ? "BC7" : "ASTC 4x4");
	if (texture_upscale.compare)
		platform_log("texture comparison: %s; %s is read as it is drawn once selected", texture_compare_label(),
			texture_set_name(1));
	if (config_boolean("debug.texture_log"))
		platform_log("textures: upscaled textures %s for this level",
			texture_upscale_state_enabled(&texture_upscale) ? "on" : "off");
}

/* source/render/render_debug.c: text the frame's debug pass draws */
void render_debug_string(unsigned char immediate, const char *string);
/* xbox_kernel.c */
double halo_frame_trace_milliseconds(void);
/* when the toggle was last pressed, for its readout */
static double texture_upscale_toggled_milliseconds = -1.0;

/* debug.upscaled_textures_toggle (xinput_sdl.c: hold Back and press the D-pad
up): the level's originals drawn in place of its upscaled textures, or back,
from the next draw; display.upscaled_textures is not changed. With
debug.texture_compare_directory, the two sets instead: both stay in memory
while the budget has room, so a switch back reads nothing */
void halo_texture_upscale_toggle(void)
{
	int toggled = texture_upscale_state_toggle(&texture_upscale);

	if (texture_upscale.compare)
	{
		unsigned long reads;
		double milliseconds = texture_override_cache_take_read_milliseconds(&reads);

		if (toggled)
			texture_override_cache_select(texture_upscale.higher);
		platform_log("%s; since the last switch %lu textures read in %.0f ms", texture_compare_label(), reads,
			milliseconds);
		texture_replacements_log("at the switch");
		return;
	}

	texture_upscale_toggled_milliseconds = halo_frame_trace_milliseconds();
	platform_log("textures: %s%s", texture_upscale_state_show_original(&texture_upscale) || !toggled ?
		"original" : "upscaled", toggled ? "" : " (none for this level)");
}

/* which textures are drawn, on screen for two seconds after the toggle (all
the time in a comparison of two sets); called as each frame begins
(render_interpolation.c), like the render scale's readout */
void halo_texture_upscale_overlay(void)
{
	if (texture_upscale.compare)
	{
		render_debug_string(0, texture_compare_label());
		return;
	}
	if (texture_upscale_toggled_milliseconds < 0.0 ||
		halo_frame_trace_milliseconds() - texture_upscale_toggled_milliseconds > 2000.0)
		return;
	if (!texture_upscale_state_enabled(&texture_upscale))
		render_debug_string(0, "Textures: original (none for this level)");
	else if (texture_upscale_state_show_original(&texture_upscale))
		render_debug_string(0, "Textures: original");
	else if (texture_upscale.replaced)
		render_debug_string(0, "Textures: upscaled");
	else
		render_debug_string(0, "Textures: upscaled (none loaded yet)");
}

/* an entry's replacement from the selected set and its level count, or 0 to
draw the entry's own texture: there is no file, or the replacement was
dropped and there is no room to read it again (texture_override_cache.h).
Found by the saved hash, never by rehashing memory that the game's cache may
have reused since */
static gpu_texture texture_entry_replacement(struct texture_entry *entry, unsigned long *levels)
{
	int fallback;
	gpu_texture texture;

	texture_replacements_start();
	texture = texture_override_cache_find(&entry->texture_override_reference, entry->texture_override_hash,
		entry->description.width, entry->description.height, levels, &fallback);
	if (fallback > texture_upscale.fallback)
		texture_upscale.fallback = fallback;
	return texture;
}

/* whether the entry is drawn as its replacement now (texture_entry_result) */
static BOOL texture_entry_replaced(struct texture_entry *entry)
{
	unsigned long levels;

	return entry->texture_override_eligible && texture_upscale_state_enabled(&texture_upscale) &&
		!texture_upscale_state_show_original(&texture_upscale) && texture_entry_replacement(entry, &levels) != 0;
}

/* after an upload: the override standing for the pixels now at the entry's
address (the game's texture cache reuses that memory for other bitmaps). 2D
textures only: not linear ones, and P8 ones only with the bump maps' palette */
static void texture_override_apply(struct texture_entry *entry, const D3DCOLOR *palette)
{
	static int logging = -1;
	static unsigned long vector_palette_hash;
	const char *directory = *config_string("debug.texture_override_directory") ?
		config_string("debug.texture_override_directory") : texture_records_listed ? texture_record_directory() : "";
	const struct xgpu_texture_description *description = &entry->description;
	unsigned long level0_size, levels;
	uint64_t hash;
	BOOL applied = FALSE;

	if (!texture_upscale_state_enabled(&texture_upscale))
		directory = "";   /* this level plays original; logging still shows each hash */
	if (logging < 0)
		logging = config_boolean("debug.texture_log");
	if ((!*directory && !logging) || entry->type != GPU_TEXTURE_2D || description->linear)
		return;
	if (description->format == 0x0b)
	{
		if (!vector_palette_hash)
			vector_palette_hash = palette_hash(global_vector_palette);
		if (palette_hash(palette) != vector_palette_hash)
			return;
	}
	level0_size = description->levels > 1 ? xgpu_texture_level_offset(description, 1) :
		xgpu_texture_face_size(description);
	hash = texture_override_hash((const unsigned char *)entry->address, level0_size);
	if (*directory)
	{
		entry->texture_override_hash = hash;
		entry->texture_override_eligible = 1;
		memset(&entry->texture_override_reference, 0, sizeof(entry->texture_override_reference));
		applied = texture_entry_replacement(entry, &levels) != 0;
		if (applied)
			texture_upscale_state_replaced(&texture_upscale);
	}
	if (logging)
		platform_log("texture override %016llx at %08lx fmt %02lx %lux%lu: %s", (unsigned long long)hash,
			(unsigned long)entry->data, (unsigned long)description->format, description->width, description->height,
			applied ? "applied" : "none");
}

/* A format change replaces the object, because Metal fixes its format at creation. */
static BOOL texture_entry_format(struct texture_entry *entry, uint32_t format)
{
	struct gpu_texture_description description = { 0 };
	gpu_texture replacement;

	if (entry->gpu_format == format && entry->texture)
		return TRUE;
	description.type = (uint8_t)entry->type;
	description.format = format;
	description.usage = GPU_USAGE_UPLOAD;
	description.width = entry->description.width;
	description.height = entry->description.height;
	description.depth = entry->description.depth;
	description.levels = entry->description.levels;
	replacement = gpu_texture_create(&description);
	if (!replacement)
		return FALSE;
	gpu_texture_destroy(entry->texture);
	entry->texture = replacement;
	/* (a cached result may still name the destroyed handle) */
	entry->result_valid = FALSE;
	entry->gpu_format = format;
	entry->decoded = format == GPU_FORMAT_BGRA8 && entry->description.compressed;
	return TRUE;
}

/* Stage every face and mip before changing formats. One unsupported block makes
 * the entire texture use today's BGRA8 path, including its smallest mips. */
static unsigned char *texture_astc_prepare(const struct texture_entry *entry)
{
	const struct xgpu_texture_description *description = &entry->description;
	struct format_information information = format_information(description->format);
	enum astc_dxt_format format = information.kind == _texel_dxt1 ? ASTC_DXT1 :
		information.kind == _texel_dxt3 ? ASTC_DXT3 : ASTC_DXT5;
	unsigned long faces = description->cube_map ? 6 : 1;
	unsigned long source_face = xgpu_texture_face_size(description);
	unsigned long face_bytes = 0, face, level, offset = 0;
	unsigned char *bytes;

	for (level = 0; level < description->levels; level++)
		face_bytes += ((level_dimension(description->width, level) + 3) / 4) *
			((level_dimension(description->height, level) + 3) / 4) * 16;
	bytes = malloc(face_bytes * faces);
	if (!bytes)
		return NULL;
	for (face = 0; face < faces; face++)
	{
		for (level = 0; level < description->levels; level++)
		{
			unsigned long width = level_dimension(description->width, level);
			unsigned long height = level_dimension(description->height, level);
			const unsigned char *source = (const unsigned char *)entry->address + face * source_face +
				xgpu_texture_level_offset(description, level);

			if (!astc_rewrite_level(format, source, width, height, bytes + offset))
			{
				free(bytes);
				return NULL;
			}
			offset += ((width + 3) / 4) * ((height + 3) / 4) * 16;
		}
	}
	return bytes;
}

#ifdef HALO_IOS
/* Only scalars and byte buffers cross the ILP32 guest/LP64 host boundary. */
extern int host_astc_texture_key(const void *source, unsigned int format,
	unsigned int width, unsigned int height, unsigned int levels, unsigned int faces,
	unsigned int source_face_bytes, void *key);
extern int host_astc_texture_read(const void *key, void *bytes, unsigned int size);
extern unsigned long long host_astc_texture_generation(void);
static BOOL texture_astc_key(struct texture_entry *entry)
{
	const struct xgpu_texture_description *description = &entry->description;
	struct format_information information = format_information(description->format);
	unsigned int format = information.kind == _texel_dxt1 ? ASTC_DXT1 :
		information.kind == _texel_dxt3 ? ASTC_DXT3 : ASTC_DXT5;
	if (!entry->astc_key_valid)
		entry->astc_key_valid = host_astc_texture_key((const void *)entry->address, format,
			description->width, description->height, description->levels,
			description->cube_map ? 6 : 1, xgpu_texture_face_size(description), &entry->astc_key);
	return entry->astc_key_valid;
}

static size_t texture_astc_bytes(const struct texture_entry *entry)
{
	size_t bytes = 0;
	for (unsigned long level = 0; level < entry->description.levels; level++)
		bytes += ((level_dimension(entry->description.width, level) + 3) / 4) *
			((level_dimension(entry->description.height, level) + 3) / 4) * 16;
	return bytes * (entry->description.cube_map ? 6 : 1);
}

static unsigned char *texture_astc_stored(struct texture_entry *entry)
{
	size_t size = texture_astc_bytes(entry);
	unsigned char *bytes;
	if (!texture_astc_key(entry))
		return NULL;
	bytes = malloc(size);
	if (bytes && !host_astc_texture_read(&entry->astc_key, bytes, size))
	{
		free(bytes);
		bytes = NULL;
	}
	if (bytes && config_boolean("debug.texture_log"))
		platform_log("texture ASTC store hit %08lx frame %lu bytes %lu", (unsigned long)entry->data,
			texture_frame, (unsigned long)size);
	return bytes;
}
#endif

static void texture_astc_upload(const struct texture_entry *entry, const unsigned char *bytes)
{
	unsigned long face, level, offset = 0;
	for (face = 0; face < (entry->description.cube_map ? 6UL : 1UL); face++)
	{
		for (level = 0; level < entry->description.levels; level++)
		{
			unsigned long size = ((level_dimension(entry->description.width, level) + 3) / 4) *
				((level_dimension(entry->description.height, level) + 3) / 4) * 16;
			gpu_texture_upload(entry->texture, face, level, bytes + offset, size);
			offset += size;
		}
	}
}

/* an entry's texture, type and description: its high-res HUD texture's, if
it has one, with the bitmap's own size (which its coordinates are in) */
static gpu_texture texture_entry_result_uncached(struct texture_entry *entry, uint32_t *type,
	struct xgpu_texture_description *description)
{
	*type = entry->type;
	*description = entry->description;
	/* (a menu's bitmap: menu_files.h) */
	{
		unsigned long levels;
		gpu_texture art = menu_art_texture(entry->data, &levels);

		if (art)
		{
			*type = GPU_TEXTURE_2D;
			description->levels = levels;
			description->hires = TRUE;
			return art;
		}
	}
	if (entry->override >= 0)
	{
		gpu_texture texture = hud_hires_override_texture(entry->override, &description->levels);

		if (texture)
		{
			*type = GPU_TEXTURE_2D;
			description->hires = TRUE;
			description->hires_coverage = hud_hires_override_coverage(entry->override);
			description->hires_point_threshold = hud_hires_override_point_threshold(entry->override);
			return texture;
		}
	}
	return entry->texture;
}

static gpu_texture texture_entry_result(struct texture_entry *entry, uint32_t *type,
	struct xgpu_texture_description *description)
{
	unsigned long menu_generation;
	/* The atlas uploads newly rasterized glyphs when bound, even on a cache hit. */
	gpu_texture atlas = text_hires_atlas_texture(entry->data);

	if (atlas)
	{
		*type = GPU_TEXTURE_2D;
		*description = entry->description;
		description->levels = 1;
		return atlas;
	}
	menu_generation = menu_art_generation();
	if (!entry->result_valid || entry->result_menu_generation != menu_generation)
	{
		entry->result_texture = texture_entry_result_uncached(entry, &entry->result_type,
			&entry->result_description);
		entry->result_menu_generation = menu_generation;
		entry->result_valid = TRUE;
	}
	*type = entry->result_type;
	*description = entry->result_description;
	/* The replacement is looked up at every bind, cached result or not: the
	lookup marks it drawn, which keeps texture_replacement_cache.h from
	dropping it, and a dropped one is read again here when there is room.
	(debug.upscaled_textures_toggle: the original, entry->texture, is always
	uploaded too, so the toggle only picks which one is drawn.) */
	if (entry->result_texture == entry->texture && entry->texture_override_eligible &&
		texture_upscale_state_enabled(&texture_upscale) && !texture_upscale_state_show_original(&texture_upscale))
	{
		unsigned long levels;
		gpu_texture replacement = texture_entry_replacement(entry, &levels);

		/* (debug.texture_override_directory: its own levels, sampled as the
		game samples the texture; never hires. Dropped for room, with none to
		read it again: the bitmap as the map has it) */
		if (replacement)
		{
#ifdef HALO_IOS
			/* (counted at every bind, cached result or not) */
			extern void benchmark_graphics_texture_replacement_selected(void);
			benchmark_graphics_texture_replacement_selected();
#endif
			*type = GPU_TEXTURE_2D;
			description->levels = levels;
			return replacement;
		}
	}
	return entry->result_texture;
}

gpu_texture xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, uint32_t *type,
	struct xgpu_texture_description *description)
{
	DWORD data = resource[1], format_word = resource[3], size_word = resource[4];
	struct texture_entry **bucket = &texture_buckets[bucket_index(data, format_word, size_word)];
	struct texture_entry *entry;
	unsigned long generation;
	BOOL palettized = ((format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) == 0x0b;
	unsigned long hash = palettized ? palette_hash(palette) : 0;

	struct texture_entry *oldest_variant = NULL;
	unsigned long variant_count = 0;
	static int no_cache = -1;
	unsigned long recent = bucket_index(data, format_word, size_word) % RECENT_TEXTURE_COUNT;
	unsigned long watch_serial = memory_watch_serial();

	/* (a texture that is not palettized has the one entry, until one is
	dropped: remembered, a page written since only means checking it) */
	entry = NULL;
	if (!palettized && recent_textures[recent].entry && recent_textures[recent].data == data &&
		recent_textures[recent].format_word == format_word && recent_textures[recent].size_word == size_word &&
		recent_textures[recent].drop_serial == texture_drop_serial)
	{
		entry = recent_textures[recent].entry;
		if (recent_textures[recent].watch_serial == watch_serial)
		{
			entry->last_used_frame = texture_frame;
			return texture_entry_result(entry, type, description);
		}
	}

	if (!entry)
	{
		for (entry = *bucket; entry; entry = entry->next)
		{
			if (entry->data == data && entry->format_word == format_word && entry->size_word == size_word)
			{
				if (entry->palette_hash == hash)
					break;
				variant_count++;
				if (!oldest_variant || entry->last_used_frame < oldest_variant->last_used_frame)
					oldest_variant = entry;
			}
		}
	}
	if (!entry && variant_count >= MAXIMUM_PALETTE_VARIANTS)
	{
		/* a palette that keeps changing reuses the stalest copy */
		entry = oldest_variant;
		entry->palette_hash = hash;
		entry->generation = 0;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
		{
			xgpu_texture_describe(format_word, size_word, description);
			*type = GPU_TEXTURE_2D;
			return 0;
		}
		entry->data = data;
		entry->format_word = format_word;
		entry->size_word = size_word;
		entry->palette_hash = hash;
		xgpu_texture_describe(format_word, size_word, &entry->description);
		entry->type = entry->description.cube_map ? GPU_TEXTURE_CUBE :
			entry->description.depth > 1 ? GPU_TEXTURE_3D : GPU_TEXTURE_2D;
		entry->address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(data);
		/* (a size beyond D3DDevice_GetDeviceCaps' is never uploaded: its
		byte counts would not fit in 32 bits) */
		entry->size = texture_size_supported(&entry->description) ?
			xgpu_texture_face_size(&entry->description) * (entry->description.cube_map ? 6 : 1) : 0;
		if (!entry->size)
		{
			platform_log("textures: a %lux%lux%lu texture is larger than the device takes; it is not drawn",
				entry->description.width, entry->description.height, entry->description.depth);
			VALIDATION_NOTE(FRONT_TEXTURE_NOT_DRAWN, 2, entry->description.width << 16 | entry->description.height);
		}
		entry->generation = 0;
		{
			struct gpu_texture_description texture = { 0 };
			struct format_information information = format_information(entry->description.format);

			entry->decoded = entry->description.compressed && !device_capabilities.s3tc;
			texture.type = (uint8_t)entry->type;
			texture.format = !entry->description.compressed || entry->decoded ? GPU_FORMAT_BGRA8 :
				information.kind == _texel_dxt1 ? GPU_FORMAT_BC1 :
				information.kind == _texel_dxt3 ? GPU_FORMAT_BC2 : GPU_FORMAT_BC3;
			texture.usage = GPU_USAGE_UPLOAD;
			texture.width = (uint32_t)entry->description.width;
			texture.height = (uint32_t)entry->description.height;
			texture.depth = (uint32_t)entry->description.depth;
			texture.levels = (uint32_t)entry->description.levels;
			entry->gpu_format = texture.format;
			entry->texture = gpu_texture_create(&texture);
		}
		entry->override = -1;
		entry->next = *bucket;
		*bucket = entry;
	}

	if (no_cache < 0)
		no_cache = config_boolean("debug.texture_no_cache");
	/* (its pages' newest generation, found again only once a watched page
	has been written: a large texture's pages, scanned for every draw that
	bound it, were much of a frame with many objects) */
	if (entry->watched_serial && entry->watched_serial == watch_serial)
		generation = entry->watched_generation;
	else
	{
		generation = memory_watch_generation(entry->address, entry->size);
		entry->watched_serial = watch_serial;
		entry->watched_generation = generation;
	}
	if (!entry->generation || generation > entry->generation || no_cache)
	{
		entry->result_valid = FALSE;
#ifdef HALO_IOS
		entry->astc_key_valid = entry->astc_pending = FALSE;
#endif
		/* A rewritten guest page gets a fresh content key. The old worker result
		 * cannot match changed bytes, even when the address is reused. */
		/* protect first, so a write racing with the upload is noticed */
		memory_watch_protect(entry->address, entry->size);
		entry->generation = memory_watch_generation(entry->address, entry->size);
		if (!entry->generation)
			entry->generation = 1;
		/* (which bitmap is here may have changed with the pixels) */
		entry->override = -1;
		entry->texture_override_eligible = 0;
		if (entry->size && !palettized && !entry->description.cube_map && entry->description.depth == 1)
		{
			unsigned long levels;

			entry->override = hud_hires_override_find(entry->address, entry->description.width,
				entry->description.height, entry->description.levels > 1 ?
				xgpu_texture_level_offset(&entry->description, 1) : xgpu_texture_face_size(&entry->description));
			if (entry->override >= 0 && !hud_hires_override_texture(entry->override, &levels))
				entry->override = -1;
		}
		if (entry->override < 0 && entry->size && platform_is_contiguous((void *)entry->address) &&
			platform_is_contiguous((void *)(entry->address + entry->size - 1)))
		{
			unsigned char channel_order = custom_edition_texels_order(entry->address);

			unsigned char *astc = NULL;
			uint32_t result_type;
			struct xgpu_texture_description result_description;
			BOOL eligible;

			texture_override_apply(entry, palette);
			eligible = device_capabilities.astc && !device_capabilities.s3tc && entry->description.compressed &&
				entry->type != GPU_TEXTURE_3D &&
				!(resource[0] & D3DCOMMON_D3DCREATED) && channel_order == _custom_edition_channels_xbox &&
				!*config_string("debug.texture_dump_directory") &&
				!text_hires_atlas_texture(entry->data) &&
				texture_entry_result_uncached(entry, &result_type, &result_description) == entry->texture &&
				!texture_entry_replaced(entry);
			if (eligible)
			{
#ifdef HALO_IOS
				astc = texture_astc_stored(entry);
#endif
				if (!astc)
					astc = texture_astc_prepare(entry);
#ifdef HALO_IOS
				entry->astc_pending = !astc && entry->astc_key_valid;
#endif
			}
			/* Unsupported alpha, replacements, allocation failure, and runtime writes
			 * all retain the existing decode. BC devices keep their normal path. */
			if (astc && !texture_entry_format(entry, GPU_FORMAT_ASTC4X4))
			{
				free(astc);
				astc = NULL;
			}
			if (!astc && !texture_entry_format(entry,
				entry->description.compressed && device_capabilities.s3tc &&
				channel_order == _custom_edition_channels_xbox ? entry->gpu_format : GPU_FORMAT_BGRA8))
			{
				entry->generation = 0; /* retry allocation on the next lookup */
				return 0;
			}
			if (config_boolean("debug.texture_log"))
			{
				const unsigned char *bytes = (const unsigned char *)entry->address;
				unsigned long index, ones = 0, zeros = 0;

				for (index = 0; index < entry->size; index++)
				{
					ones += bytes[index] == 0xff;
					zeros += bytes[index] == 0;
				}
				platform_log("texture upload %08lx fmt %02lx %lux%lu size %lu gen %lu ff %lu%% 00 %lu%% gpu format %u",
					(unsigned long)data, (unsigned long)entry->description.format, entry->description.width,
					entry->description.height, entry->size, entry->generation,
					ones * 100 / entry->size, zeros * 100 / entry->size, (unsigned)entry->gpu_format);
			}
			if (astc)
				texture_astc_upload(entry, astc);
			else
				upload(entry->texture, entry->type, &entry->description, (const unsigned char *)entry->address, palette,
					entry->decoded, channel_order);
			free(astc);
		}
	}
	entry->last_used_frame = texture_frame;
	if (!palettized && !no_cache)
	{
		recent_textures[recent].data = data;
		recent_textures[recent].format_word = format_word;
		recent_textures[recent].size_word = size_word;
		recent_textures[recent].entry = entry;
		recent_textures[recent].watch_serial = watch_serial;
		recent_textures[recent].drop_serial = texture_drop_serial;
	}
	return texture_entry_result(entry, type, description);
}

/* how many sweeps back the frame is taken that a texture must have been
drawn since: display.texture_idle_seconds (a device profile's, or
debug.texture_idle_seconds when not negative) in sweeps, at least one */
static unsigned long texture_idle_sweeps(void)
{
	double seconds = config_real("debug.texture_idle_seconds");
	unsigned long sweeps;

	if (seconds < 0.0)
		seconds = config_real("display.texture_idle_seconds");
	sweeps = (unsigned long)(seconds / TEXTURE_SWEEP_SECONDS + 0.5);
	if (sweeps < 1)
		sweeps = 1;
	if (sweeps > TEXTURE_SWEEP_COUNT)
		sweeps = TEXTURE_SWEEP_COUNT;
	return sweeps;
}

/* Count actual mip block sizes, including partial blocks at tiny mips. */
static unsigned long texture_gpu_bytes(const struct texture_entry *entry)
{
	unsigned long bytes = 0;
	for (unsigned long level = 0; level < entry->description.levels; level++)
	{
		unsigned long width = level_dimension(entry->description.width, level);
		unsigned long height = level_dimension(entry->description.height, level);
		unsigned long depth = level_dimension(entry->description.depth, level);
		if (entry->gpu_format == GPU_FORMAT_ASTC4X4 || entry->gpu_format == GPU_FORMAT_BC2 ||
			entry->gpu_format == GPU_FORMAT_BC3)
			bytes += ((width + 3) / 4) * ((height + 3) / 4) * 16 * depth;
		else if (entry->gpu_format == GPU_FORMAT_BC1)
			bytes += ((width + 3) / 4) * ((height + 3) / 4) * 8 * depth;
		else
			bytes += width * height * depth * 4;
	}
	return bytes * (entry->description.cube_map ? 6 : 1);
}

#ifdef HALO_IOS
/* The worker publishes keys, never entry pointers. Search the live buckets again
 * at a frame boundary, and reject guest data rewritten since its upload. */
static void texture_astc_swap_ready(void)
{
	static uint64_t observed;
	uint64_t current = host_astc_texture_generation();
	if (current == observed)
		return;
	observed = current;
	for (unsigned long bucket = 0; bucket < TEXTURE_BUCKET_COUNT; bucket++)
		for (struct texture_entry *entry = texture_buckets[bucket]; entry; entry = entry->next)
		{
			unsigned char *bytes;
			uint32_t type;
			struct xgpu_texture_description description;
			if (!entry->astc_pending || !entry->astc_key_valid ||
				entry->gpu_format != GPU_FORMAT_BGRA8 ||
				memory_watch_generation(entry->address, entry->size) > entry->generation ||
				*config_string("debug.texture_dump_directory") ||
				text_hires_atlas_texture(entry->data) ||
				texture_entry_result_uncached(entry, &type, &description) != entry->texture ||
				texture_entry_replaced(entry))
				continue;
			bytes = texture_astc_stored(entry);
			if (!bytes)
				continue;
			if (texture_entry_format(entry, GPU_FORMAT_ASTC4X4))
			{
				texture_astc_upload(entry, bytes);
				entry->astc_pending = FALSE;
				if (config_boolean("debug.texture_log"))
					platform_log("texture ASTC swap %08lx frame %lu", (unsigned long)entry->data, texture_frame);
				/* A recent hit may bypass the normal upload check. */
				memset(recent_textures, 0, sizeof(recent_textures));
			}
			free(bytes);
		}
}
#endif

/* drops the textures last drawn in a frame before this one, and adds up the
bytes of the ones dropped to *bytes (when not NULL) */
static void texture_drop_before(unsigned long frame, unsigned long long *bytes)
{
	unsigned long index;

	for (index = 0; index < TEXTURE_BUCKET_COUNT; index++)
	{
		struct texture_entry **link = &texture_buckets[index];

		while (*link)
		{
			struct texture_entry *entry = *link;

			if (entry->last_used_frame < frame)
			{
				*link = entry->next;
				if (bytes)
					*bytes += texture_gpu_bytes(entry);
				gpu_texture_destroy(entry->texture);
				texture_drop_serial++;
				free(entry);
			}
			else
			{
				link = &entry->next;
			}
		}
	}
}

/* drops the textures not drawn in the last second or two, once asked to (a
memory warning), and logs what it freed and the memory left around it */
static void texture_trim_if_requested(void)
{
	unsigned long long bytes = 0;
	unsigned long count = texture_drop_serial;
	long before = -1, after = -1;

#ifdef HALO_IOS
	if (host_memory_pressure_poll())
		texture_trim_requested = 1;
#endif
	if (!texture_trim_requested)
		return;
	texture_trim_requested = 0;
	if (texture_override_cache())
	{
		/* (the replacements drop what the last two frames did not draw, and
		their budget halves) */
		uint64_t freed = texture_override_cache_memory_warning();

		platform_log("texture replacements: the system warned of low memory; %.1f MB dropped, %.1f MB at most "
			"from now on", replacement_megabytes(freed), replacement_megabytes(texture_override_cache()->budget));
	}
#ifdef HALO_IOS
	before = host_memory_available_megabytes();
#endif
	texture_drop_before(texture_trim_frames[0], &bytes);
#ifdef HALO_IOS
	after = host_memory_available_megabytes();
#endif
	platform_log("textures: memory pressure, dropped %lu textures not drawn in the last second or two, about %llu MB (%ld MB left before, %ld after; -1: unknown)",
		texture_drop_serial - count, bytes >> 20, before, after);
}

void xgpu_texture_cache_begin_frame(void)
{
	unsigned long index, idle_since, sweeps;
	unsigned long changed = hud_hires_settings_changed();
	Uint64 now;

	/* Refresh only map bitmaps belonging to the setting that changed. Keep
	 * decoded replacement art and world textures; rebuild native pixels on
	 * demand too, since an override may have skipped their first upload. */
	if (changed)
	{
		for (index = 0; index < TEXTURE_BUCKET_COUNT; index++)
		{
			struct texture_entry *entry;

			for (entry = texture_buckets[index]; entry; entry = entry->next)
				if (hud_hires_bitmap_affected(entry->address,
					(long)entry->description.width, (long)entry->description.height, changed))
					entry->generation = 0;
		}
		/* Recent hits bypass the generation check. Forget lookups, not textures. */
		memset(recent_textures, 0, sizeof(recent_textures));
	}
#ifdef HALO_IOS
	texture_astc_swap_ready();
#endif
	texture_frame++;
	if (texture_override_cache())
	{
		static long toggle_every = -1;
		uint64_t freed;
		unsigned long count = texture_override_cache_begin_frame(&freed);

		if (count)
			platform_log("texture replacements: %lu the level has not drawn dropped, %.1f MB", count,
				replacement_megabytes(freed));
		if (texture_frame % 300 == 0 && config_boolean("debug.texture_replacement_stats"))
			texture_replacements_log("now");
		/* debug.upscaled_textures_toggle_every: the toggle's press, for a run with no controller */
		if (toggle_every < 0)
			toggle_every = config_boolean("debug.upscaled_textures_toggle") ?
				config_integer("debug.upscaled_textures_toggle_every") : 0;
		if (toggle_every > 0 && texture_frame % (unsigned long)toggle_every == 0)
			halo_texture_upscale_toggle();
	}
	now = SDL_GetTicks();
	if (now >= texture_trim_next)
	{
		texture_trim_frames[0] = texture_trim_frames[1];
		texture_trim_frames[1] = texture_frame;
		texture_trim_next = now + 1000;
	}
	texture_trim_if_requested();
	if (now < texture_next_sweep)
		return;
	texture_next_sweep = now + TEXTURE_SWEEP_SECONDS * 1000;
	sweeps = texture_idle_sweeps();
	/* drop the textures not used since the sweep the idle time ago (none
	before that many sweeps, which find frame 0). The time is counted in
	seconds, not frames: 1800 frames, half a minute at 60 frames a second, is
	five seconds at 360, and a texture out of sight that long was converted
	and uploaded again when it came back into view */
	idle_since = texture_sweep_count < sweeps ? 0 :
		texture_sweep_frames[(texture_sweep_count - sweeps) % TEXTURE_SWEEP_COUNT];
	texture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT] = texture_frame;
	texture_sweep_count++;
	texture_drop_before(idle_since, NULL);
}
