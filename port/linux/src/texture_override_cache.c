/*
TEXTURE_OVERRIDE_CACHE.C

Replacement textures read from their files into GPU memory, under
texture_replacement_cache.h's budget. See texture_override_cache.h.
*/

#include "texture_override_cache.h"
#include "texture_override.h"
#include "texture_record.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern struct gpu_capabilities device_capabilities;
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static struct texture_replacement_cache cache;
static int started;
static const char *directories[2] = { "", "" };
/* the first set is the upscaled textures' cache (texture_record.h) rather
than a folder of <hash>.rgba files: its record format (TEXTURE_RECORD_*, 0:
not a cache) and its manifest's entries, sorted by key */
static uint32_t records_format;
struct record_entry
{
	uint64_t hash, recipe_key;
	uint32_t width, height;
};
static struct record_entry *record_entries;
static size_t record_entry_count;
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
	records_format = 0;
}

static int by_record_key(const void *a, const void *b)
{
	const struct record_entry *x = a, *y = b;

	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	if (x->width != y->width)
		return x->width < y->width ? -1 : 1;
	return (x->height > y->height) - (x->height < y->height);
}

size_t texture_override_cache_records(const char *directory, uint32_t format, const char *second)
{
	char path[1024], line[512];
	FILE *file;
	size_t capacity = 0;
	int length;

	free(record_entries);
	record_entries = NULL;
	record_entry_count = 0;
	directories[0] = directory ? directory : "";
	directories[1] = second ? second : "";
	records_format = format;
	length = snprintf(path, sizeof(path), "%s/manifest.tsv", directories[0]);
	file = format && *directories[0] && length > 0 && (size_t)length < sizeof(path) ? fopen(path, "r") : NULL;
	if (!file)
		return 0;
	/* (port/runtime/texture_cache.c's manifest: hash, width, height, result, treatment, recipe key, global key,
	bytes) */
	while (fgets(line, sizeof(line), file))
	{
		struct record_entry entry;
		unsigned result, treatment;
		uint64_t global, bytes;

		if (sscanf(line, "%" SCNx64 "\t%u\t%u\t%u\t%u\t%" SCNx64 "\t%" SCNx64 "\t%" SCNu64, &entry.hash,
			&entry.width, &entry.height, &result, &treatment, &entry.recipe_key, &global, &bytes) != 8)
			continue;
		/* (an entry kept original, TEXTURE_POLICY_ORIGINAL, has no record to draw) */
		if (result == 2)
			continue;
		if (record_entry_count == capacity)
		{
			size_t more = capacity ? capacity * 2 : 256;
			struct record_entry *grown = realloc(record_entries, more * sizeof(*grown));

			if (!grown)
				break;
			record_entries = grown;
			capacity = more;
		}
		record_entries[record_entry_count++] = entry;
	}
	fclose(file);
	if (record_entry_count)
		qsort(record_entries, record_entry_count, sizeof(*record_entries), by_record_key);
	return record_entry_count;
}

static const struct record_entry *record_entry(uint64_t hash, uint32_t width, uint32_t height)
{
	struct record_entry key = { hash, 0, width, height };

	return record_entry_count ? bsearch(&key, record_entries, record_entry_count, sizeof(*record_entries),
		by_record_key) : NULL;
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
/* reads a cache record (texture_record.h) into a texture, as load does a file of the folder */
static void load_record(struct texture_replacement *record)
{
	struct texture_record_layout layout;
	struct gpu_texture_description description = { 0 };
	unsigned char header[TEXTURE_RECORD_FIXED_SIZE + 16 * TEXTURE_RECORD_MAXIMUM_LEVELS];
	const struct record_entry *entry = record_entry(record->hash, record->width, record->height);
	char path[1024];
	unsigned char *bytes;
	const char *problem = NULL;
	gpu_texture texture;
	double began = now_milliseconds();
	FILE *file;
	long size = 0;
	size_t header_bytes = 0;
	uint32_t level;
	int length;

	length = snprintf(path, sizeof(path), "%s/%016llx-%lux%lu.bin", directories[0], (unsigned long long)record->hash,
		(unsigned long)record->width, (unsigned long)record->height);
	file = entry && length > 0 && (size_t)length < sizeof(path) ? fopen(path, "rb") : NULL;
	if (!file)
	{
		texture_replacement_loaded(&cache, record, 0, 0);
		return;
	}
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < TEXTURE_RECORD_FIXED_SIZE || fseek(file, 0, SEEK_SET))
		problem = "shorter than its header";
	if (!problem)
	{
		header_bytes = fread(header, 1, TEXTURE_RECORD_FIXED_SIZE, file);
		if (header_bytes == TEXTURE_RECORD_FIXED_SIZE && texture_record_header_size(header) > TEXTURE_RECORD_FIXED_SIZE &&
			texture_record_header_size(header) <= sizeof(header))
			header_bytes += fread(header + header_bytes, 1, texture_record_header_size(header) - header_bytes, file);
		problem = texture_record_check(header, header_bytes, (uint64_t)size, record->hash, record->width,
			record->height, records_format, &layout);
	}
	if (!problem && layout.recipe_key != entry->recipe_key)
		problem = "made by another recipe than the manifest's";
	if (!problem && (layout.width > device_capabilities.max_texture_size ||
		layout.height > device_capabilities.max_texture_size))
		problem = "larger than the device's textures";
	if (problem)
	{
		platform_log("texture cache %016llx: %s is %s; not drawn", (unsigned long long)record->hash, path, problem);
		fclose(file);
		texture_replacement_loaded(&cache, record, 0, 0);
		return;
	}
	if (!texture_replacement_admit(&cache, record, (uint64_t)layout.payload_size))
	{
		/* no room: the texture is drawn as the map has it until a later try finds some */
		fclose(file);
		return;
	}
	bytes = malloc((size_t)layout.payload_size);
	if (!bytes || fread(bytes, 1, (size_t)layout.payload_size, file) != (size_t)layout.payload_size ||
		(problem = texture_record_check_payload(&layout, bytes)) != NULL)
	{
		platform_log("texture cache %016llx: could not read %s%s%s", (unsigned long long)record->hash, path,
			problem ? ": " : "", problem ? problem : "");
		fclose(file);
		free(bytes);
		texture_replacement_loaded(&cache, record, 0, 0);
		return;
	}
	fclose(file);
	description.type = GPU_TEXTURE_2D;
	description.format = layout.format == TEXTURE_RECORD_BC7 ? GPU_FORMAT_BC7 :
		layout.format == TEXTURE_RECORD_BGRA8 ? GPU_FORMAT_BGRA8 : GPU_FORMAT_ASTC4X4;
	description.usage = GPU_USAGE_UPLOAD;
	description.width = layout.width;
	description.height = layout.height;
	description.depth = 1;
	description.levels = layout.levels;
	texture = gpu_texture_create(&description);
	for (level = 0; texture && level < layout.levels; level++)
	{
		uint32_t w = texture_record_half_to(layout.width, level), h = texture_record_half_to(layout.height, level);

		/* a BGRA8 level's padding is not texels */
		gpu_texture_upload(texture, 0, level, bytes + (layout.level_offset[level] - layout.header_size),
			layout.format == TEXTURE_RECORD_BGRA8 ? w * h * 4 : (uint32_t)layout.level_size[level]);
	}
	free(bytes);
	if (texture)
		platform_log("texture cache %016llx: %ux%u %s, %u levels, %.2f MB, in place of %lux%lu",
			(unsigned long long)record->hash, layout.width, layout.height,
			layout.format == TEXTURE_RECORD_BC7 ? "BC7" : layout.format == TEXTURE_RECORD_BGRA8 ? "BGRA8" : "ASTC 4x4",
			layout.levels,
			(double)layout.payload_size / (1024.0 * 1024.0), (unsigned long)record->width,
			(unsigned long)record->height);
	else
		platform_log("texture cache %016llx: the device made no texture for %s", (unsigned long long)record->hash, path);
	texture_replacement_loaded(&cache, record, texture, layout.levels);
	read_milliseconds += now_milliseconds() - began;
	reads++;
}

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

	if (record->set == 0 && records_format)
	{
		load_record(record);
		return;
	}
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
