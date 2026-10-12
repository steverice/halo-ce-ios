/* texture_override_cache.c: real replacement files of two sets read under the budget, with the GPU at its boundary
stubbed. Selection, the first set standing in for a file the second lacks, both sets in memory, files the cache must
refuse, and a device that makes no texture. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "texture_override_cache.h"

struct gpu_capabilities device_capabilities = { .max_texture_size = 8192 };
static unsigned created, destroyed, live, uploads, last_width;
static int fail_wide;
void platform_log(const char *format, ...) { (void)format; }
gpu_texture gpu_texture_create(const struct gpu_texture_description *d)
{
	last_width = d->width;
	if (fail_wide == 2 || (fail_wide && d->width == 8)) return 0;
	live++; return ++created;
}
void gpu_texture_destroy(gpu_texture texture) { assert(texture); destroyed++; live--; }
void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	(void)face; (void)level; assert(texture && data && size); uploads++;
	const unsigned char *pixels = data;
	/* RGBA in the file, BGRA at the GPU */
	assert(pixels[0] == 3 && pixels[1] == 2 && pixels[2] == 1 && pixels[3] == 4);
}
static void fixture(const char *directory, uint64_t hash, unsigned width, unsigned height)
{
	char path[512]; unsigned char header[12] = { 0 }, pixels[512] = { 0 };
	FILE *file; unsigned words[3] = { width, height, 1 }, word, byte;
	pixels[0] = 1; pixels[1] = 2; pixels[2] = 3; pixels[3] = 4;
	for (word = 0; word < 3; word++) for (byte = 0; byte < 4; byte++)
		header[word * 4 + byte] = words[word] >> (byte * 8);
	snprintf(path, sizeof(path), "%s/%016llx.rgba", directory, (unsigned long long)hash);
	file = fopen(path, "wb"); assert(file);
	assert(fwrite(header, 1, 12, file) == 12);
	assert(width * height * 4 <= sizeof(pixels));
	assert(fwrite(pixels, 1, width * height * 4, file) == width * height * 4);
	fclose(file);
}
/* a draw of the original 2x1 texture with this hash: a fresh reference each time, as after an upload */
static gpu_texture find(uint64_t hash, int *fallback)
{
	struct texture_override_reference reference = { 0 };
	unsigned long levels = 0;
	gpu_texture texture = texture_override_cache_find(&reference, hash, 2, 1, &levels, fallback);
	assert(!texture || levels == 1);
	return texture;
}
static void next_map(void)
{
	uint64_t freed;
	texture_override_cache_map_loaded(0);   /* a level without replacements drops them all */
	assert(!live && destroyed == created);
	texture_override_cache_map_loaded(1);
	texture_override_cache_begin_frame(&freed);
}
int main(void)
{
	char root[] = "/tmp/texture-compare-XXXXXX", first[512], second[512], path[512];
	struct texture_override_reference reference = { 0 };
	unsigned long levels; int fallback; gpu_texture a, b; uint64_t freed;
	assert(mkdtemp(root));
	snprintf(first, sizeof(first), "%s/first", root); assert(!mkdir(first, 0700));
	snprintf(second, sizeof(second), "%s/second", root); assert(!mkdir(second, 0700));
	/* before the cache exists nothing is replaced */
	assert(!texture_override_cache() && !find(1, &fallback));
	texture_override_cache_start(64 << 20);
	texture_override_cache_directories(first, second);
	texture_override_cache_map_loaded(1);
	fixture(first, 1, 4, 2); fixture(second, 1, 8, 4);
	/* the first set, read once; the same reference finds it again without a search */
	a = texture_override_cache_find(&reference, 1, 2, 1, &levels, &fallback);
	assert(a && levels == 1 && !fallback && live == 1 && last_width == 4);
	assert(texture_override_cache_find(&reference, 1, 2, 1, &levels, &fallback) == a);
	assert(find(1, &fallback) == a && created == 1 && uploads == 1);
	/* the second set: its own file, and the first set's texture stays in memory beside it */
	texture_override_cache_select(1);
	b = texture_override_cache_find(&reference, 1, 2, 1, &levels, &fallback);
	assert(b && b != a && last_width == 8 && !fallback && live == 2 && created == 2);
	assert(texture_override_cache()->bytes == 4 * 2 * 4 + 8 * 4 * 4);
	/* switches back and forth read nothing more */
	for (unsigned toggle = 0; toggle < 20; toggle++)
	{
		texture_override_cache_select(toggle % 2);
		texture_override_cache_begin_frame(&freed);
		assert(texture_override_cache_find(&reference, 1, 2, 1, &levels, &fallback) == (toggle % 2 ? b : a));
		assert(!fallback && live == 2 && created == 2 && uploads == 2);
	}
	next_map();
	/* a file only the first set has: the first set's stands in, flagged, and is not read twice */
	fixture(first, 2, 4, 2);
	texture_override_cache_select(1);
	a = find(2, &fallback);
	assert(a && fallback && last_width == 4 && live == 1);
	assert(find(2, &fallback) == a && fallback && created == 3);
	texture_override_cache_select(0);
	assert(find(2, &fallback) == a && !fallback && created == 3);
	/* a file only the second set has is drawn in the second set alone */
	fixture(second, 3, 8, 4);
	assert(!find(3, &fallback));
	texture_override_cache_select(1);
	assert(find(3, &fallback) && !fallback && last_width == 8);
	next_map();
	/* the second set's file may be the first's size: no fallback */
	fixture(first, 4, 4, 2); fixture(second, 4, 4, 2);
	texture_override_cache_select(1);
	assert(find(4, &fallback) && !fallback && live == 1);
	next_map();
	/* a short pixel payload is refused whatever its header says, and the first set stands in */
	fixture(first, 5, 4, 2); fixture(second, 5, 8, 4);
	snprintf(path, sizeof(path), "%s/%016llx.rgba", second, 5ULL);
	assert(!truncate(path, 12 + 8 * 4 * 4 - 1));
	texture_override_cache_select(1);
	assert(find(5, &fallback) && fallback == 1 && last_width == 4 && live == 1);
	next_map();
	/* a size that is no multiple of the original's is refused */
	fixture(first, 6, 6, 3);
	assert(!find(6, &fallback) && !live);
	/* a device that makes no texture must not be uploaded to */
	fail_wide = 1;
	texture_override_cache_select(1);
	assert(find(1, &fallback) && fallback && last_width == 4 && live == 1);
	next_map();
	fail_wide = 2;
	texture_override_cache_select(1);
	assert(!find(1, &fallback) && !live);
	next_map(); fail_wide = 0;
	/* the device's largest texture applies before anything is allocated */
	device_capabilities.max_texture_size = 4;
	texture_override_cache_select(1);
	assert(find(1, &fallback) && fallback && last_width == 4 && live == 1);
	next_map();
	device_capabilities.max_texture_size = 8192;
	/* a budget with room for one set only: the set left behind is dropped for the one arriving, and read again
	on the way back */
	texture_override_cache()->budget = 8 * 4 * 4;
	a = find(1, &fallback);
	assert(a && live == 1);
	texture_override_cache_select(1);
	b = find(1, &fallback);
	assert(b && b != a && !fallback && live == 1 && last_width == 8);
	texture_override_cache_select(0);
	assert(find(1, &fallback) && !fallback && live == 1 && last_width == 4);
	assert(texture_override_cache()->statistics.reloads == 1 && texture_override_cache()->statistics.evictions == 2);
	/* the memory warning drops what is not being drawn */
	texture_override_cache_begin_frame(&freed); texture_override_cache_begin_frame(&freed);
	texture_override_cache_begin_frame(&freed);
	assert(texture_override_cache_memory_warning() == 4 * 2 * 4 && !live);
	texture_override_cache_map_loaded(0);
	assert(!live && destroyed == created);
	for (unsigned hash = 1; hash <= 6; hash++)
	{
		snprintf(path, sizeof(path), "%s/%016llx.rgba", first, (unsigned long long)hash); unlink(path);
		snprintf(path, sizeof(path), "%s/%016llx.rgba", second, (unsigned long long)hash); unlink(path);
	}
	rmdir(first); rmdir(second); rmdir(root);
	puts("texture compare probe: PASS (selection, both sets under the budget, fallback, refused files, allocation)");
	return 0;
}
