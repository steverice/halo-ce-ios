/* The upscaled textures' cache records (texture_record.h) read by texture_override_cache.c, with the GPU at its
boundary stubbed: a BC7 and an ASTC 4x4 record of a full mip chain, a BGRA8 gate exception any GPU draws, uploaded as blocks with no decode and counted
at their compressed size; records the reader must refuse (another format, another recipe than the manifest's, a
damaged payload, a torn file, one the manifest does not list, a size that is not 1, 2, 4 or 8 times); and the
second set of debug.texture_compare_directory beside the cache. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "texture_override_cache.h"
#include "texture_record.h"

struct gpu_capabilities device_capabilities = { .max_texture_size = 8192 };
static unsigned created, live, uploads, last_format, last_levels, last_width;
static uint64_t uploaded_bytes;
void platform_log(const char *format, ...) { (void)format; }
gpu_texture gpu_texture_create(const struct gpu_texture_description *d)
{
	last_format = d->format; last_levels = d->levels; last_width = d->width;
	live++; return ++created;
}
void gpu_texture_destroy(gpu_texture texture) { assert(texture); live--; }
void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	const unsigned char *blocks = data;
	(void)face;
	assert(texture && data && (last_format == GPU_FORMAT_BGRA8 ? size % 4 == 0 : size % 16 == 0));
	/* each block holds its level's number: the levels arrive in order, undecoded */
	for (uint32_t i = 0; i < size; i++)
		assert(blocks[i] == (unsigned char)(level + 1));
	uploads++; uploaded_bytes += size;
}

static void put32(unsigned char *at, uint32_t value) { for (int i = 0; i < 4; i++) at[i] = (unsigned char)(value >> (8 * i)); }
static void put64(unsigned char *at, uint64_t value) { put32(at, (uint32_t)value); put32(at + 4, (uint32_t)(value >> 32)); }

/* a record of a w x h original at factor times its size, every level, in format; damage: 1 flips a payload
byte after the checksum, 2 cuts the file short */
static uint64_t record(const char *directory, uint64_t hash, uint32_t w, uint32_t h, uint32_t factor, uint32_t format,
	uint64_t recipe, int damage)
{
	unsigned char header[TEXTURE_RECORD_FIXED_SIZE + 16 * TEXTURE_RECORD_MAXIMUM_LEVELS] = { 0 };
	unsigned char *payload = NULL;
	uint32_t width = w * factor, height = h * factor, levels = 0, x = width, y = height;
	uint64_t size = 0, offset;
	char path[512];
	FILE *file;

	for (;;)
	{
		uint64_t bytes = texture_record_format_level_bytes(format, x, y);

		payload = realloc(payload, size + bytes);
		memset(payload + size, levels + 1, bytes);
		size += bytes;
		levels++;
		if (x == 1 && y == 1)
			break;
		x = texture_record_half(x); y = texture_record_half(y);
	}
	memcpy(header, TEXTURE_RECORD_MAGIC, 4);
	put32(header + 4, TEXTURE_RECORD_VERSION); put32(header + 8, format);
	put32(header + 12, w); put32(header + 16, h); put32(header + 20, width); put32(header + 24, height);
	put32(header + 28, levels); put64(header + 32, hash); put64(header + 40, recipe);
	put64(header + 48, texture_record_checksum(payload, size));
	put32(header + 56, TEXTURE_RECORD_FIXED_SIZE + 16 * levels);
	offset = TEXTURE_RECORD_FIXED_SIZE + 16 * levels;
	x = width; y = height;
	for (uint32_t level = 0; level < levels; level++)
	{
		put64(header + TEXTURE_RECORD_FIXED_SIZE + 16 * level, offset);
		put64(header + TEXTURE_RECORD_FIXED_SIZE + 16 * level + 8, texture_record_format_level_bytes(format, x, y));
		offset += texture_record_format_level_bytes(format, x, y);
		x = texture_record_half(x); y = texture_record_half(y);
	}
	if (damage == 1)
		payload[size / 2] ^= 0x40;
	snprintf(path, sizeof(path), "%s/%016llx-%ux%u.bin", directory, (unsigned long long)hash, w, h);
	file = fopen(path, "wb"); assert(file);
	assert(fwrite(header, 1, TEXTURE_RECORD_FIXED_SIZE + 16 * levels, file) == TEXTURE_RECORD_FIXED_SIZE + 16 * levels);
	assert(fwrite(payload, 1, damage == 2 ? size - 16 : size, file) == (damage == 2 ? size - 16 : size));
	fclose(file);
	free(payload);
	return size;
}

static gpu_texture find(uint64_t hash, uint32_t w, uint32_t h, unsigned long *levels)
{
	struct texture_override_reference reference = { 0 };
	int fallback;

	*levels = 0;
	return texture_override_cache_find(&reference, hash, w, h, levels, &fallback);
}

int main(void)
{
	char root[] = "/tmp/texture-record-XXXXXX", cache[512], second[512], path[512];
	unsigned long levels;
	uint64_t bc7, astc, raw;
	FILE *manifest;

	assert(!strcmp(TEXTURE_RECORD_DIRECTORY, "texture-cache"));
	/* the checksum as tools/texture_cache_write.py computes it: two words, a = 3, b = 1 + 2 * 2 */
	{
		unsigned char words[16] = { 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0 };
		assert(texture_record_checksum(words, 16) == (3ull ^ (5ull << 32)));
	}
	assert(mkdtemp(root));
	snprintf(cache, sizeof(cache), "%s/texture-cache", root); assert(!mkdir(cache, 0700));
	snprintf(second, sizeof(second), "%s/second", root); assert(!mkdir(second, 0700));
	bc7 = record(cache, 0x11, 64, 32, 8, TEXTURE_RECORD_BC7, 0xabc, 0);         /* 8x: 512x256, 10 levels */
	astc = record(cache, 0x22, 16, 16, 2, TEXTURE_RECORD_ASTC4X4, 0xdef, 0);    /* 2x: 32x32, 6 levels */
	record(cache, 0x33, 16, 16, 2, TEXTURE_RECORD_BC7, 0x999, 0);               /* the manifest says another recipe */
	record(cache, 0x44, 16, 16, 4, TEXTURE_RECORD_BC7, 0x444, 1);               /* damaged */
	record(cache, 0x55, 16, 16, 4, TEXTURE_RECORD_BC7, 0x555, 2);               /* torn */
	record(cache, 0x66, 16, 16, 2, TEXTURE_RECORD_BC7, 0x666, 0);               /* not listed */
	record(cache, 0x77, 16, 16, 16, TEXTURE_RECORD_BC7, 0x777, 0);              /* 16x */
	raw = record(cache, 0x88, 4, 2, 2, TEXTURE_RECORD_BGRA8, 0x888, 0);       /* a gate exception: 8x4 BGRA8, 4 levels */
	snprintf(path, sizeof(path), "%s/manifest.tsv", cache);
	manifest = fopen(path, "w"); assert(manifest);
	/* hash, width, height, result, treatment, recipe key, global key, bytes (port/runtime/texture_cache.c) */
	fprintf(manifest, "0000000000000011\t64\t32\t0\t1\t0000000000000abc\t0000000000000001\t%llu\n",
		(unsigned long long)bc7);
	fprintf(manifest, "0000000000000022\t16\t16\t1\t1\t0000000000000def\t0000000000000001\t%llu\n",
		(unsigned long long)astc);
	fprintf(manifest, "0000000000000033\t16\t16\t1\t1\t0000000000000333\t0000000000000001\t1\n");
	fprintf(manifest, "0000000000000044\t16\t16\t1\t1\t0000000000000444\t0000000000000001\t1\n");
	fprintf(manifest, "0000000000000055\t16\t16\t1\t1\t0000000000000555\t0000000000000001\t1\n");
	fprintf(manifest, "0000000000000077\t16\t16\t1\t1\t0000000000000777\t0000000000000001\t1\n");
	fprintf(manifest, "0000000000000088\t4\t2\t1\t1\t0000000000000888\t0000000000000001\t%llu\n",
		(unsigned long long)raw);
	fprintf(manifest, "not a line\n");
	fclose(manifest);

	texture_override_cache_start(64 << 20);
	/* a BC GPU: the BC7 record, all ten levels as blocks, counted compressed */
	assert(texture_override_cache_records(cache, TEXTURE_RECORD_BC7, "") == 7);
	texture_override_cache_map_loaded(1);
	assert(find(0x11, 64, 32, &levels) && levels == 10 && last_format == GPU_FORMAT_BC7 && last_width == 512);
	assert(uploads == 10 && uploaded_bytes == bc7 && texture_override_cache()->bytes == bc7);
	/* 512x256 RGBA8 with its mips would be 4/3 of 512 KB: the record is a quarter of that */
	assert(bc7 * 4 < 512ull * 256 * 4 * 4 / 3 + 4096);
	/* ASTC on a BC GPU, another recipe, damage, a torn file, unlisted, 16x: none drawn, nothing made */
	assert(!find(0x22, 16, 16, &levels) && !find(0x33, 16, 16, &levels) && !find(0x44, 16, 16, &levels));
	assert(!find(0x55, 16, 16, &levels) && !find(0x66, 16, 16, &levels) && !find(0x77, 16, 16, &levels));
	assert(created == 1 && live == 1);
	/* the BGRA8 exception is drawn on any GPU, its levels' padding not uploaded (the 2x1 and 1x1 levels pad to 8) */
	uploaded_bytes = 0;
	assert(find(0x88, 4, 2, &levels) && levels == 4 && last_format == GPU_FORMAT_BGRA8 && last_width == 8);
	assert(uploaded_bytes == (8 * 4 + 4 * 2 + 2 * 1 + 1 * 1) * 4 && raw == 128 + 32 + 8 + 8);
	assert(created == 2 && live == 2);
	/* a GPU without BC: the ASTC record; the BC7 one is the wrong format there */
	texture_override_cache_map_loaded(0);
	assert(!live);
	assert(texture_override_cache_records(cache, TEXTURE_RECORD_ASTC4X4, "") == 7);
	texture_override_cache_map_loaded(1);
	assert(find(0x22, 16, 16, &levels) && levels == 6 && last_format == GPU_FORMAT_ASTC4X4 && last_width == 32);
	assert(!find(0x11, 64, 32, &levels) && live == 1);
	assert(find(0x88, 4, 2, &levels) && last_format == GPU_FORMAT_BGRA8 && live == 2);
	/* no manifest: nothing is listed, and nothing is read */
	texture_override_cache_map_loaded(0);
	assert(texture_override_cache_records(second, TEXTURE_RECORD_BC7, "") == 0);
	texture_override_cache_map_loaded(1);
	assert(!find(0x11, 64, 32, &levels) && !live);
	/* a format of 0 (a GPU that draws neither) lists nothing even with a manifest */
	assert(texture_override_cache_records(cache, 0, "") == 0);
	puts("PASS: texture cache records: BC7 and ASTC 4x4 mip chains as blocks, BGRA8 exceptions, refusals, manifest index");
	return 0;
}
