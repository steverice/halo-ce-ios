/*
TEXTURE_OVERRIDE_CACHE.C

Replacement textures read from their files into GPU memory, under
texture_replacement_cache.h's budget. See texture_override_cache.h.
*/

#include "texture_override_cache.h"
#include "texture_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern struct gpu_capabilities device_capabilities;
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static struct texture_replacement_cache cache;
static int started;
static const char *directories[2] = { "", "" };
static double read_milliseconds;
static unsigned long reads;

static void release(uint32_t texture)
{
	gpu_texture_destroy(texture);
}

static double now_milliseconds(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (double)time.tv_sec * 1000.0 + (double)time.tv_nsec / 1000000.0;
}

void texture_override_cache_start(uint64_t budget)
{
	if (started)
		return;
	texture_replacement_cache_initialize(&cache, budget, release);
	started = 1;
}

struct texture_replacement_cache *texture_override_cache(void)
{
	return started ? &cache : NULL;
}

void texture_override_cache_directories(const char *first, const char *second)
{
	directories[0] = first ? first : "";
	directories[1] = second ? second : "";
}

void texture_override_cache_map_loaded(int enabled)
{
	if (started)
		texture_replacement_map_loaded(&cache, enabled);
}

void texture_override_cache_select(int second)
{
	if (started)
		texture_replacement_select(&cache, second ? 1 : 0);
}

unsigned long texture_override_cache_begin_frame(uint64_t *freed)
{
	unsigned long count = 0;

	*freed = started ? texture_replacement_begin_frame(&cache, &count) : 0;
	return count;
}

uint64_t texture_override_cache_memory_warning(void)
{
	return started ? texture_replacement_memory_warning(&cache) : 0;
}

double texture_override_cache_take_read_milliseconds(unsigned long *count)
{
	double milliseconds = read_milliseconds;

	*count = reads;
	read_milliseconds = 0.0;
	reads = 0;
	return milliseconds;
}

/* reads the record's file, <hash>.rgba in its set's folder, into a texture,
if the file can stand for a texture of the record's size and the budget has
room for it. The header is checked before any of the pixels are read */
static void load(struct texture_replacement *record)
{
	struct texture_override_layout layout;
	struct gpu_texture_description description = { 0 };
	unsigned char header[TEXTURE_OVERRIDE_HEADER_SIZE];
	char path[1024];
	unsigned char *bytes;
	const char *problem = NULL;
	gpu_texture texture;
	double began = now_milliseconds();
	FILE *file;
	long size = 0;
	size_t pixels;
	uint32_t level;
	int length;

	length = snprintf(path, sizeof(path), "%s/%016llx.rgba", directories[record->set], (unsigned long long)record->hash);
	file = *directories[record->set] && length > 0 && (size_t)length < sizeof(path) ? fopen(path, "rb") : NULL;
	if (!file)
	{
		texture_replacement_loaded(&cache, record, 0, 0);
		return;
	}
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < TEXTURE_OVERRIDE_HEADER_SIZE || fseek(file, 0, SEEK_SET) ||
		fread(header, 1, sizeof(header), file) != sizeof(header))
		problem = "shorter than its header";
	if (!problem)
		problem = texture_override_check(header, (size_t)size, record->width, record->height, &layout);
	if (!problem && (layout.width > device_capabilities.max_texture_size ||
		layout.height > device_capabilities.max_texture_size))
		problem = "larger than the device's textures";
	if (problem)
	{
		platform_log("texture override %016llx: %s is %s; not drawn", (unsigned long long)record->hash, path, problem);
		fclose(file);
		texture_replacement_loaded(&cache, record, 0, 0);
		return;
	}
	pixels = (size_t)size - TEXTURE_OVERRIDE_HEADER_SIZE;
	if (!texture_replacement_admit(&cache, record, pixels))
	{
		/* no room: the texture is drawn as the map has it until a later try finds some */
		fclose(file);
		return;
	}
	bytes = malloc(pixels);
	if (!bytes || fread(bytes, 1, pixels, file) != pixels)
	{
		platform_log("texture override %016llx: could not read %s", (unsigned long long)record->hash, path);
		fclose(file);
		free(bytes);
		texture_replacement_loaded(&cache, record, 0, 0);
		return;
	}
	fclose(file);
	description.type = GPU_TEXTURE_2D;
	description.format = GPU_FORMAT_BGRA8;
	description.usage = GPU_USAGE_UPLOAD;
	description.width = layout.width;
	description.height = layout.height;
	description.depth = 1;
	description.levels = layout.levels;
	texture = gpu_texture_create(&description);
	for (level = 0; texture && level < layout.levels; level++)
	{
		unsigned char *texels = bytes + layout.level_offset[level] - TEXTURE_OVERRIDE_HEADER_SIZE;

		texture_override_rgba_to_bgra(texels, layout.level_size[level] / 4);
		gpu_texture_upload(texture, 0, level, texels, (uint32_t)layout.level_size[level]);
	}
	free(bytes);
	if (texture)
		platform_log("texture override %016llx: %ux%u, %u levels, in place of %lux%lu", (unsigned long long)record->hash,
			layout.width, layout.height, layout.levels, (unsigned long)record->width, (unsigned long)record->height);
	else
		platform_log("texture override %016llx: the device made no texture for %s", (unsigned long long)record->hash, path);
	texture_replacement_loaded(&cache, record, texture, layout.levels);
	read_milliseconds += now_milliseconds() - began;
	reads++;
}

gpu_texture texture_override_cache_find(struct texture_override_reference *reference, uint64_t hash,
	unsigned long width, unsigned long height, unsigned long *levels, int *fallback)
{
	struct texture_replacement *record;
	int pass;

	*fallback = 0;
	if (!started)
		return 0;
	if (reference->serial != cache.serial)
	{
		reference->index = texture_replacement_record(&cache, cache.selected, hash, (uint32_t)width, (uint32_t)height);
		reference->serial = cache.serial;
	}
	/* (the second pass: the first set's, where the second has none) */
	for (pass = 0; pass < 2; pass++)
	{
		if (reference->index < 0)
			return 0;
		record = &cache.records[reference->index];
		if (record->state != _texture_replacement_resident && texture_replacement_wants_load(&cache, record))
			load(record);
		if (record->state != _texture_replacement_absent || record->set == 0)
			break;
		reference->index = texture_replacement_record(&cache, 0, hash, (uint32_t)width, (uint32_t)height);
	}
	if (record->state != _texture_replacement_resident)
		return 0;
	*fallback = cache.selected != record->set;
	texture_replacement_touch(&cache, record);
	*levels = record->levels;
	return record->texture;
}
