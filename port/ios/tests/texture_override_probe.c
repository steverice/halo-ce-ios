/* texture_override.h: the hash and file checks of debug.texture_override_directory
(xbox_textures.c), which step-tools/texture-upscale.py's files must pass */
#include "texture_override.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char file[6 << 20];

/* a file with this header and the texel bytes of `levels` halving levels of
width x height, less short_by bytes */
static size_t make_file(uint32_t width, uint32_t height, uint32_t levels, size_t short_by)
{
	uint32_t words[3] = { width, height, levels }, level;
	size_t size = TEXTURE_OVERRIDE_HEADER_SIZE;
	int word, byte;

	for (word = 0; word < 3; word++)
		for (byte = 0; byte < 4; byte++)
			file[word * 4 + byte] = (unsigned char)(words[word] >> (byte * 8));
	for (level = 0; level < levels; level++)
	{
		uint32_t w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1;

		memset(file + size, (int)level, (size_t)w * h * 4);
		size += (size_t)w * h * 4;
	}
	return size - short_by;
}

/* The layout checker reads only the header. Check production-size mip
chains without reserving their pixel storage in this probe. */
static void check_large_layout(uint32_t width, uint32_t height, uint32_t levels,
	unsigned long original_width, unsigned long original_height)
{
	unsigned char header[TEXTURE_OVERRIDE_HEADER_SIZE];
	uint32_t words[3] = { width, height, levels };
	size_t size = TEXTURE_OVERRIDE_HEADER_SIZE;
	struct texture_override_layout layout;
	for (unsigned word = 0; word < 3; word++)
		for (unsigned byte = 0; byte < 4; byte++)
			header[word * 4 + byte] = words[word] >> (byte * 8);
	for (unsigned level = 0; level < levels; level++)
		size += (size_t)(width >> level ? width >> level : 1) *
			(height >> level ? height >> level : 1) * 4;
	assert(!texture_override_check(header, size, original_width, original_height, &layout));
	assert(layout.width == width && layout.height == height && layout.levels == levels);
	assert(layout.level_offset[levels - 1] + layout.level_size[levels - 1] == size);
}

int main(void)
{
	struct texture_override_layout layout;
	unsigned char texels[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	size_t size;

	/* 64-bit FNV-1a's published test vectors */
	assert(texture_override_hash((const unsigned char *)"", 0) == 0xcbf29ce484222325ULL);
	assert(texture_override_hash((const unsigned char *)"a", 1) == 0xaf63dc4c8601ec8cULL);
	assert(texture_override_hash((const unsigned char *)"foobar", 6) == 0x85944171f73967e8ULL);

	/* 2x of a 4x2 texture: 8x4, 4x2, 2x1, 1x1 */
	size = make_file(8, 4, 4, 0);
	assert(size == 12 + (32 + 8 + 2 + 1) * 4);
	assert(texture_override_check(file, size, 4, 2, &layout) == NULL);
	assert(layout.levels == 4 && layout.level_offset[1] == 12 + 128 && layout.level_size[3] == 4);
	/* 1x and 4x stand too */
	assert(texture_override_check(file, make_file(4, 2, 3, 0), 4, 2, &layout) == NULL);
	assert(texture_override_check(file, make_file(16, 8, 5, 0), 4, 2, &layout) == NULL);
	assert(texture_override_check(file, make_file(32, 16, 6, 0), 4, 2, &layout) == NULL);
	/* Eightfold dimensions stay bounded, including on the 32-bit guest. */
	assert(texture_override_check(file, make_file(8192, 8, 1, 0), 1024, 1, &layout) == NULL);
	assert(texture_override_check(file, make_file(16384, 8, 1, 0), 2048, 1, &layout) != NULL);
	assert(texture_override_check(file, make_file(8, 4, 1, 0), UINT32_MAX, UINT32_MAX, &layout) != NULL);
	check_large_layout(8192, 512, 14, 1024, 64);
	check_large_layout(4096, 4096, 13, 512, 512);
	/* panel strip's chain (a) at 2x: 2048x512 down to 4x1, ten levels */
	assert(texture_override_check(file, make_file(2048, 512, 10, 0), 1024, 256, &layout) == NULL);
	assert(layout.level_size[9] == 4 * 1 * 4);

	/* rejected: a byte short, or extra bytes */
	assert(texture_override_check(file, make_file(8, 4, 4, 1), 4, 2, &layout) != NULL);
	assert(texture_override_check(file, make_file(8, 4, 4, 0) + 4, 4, 2, &layout) != NULL);
	/* rejected: a 4:1 jump (8x4, then 2x1 and 1x1, the 4x2 level missing) */
	make_file(8, 4, 3, 0);
	assert(texture_override_check(file, 12 + (32 + 2 + 1) * 4, 4, 2, &layout) != NULL);
	/* rejected: 3x, and the same bytes for a texture of another shape */
	assert(texture_override_check(file, make_file(12, 6, 2, 0), 4, 2, &layout) != NULL);
	assert(texture_override_check(file, make_file(8, 8, 4, 0), 4, 2, &layout) != NULL);
	/* rejected: more levels than halving gives, none, no header */
	assert(texture_override_check(file, make_file(8, 4, 5, 0), 4, 2, &layout) != NULL);
	assert(texture_override_check(file, make_file(8, 4, 0, 0), 4, 2, &layout) != NULL);
	assert(texture_override_check(file, 8, 4, 2, &layout) != NULL);

	texture_override_rgba_to_bgra(texels, 2);
	assert(texels[0] == 3 && texels[2] == 1 && texels[3] == 4 && texels[4] == 7 && texels[6] == 5);
	puts("PASS: texture override hash, file layout and checks");
	return 0;
}
