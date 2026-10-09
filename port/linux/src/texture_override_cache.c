/* Debug override loading, shared by the original toggle and resolution comparison. */
#include "texture_override_cache.h"
#include "texture_override.h"
#include <stdio.h>
#include <stdlib.h>

extern struct gpu_capabilities device_capabilities;
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

struct override_result
{
	uint64_t hash;
	unsigned long width, height, levels;
	gpu_texture texture;
	int fallback;
};
static struct override_result *results;
static unsigned long count, capacity, serial = 1;

unsigned long texture_override_cache_serial(void) { return serial; }
void texture_override_cache_clear(void)
{
	unsigned long index;
	/* The backend's destroy API retires handles; already encoded Metal/GL
	work retains the underlying resources until its commands complete. */
	for (index = 0; index < count; index++)
		if (results[index].texture)
			gpu_texture_destroy(results[index].texture);
	count = 0;
	serial++;
}

/* Check the header and total length before allocating any pixel storage. */
static FILE *open_override(const char *directory, uint64_t hash, unsigned long width, unsigned long height,
	struct texture_override_layout *layout, size_t *size)
{
	char path[1024]; unsigned char header[TEXTURE_OVERRIDE_HEADER_SIZE];
	FILE *file; long length; int path_length; const char *problem;
	if (!directory || !*directory) return NULL;
	path_length = snprintf(path, sizeof(path), "%s/%016llx.rgba", directory, (unsigned long long)hash);
	if (path_length < 0 || (size_t)path_length >= sizeof(path))
		return NULL;
	file = fopen(path, "rb");
	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < TEXTURE_OVERRIDE_HEADER_SIZE ||
		fseek(file, 0, SEEK_SET) || fread(header, 1, sizeof(header), file) != sizeof(header))
	{
		platform_log("texture override %016llx: could not read header", (unsigned long long)hash);
		fclose(file); return NULL;
	}
	problem = texture_override_check(header, (size_t)length, width, height, layout);
	if (!problem && (layout->width > device_capabilities.max_texture_size ||
		layout->height > device_capabilities.max_texture_size))
		problem = "larger than the GPU texture limit";
	if (problem)
	{
		platform_log("texture override %016llx: %s is %s; not applied", (unsigned long long)hash, path, problem);
		fclose(file); return NULL;
	}
	*size = (size_t)length;
	return file;
}

static gpu_texture load_override(const char *directory, uint64_t hash, unsigned long width,
	unsigned long height, uint32_t expected_width, uint32_t expected_height, unsigned long *levels)
{
	struct texture_override_layout layout;
	struct gpu_texture_description description = { 0 };
	size_t size; FILE *file = open_override(directory, hash, width, height, &layout, &size);
	unsigned char *bytes; gpu_texture texture; uint32_t level;
	if (!file) return 0;
	if (expected_width && (layout.width != expected_width || layout.height != expected_height))
	{
		platform_log("texture comparison %016llx: higher is not twice current's dimensions; using APPROVED CURRENT",
			(unsigned long long)hash);
		fclose(file); return 0;
	}
	bytes = malloc(size);
	if (!bytes || fseek(file, 0, SEEK_SET) || fread(bytes, 1, size, file) != size)
	{
		platform_log("texture override %016llx: could not read pixels", (unsigned long long)hash);
		fclose(file); free(bytes); return 0;
	}
	fclose(file);
	description.type = GPU_TEXTURE_2D;
	description.format = GPU_FORMAT_BGRA8;
	description.usage = GPU_USAGE_UPLOAD;
	description.width = layout.width; description.height = layout.height;
	description.depth = 1; description.levels = layout.levels;
	texture = gpu_texture_create(&description);
	if (!texture)
	{
		platform_log("texture override %016llx: GPU allocation failed", (unsigned long long)hash);
		free(bytes); return 0;
	}
	for (level = 0; level < layout.levels; level++)
	{
		texture_override_rgba_to_bgra(bytes + layout.level_offset[level], layout.level_size[level] / 4);
		gpu_texture_upload(texture, 0, level, bytes + layout.level_offset[level], (uint32_t)layout.level_size[level]);
	}
	free(bytes); *levels = layout.levels;
	platform_log("texture override %016llx: %ux%u, %u levels, in place of %lux%lu", (unsigned long long)hash,
		layout.width, layout.height, layout.levels, width, height);
	return texture;
}

gpu_texture texture_override_cache_find(const char *current, const char *higher_directory, int higher,
	uint64_t hash, unsigned long width, unsigned long height, unsigned long *levels, int *fallback)
{
	struct override_result *result; unsigned long index;
	for (index = 0; index < count; index++)
	{
		result = &results[index];
		if (result->hash == hash && result->width == width && result->height == height)
			goto found;
	}
	if (count == capacity)
	{
		unsigned long next_capacity = capacity ? capacity * 2 : 64;
		struct override_result *next = realloc(results, next_capacity * sizeof(*results));
		if (!next) { *levels = 0; *fallback = higher; return 0; }
		results = next; capacity = next_capacity;
	}
	result = &results[count++];
	*result = (struct override_result){ .hash = hash, .width = width, .height = height };
	if (higher)
	{
		struct texture_override_layout current_layout; size_t size;
		FILE *file = open_override(current, hash, width, height, &current_layout, &size);
		if (file)
		{
			fclose(file);
			result->texture = load_override(higher_directory, hash, width, height,
				current_layout.width * 2, current_layout.height * 2, &result->levels);
			if (!result->texture)
			{
				result->fallback = 1;
				platform_log("texture comparison %016llx: HIGHER unavailable; using APPROVED CURRENT",
					(unsigned long long)hash);
				result->texture = load_override(current, hash, width, height, 0, 0, &result->levels);
				if (!result->texture) result->fallback = 2;
			}
		}
	}
	else
		result->texture = load_override(current, hash, width, height, 0, 0, &result->levels);
found:
	*levels = result->levels; *fallback = result->fallback;
	return result->texture;
}
