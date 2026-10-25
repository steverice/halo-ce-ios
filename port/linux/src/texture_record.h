/*
TEXTURE_RECORD.H

An upscaled texture's record in the on-device cache (texture-cache/ under the
data root, port/runtime/texture_cache.h): the complete mip chain of the
replacement, in a GPU block format, ready to upload with no decode.
tools/texture_cache_write.py writes records from a Mac set; xbox_textures.c
reads them (texture_override_cache.c).

The file is <hash>-<width>x<height>.bin, named by the original texture's key
(its level-0 hash, 16 lowercase hex digits, and its size). All numbers are
little-endian:

	offset  size
	0       4    "VUTR"
	4       4    version, TEXTURE_RECORD_VERSION
	8       4    format: TEXTURE_RECORD_BC7 or TEXTURE_RECORD_ASTC4X4 (both linear UNORM)
	12      4    the original's width
	16      4    the original's height
	20      4    level 0's width: the original's times 1, 2, 4 or 8
	24      4    level 0's height, the same multiple
	28      4    levels, 1 to TEXTURE_RECORD_MAXIMUM_LEVELS, each half the one before (never below 1)
	32      8    the original's level-0 hash
	40      8    the recipe key (texture_recipe_key) it was made by
	48      8    checksum of every byte after the level table (texture_record_checksum)
	56      4    the header's size with the level table: 64 + 16 * levels
	60      4    0
	64      16 * levels: each level's offset from the file's start and length, 8 bytes each

then the levels, largest first, end to end with no gaps, each in 4x4 blocks of
16 bytes, rows of blocks top first (a level under 4 texels wide or high is one
block across or down). The file ends with the last level.
*/

#ifndef TEXTURE_RECORD_H
#define TEXTURE_RECORD_H

#include <stddef.h>
#include <stdint.h>

/* under the data root; port/runtime/texture_cache.h's TEXTURE_CACHE_DIRECTORY */
#define TEXTURE_RECORD_DIRECTORY "texture-cache"
#define TEXTURE_RECORD_MAGIC "VUTR"
#define TEXTURE_RECORD_VERSION 1
#define TEXTURE_RECORD_FIXED_SIZE 64
#define TEXTURE_RECORD_MAXIMUM_LEVELS 14
#define TEXTURE_RECORD_MAXIMUM_DIMENSION 8192

enum
{
	TEXTURE_RECORD_BC7 = 1,
	TEXTURE_RECORD_ASTC4X4 = 2,
};

struct texture_record_layout
{
	uint32_t format;
	uint32_t source_width, source_height;
	uint32_t width, height, levels;
	uint64_t hash, recipe_key, checksum;
	uint32_t header_size;
	uint64_t level_offset[TEXTURE_RECORD_MAXIMUM_LEVELS];
	uint64_t level_size[TEXTURE_RECORD_MAXIMUM_LEVELS];
	uint64_t payload_size; /* the bytes after the header: the levels */
};

static inline uint32_t texture_record_u32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static inline uint64_t texture_record_u64(const unsigned char *bytes)
{
	return (uint64_t)texture_record_u32(bytes) | ((uint64_t)texture_record_u32(bytes + 4) << 32);
}

/* the bytes of one level of a width x height texture in 4x4 blocks of 16 bytes */
static inline uint64_t texture_record_level_bytes(uint32_t width, uint32_t height)
{
	return (uint64_t)((width + 3) / 4) * ((height + 3) / 4) * 16;
}

static inline uint32_t texture_record_half(uint32_t size)
{
	return size > 1 ? size / 2 : 1;
}

/* The checksum: over the payload as little-endian 64-bit words w[0..n), the
sums a = w[0] + ... + w[n-1] and b = 1 w[0] + 2 w[1] + ... + n w[n-1], both
modulo 2^64, give a ^ (b << 32 | b >> 32). The payload is whole blocks, so a
multiple of 8 bytes. Not a cryptographic hash: it catches a short write, a
torn file and stray bytes, at memory speed (numpy computes it the same way in
tools/texture_cache_write.py). */
static inline uint64_t texture_record_checksum(const unsigned char *payload, uint64_t size)
{
	uint64_t a = 0, b = 0, index;

	for (index = 0; index < size / 8; index++)
	{
		uint64_t word = texture_record_u64(payload + index * 8);

		a += word;
		b += (index + 1) * word;
	}
	return a ^ (b << 32 | b >> 32);
}

/* NULL if the header (TEXTURE_RECORD_FIXED_SIZE bytes, then the level
table: at least texture_record_header_size's, from the first) describes a
record of file_size bytes that can stand for a width x height texture with
this hash in the format wanted (TEXTURE_RECORD_*), and fills layout; else
what is wrong. The checksum is checked apart (texture_record_check_payload) */
static inline uint32_t texture_record_header_size(const unsigned char *fixed)
{
	return texture_record_u32(fixed + 56);
}

static inline const char *texture_record_check(const unsigned char *header, size_t header_bytes, uint64_t file_size,
	uint64_t hash, uint32_t width, uint32_t height, uint32_t format, struct texture_record_layout *layout)
{
	uint32_t factor, level, w, h;
	uint64_t offset;

	if (header_bytes < TEXTURE_RECORD_FIXED_SIZE || header[0] != 'V' || header[1] != 'U' || header[2] != 'T' ||
		header[3] != 'R')
		return "not a texture record";
	if (texture_record_u32(header + 4) != TEXTURE_RECORD_VERSION)
		return "a record of another version";
	layout->format = texture_record_u32(header + 8);
	layout->source_width = texture_record_u32(header + 12);
	layout->source_height = texture_record_u32(header + 16);
	layout->width = texture_record_u32(header + 20);
	layout->height = texture_record_u32(header + 24);
	layout->levels = texture_record_u32(header + 28);
	layout->hash = texture_record_u64(header + 32);
	layout->recipe_key = texture_record_u64(header + 40);
	layout->checksum = texture_record_u64(header + 48);
	layout->header_size = texture_record_u32(header + 56);
	if (layout->format != TEXTURE_RECORD_BC7 && layout->format != TEXTURE_RECORD_ASTC4X4)
		return "in an unknown format";
	if (layout->format != format)
		return format == TEXTURE_RECORD_BC7 ? "ASTC 4x4, not this GPU's BC7" : "BC7, not this GPU's ASTC 4x4";
	if (layout->hash != hash || layout->source_width != width || layout->source_height != height)
		return "for another texture";
	for (factor = 1; factor <= 8; factor *= 2)
		if (width && height && width <= TEXTURE_RECORD_MAXIMUM_DIMENSION / factor &&
			height <= TEXTURE_RECORD_MAXIMUM_DIMENSION / factor && layout->width == width * factor &&
			layout->height == height * factor)
			break;
	if (factor > 8)
		return "not 1, 2, 4 or 8 times the size of the texture";
	if (layout->levels < 1 || layout->levels > TEXTURE_RECORD_MAXIMUM_LEVELS)
		return "of a bad level count";
	if (texture_record_u32(header + 60) || layout->header_size != TEXTURE_RECORD_FIXED_SIZE + 16 * layout->levels)
		return "of a bad header size";
	if (header_bytes < layout->header_size)
		return "shorter than its level table";
	w = layout->width;
	h = layout->height;
	offset = layout->header_size;
	for (level = 0; level < layout->levels; level++)
	{
		layout->level_offset[level] = texture_record_u64(header + TEXTURE_RECORD_FIXED_SIZE + 16 * level);
		layout->level_size[level] = texture_record_u64(header + TEXTURE_RECORD_FIXED_SIZE + 16 * level + 8);
		if (layout->level_offset[level] != offset || layout->level_size[level] != texture_record_level_bytes(w, h))
			return "of a bad level table";
		offset += layout->level_size[level];
		if (w == 1 && h == 1 && level + 1 < layout->levels)
			return "of more levels than its size has";
		w = texture_record_half(w);
		h = texture_record_half(h);
	}
	if (offset != file_size)
		return "not the size its levels add up to";
	layout->payload_size = offset - layout->header_size;
	return NULL;
}

/* NULL if the payload (the bytes after the header) matches the header's checksum */
static inline const char *texture_record_check_payload(const struct texture_record_layout *layout,
	const unsigned char *payload)
{
	return texture_record_checksum(payload, layout->payload_size) == layout->checksum ? NULL :
		"damaged (its checksum differs)";
}

#endif
