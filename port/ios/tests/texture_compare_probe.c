/* Real override files and cache selection, with allocation at the GPU boundary. */
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
static int fail_higher;
void platform_log(const char *format, ...) { (void)format; }
gpu_texture gpu_texture_create(const struct gpu_texture_description *d)
{
	last_width = d->width;
	if (fail_higher == 2 || (fail_higher && d->width == 8)) return 0;
	live++; return ++created;
}
void gpu_texture_destroy(gpu_texture texture) { assert(texture); destroyed++; live--; }
void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	(void)face; (void)level; assert(texture && data && size); uploads++;
	const unsigned char *pixels = data;
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
int main(void)
{
	char root[] = "/tmp/texture-compare-XXXXXX", current[512], higher[512], path[512];
	unsigned long levels, serial; int fallback; gpu_texture first, second;
	assert(mkdtemp(root));
	snprintf(current, sizeof(current), "%s/current", root); assert(!mkdir(current, 0700));
	snprintf(higher, sizeof(higher), "%s/higher", root); assert(!mkdir(higher, 0700));
	fixture(current, 1, 4, 2); fixture(higher, 1, 8, 4);
	first = texture_override_cache_find(current, higher, 0, 1, 2, 1, &levels, &fallback);
	assert(first && levels == 1 && !fallback && live == 1 && last_width == 4);
	assert(texture_override_cache_find(current, higher, 0, 1, 2, 1, &levels, &fallback) == first);
	assert(created == 1 && uploads == 1);
	serial = texture_override_cache_serial(); texture_override_cache_clear();
	assert(live == 0 && destroyed == 1 && texture_override_cache_serial() != serial);
	second = texture_override_cache_find(current, higher, 1, 1, 2, 1, &levels, &fallback);
	assert(second && second != first && last_width == 8 && !fallback && live == 1);
	texture_override_cache_clear();
	/* Missing higher: return current, explicitly flagged; cache the miss too. */
	fixture(current, 2, 4, 2);
	first = texture_override_cache_find(current, higher, 1, 2, 2, 1, &levels, &fallback);
	assert(first && fallback && last_width == 4 && live == 1);
	assert(texture_override_cache_find(current, higher, 1, 2, 2, 1, &levels, &fallback) == first && fallback);
	texture_override_cache_clear();
	/* A higher-only hash cannot broaden the current policy. */
	fixture(higher, 3, 8, 4);
	assert(!texture_override_cache_find(current, higher, 1, 3, 2, 1, &levels, &fallback));
	assert(!fallback && !live);
	/* A valid file of the wrong comparison dimensions falls back visibly. */
	fixture(current, 4, 4, 2); fixture(higher, 4, 4, 2);
	assert(texture_override_cache_find(current, higher, 1, 4, 2, 1, &levels, &fallback));
	assert(fallback && last_width == 4);
	texture_override_cache_clear();
	/* A short pixel payload cannot be uploaded, even with a valid header. */
	fixture(current, 5, 4, 2); fixture(higher, 5, 8, 4);
	snprintf(path, sizeof(path), "%s/%016llx.rgba", higher, 5ULL);
	assert(!truncate(path, 12 + 8 * 4 * 4 - 1));
	assert(texture_override_cache_find(current, higher, 1, 5, 2, 1, &levels, &fallback));
	assert(fallback == 1 && last_width == 4 && live == 1);
	texture_override_cache_clear();
	/* Repeated live switches keep exactly one set resident and advance the
	serial that invalidates texture entries' fast lookup aliases. */
	for (unsigned toggle = 0; toggle < 20; toggle++)
	{
		serial = texture_override_cache_serial();
		assert(texture_override_cache_find(current, higher, toggle % 2, 1, 2, 1, &levels, &fallback));
		assert(!fallback && live == 1 && last_width == (toggle % 2 ? 8 : 4));
		texture_override_cache_clear();
		assert(!live && texture_override_cache_serial() != serial);
	}
	/* An allocation failure on higher must not upload to handle 0. */
	fail_higher = 1;
	assert(texture_override_cache_find(current, higher, 1, 1, 2, 1, &levels, &fallback));
	assert(fallback && last_width == 4 && live == 1);
	texture_override_cache_clear();
	fail_higher = 2;
	assert(!texture_override_cache_find(current, higher, 1, 1, 2, 1, &levels, &fallback));
	assert(fallback == 2 && !live);
	texture_override_cache_clear(); fail_higher = 0;
	/* Device dimension limits apply before allocation. */
	device_capabilities.max_texture_size = 4;
	assert(texture_override_cache_find(current, higher, 1, 1, 2, 1, &levels, &fallback));
	assert(fallback && last_width == 4 && live == 1);
	texture_override_cache_clear();
	assert(!live && destroyed == created);
	for (unsigned hash = 1; hash <= 5; hash++)
	{
		snprintf(path, sizeof(path), "%s/%016llx.rgba", current, (unsigned long long)hash); unlink(path);
		snprintf(path, sizeof(path), "%s/%016llx.rgba", higher, (unsigned long long)hash); unlink(path);
	}
	rmdir(current); rmdir(higher); rmdir(root);
	puts("texture compare probe: PASS (selection, policy, fallback, allocation and retirement)");
	return 0;
}
