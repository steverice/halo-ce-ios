/* Hardware capability tiers and compatibility envelopes. The same TOML data is checked by
   tools/profile_catalog_check.py; malformed catalogs never replace the conservative class. */
#include "host_profile.h"
#include "tomlc17.h"
#include "../../linux/src/halo_display.h"
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CATALOG_LIMIT 128

static toml_datum_t field(toml_datum_t table, const char *name) { return toml_get(table, name); }
static const char *string(toml_datum_t table, const char *name)
{
	toml_datum_t value = field(table, name);
	return value.type == TOML_STRING ? value.u.s : "";
}
static int number(toml_datum_t table, const char *name, int minimum, int maximum)
{
	toml_datum_t value = field(table, name);
	return value.type == TOML_INT64 && value.u.int64 >= minimum && value.u.int64 <= maximum ? (int)value.u.int64 : -1;
}
static int allowed(toml_datum_t table, const char *keys)
{
	int i;
	if (table.type != TOML_TABLE) return 0;
	for (i = 0; i < table.u.tab.size; i++)
	{
		char key[80];
		snprintf(key, sizeof(key), "|%s|", table.u.tab.key[i]);
		if (!strstr(keys, key)) return 0;
	}
	return 1;
}
static int names_valid(toml_datum_t array)
{
	int i;
	if (array.type == TOML_UNKNOWN) return 1;
	if (array.type != TOML_ARRAY || array.u.arr.size > 32) return 0;
	for (i = 0; i < array.u.arr.size; i++)
	{
		toml_datum_t v = array.u.arr.elem[i];
		if (v.type != TOML_STRING || !*v.u.s || strlen(v.u.s) > 48 ||
			strspn(v.u.s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ,.-") != strlen(v.u.s)) return 0;
	}
	return 1;
}
static int contains(toml_datum_t array, const char *name)
{
	int i;
	if (array.type != TOML_ARRAY || !name || !*name) return 0;
	for (i = 0; i < array.u.arr.size; i++)
		if (array.u.arr.elem[i].type == TOML_STRING && !strcmp(array.u.arr.elem[i].u.s, name)) return 1;
	return 0;
}
static int overlaps(toml_datum_t a, toml_datum_t b)
{
	int i;
	if (a.type != TOML_ARRAY) return 0;
	for (i = 0; i < a.u.arr.size; i++) if (contains(b, a.u.arr.elem[i].u.s)) return 1;
	return 0;
}
static int present(toml_datum_t a) { return a.type == TOML_ARRAY && a.u.arr.size > 0; }
static int identity(toml_datum_t h, const struct host_device *d)
{
	toml_datum_t models = field(h, "models"), chips = field(h, "chips");
	/* Both constraints apply when a model ships with several chips. */
	return !strcmp(string(h, "platform"), d->platform) &&
		(!present(models) || contains(models, d->model)) && (!present(chips) || contains(chips, d->chip));
}
static int hardware_index(toml_datum_t hardware, const char *id)
{
	int i;
	for (i = 0; i < hardware.u.arr.size; i++) if (!strcmp(string(hardware.u.arr.elem[i], "id"), id)) return i;
	return -1;
}
static toml_result_t read_table(const char *path)
{
	toml_result_t result = {0};
	int descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	struct stat info;
	FILE *file;
	char *text;
	size_t size;
	if (descriptor < 0) return result;
	if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
		info.st_size > HOST_PROFILE_MAXIMUM_FILE_BYTES) { close(descriptor); return result; }
	file = fdopen(descriptor, "rb");
	if (!file) { close(descriptor); return result; }
	size = (size_t)info.st_size;
	text = malloc(size + 1);
	if (text && fread(text, 1, size, file) == size)
	{
		text[size] = 0;
		result = toml_parse(text, (int)size);
	}
	free(text); fclose(file); return result;
}
static int valid_catalog(toml_datum_t root)
{
	toml_datum_t hardware = field(root, "hardware"), fallback = field(root, "fallback");
	int i, j;
	if (!allowed(root, "|schema|version|hardware|fallback|") || number(root, "schema", 1, 1) != 1 ||
		!*string(root, "version") || strlen(string(root, "version")) > 48 ||
		hardware.type != TOML_ARRAY || hardware.u.arr.size > CATALOG_LIMIT ||
		fallback.type != TOML_ARRAY || fallback.u.arr.size > CATALOG_LIMIT) return 0;
	for (i = 0; i < hardware.u.arr.size; i++)
	{
		toml_datum_t h = hardware.u.arr.elem[i], models = field(h, "models"), chips = field(h, "chips");
		const char *platform = string(h, "platform"), *basis = string(h, "basis");
		char platform_key[32], basis_key[32];
		snprintf(platform_key, sizeof(platform_key), "|%s|", platform);
		snprintf(basis_key, sizeof(basis_key), "|%s|", basis);
		if (!allowed(h, "|id|platform|models|chips|tier|gpu_family|basis|source|") ||
			!*string(h, "id") || strlen(string(h, "id")) > 48 ||
			strspn(string(h, "id"), "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(string(h, "id")) || !*platform ||
			!strstr("|ios|tvos|visionos|mac|", platform_key) || !*basis ||
			!strstr("|measured|assumed|inherited|", basis_key) || !*string(h, "source") ||
			!names_valid(models) || !names_valid(chips) || (!present(models) && !present(chips)) ||
			number(h, "gpu_family", 1, 32) < 0 ||
			(field(h, "tier").type != TOML_UNKNOWN && number(h, "tier", 1, 1000000) < 0)) return 0;
		for (j = 0; j < i; j++)
		{
			toml_datum_t other = hardware.u.arr.elem[j];
			if (!strcmp(string(h, "id"), string(other, "id"))) return 0;
			if (!strcmp(platform, string(other, "platform")) &&
				(!present(models) || !present(field(other, "models")) || overlaps(models, field(other, "models"))) &&
				(!present(chips) || !present(field(other, "chips")) || overlaps(chips, field(other, "chips")))) return 0;
		}
	}
	for (i = 0; i < fallback.u.arr.size; i++)
	{
		toml_datum_t f = fallback.u.arr.elem[i], pixels = field(f, "maximum_pixels");
		const char *file = string(f, "profile");
		int h = hardware_index(hardware, string(f, "hardware"));
		size_t length = strlen(file);
		if (!allowed(f, "|hardware|profile|minimum_memory_mb|minimum_gpu_family|maximum_pixels|maximum_refresh_hertz|") ||
			h < 0 || number(hardware.u.arr.elem[h], "tier", 1, 1000000) < 0 ||
			length < 6 || length >= 64 || strcmp(file + length - 5, ".toml") ||
			strspn(file, "abcdefghijklmnopqrstuvwxyz0123456789-.") != length || strstr(file, "..") ||
			number(f, "minimum_memory_mb", 0, 1000000) < 0 || number(f, "minimum_gpu_family", 1, 32) < 0 ||
			number(f, "maximum_refresh_hertz", 1, 1000) < 0 || pixels.type != TOML_ARRAY || pixels.u.arr.size != 3) return 0;
		for (j = 0; j < 3; j++) if (pixels.u.arr.elem[j].type != TOML_INT64 ||
			pixels.u.arr.elem[j].u.int64 < 1 || pixels.u.arr.elem[j].u.int64 > 100000000) return 0;
		for (j = 0; j < i; j++)
		{
			int previous = hardware_index(hardware, string(fallback.u.arr.elem[j], "hardware"));
			if (previous >= 0 && !strcmp(string(hardware.u.arr.elem[h], "platform"), string(hardware.u.arr.elem[previous], "platform")) &&
				number(hardware.u.arr.elem[h], "tier", 1, 1000000) == number(hardware.u.arr.elem[previous], "tier", 1, 1000000)) return 0;
		}
	}
	return 1;
}

static const char *setting(const struct host_profile *p, const char *name, const char *scene, int external, const char *otherwise)
{
	int i;
	for (i = p->count - 1; i >= 0; i--) if (p->values[i].external == external && !strcmp(p->values[i].name, name) && !strcmp(p->values[i].scene, scene))
		return p->values[i].text;
	return otherwise;
}
static int compatible(const struct host_device *d, const struct host_profile *p, toml_datum_t f)
{
	static const char *scenes[] = { "", "menu", "cutscene" };
	int i, short_side = d->pixel_width < d->pixel_height ? d->pixel_width : d->pixel_height;
	int long_side = d->pixel_width > d->pixel_height ? d->pixel_width : d->pixel_height;
	if (short_side <= 0 || d->refresh_rate <= 0 || !d->memory_megabytes ||
		d->memory_megabytes < (unsigned)number(f, "minimum_memory_mb", 0, 1000000) ||
		d->gpu_family < number(f, "minimum_gpu_family", 1, 32)) return 0;
	for (i = 0; i < 3; i++)
	{
		const char *height = setting(p, "display.render_height", scenes[i], 0, setting(p, "display.render_height", "", 0, "0"));
		const char *scale = setting(p, "display.render_scale", scenes[i], 0, setting(p, "display.render_scale", "", 0, "1"));
		const char *base_rate = setting(p, "display.frame_rate", "", 0, "display");
		const char *scene_rate = i ? setting(p, "display.frame_rate", scenes[i], 0, "") : "";
		/* Use the renderer's rounding, aspect clamp, and 240-line minimum. Ignoring the
		   GPU's maximum texture dimension is conservative: it can only reduce allocation. */
		struct halo_display_size size = halo_display_render_size(lround(long_side * atof(scale)),
			lround(short_side * atof(scale)), 1600, 0, atoi(height), INT_MAX);
		double pixels = (double)size.width * size.height;
		/* Base frame_rate is a Mac-only cap. Numeric scene caps limit every platform;
		   they combine with the base cap rather than replacing it. */
		double target = d->refresh_rate;
		if (!strcmp(d->platform, "mac"))
			target = !strcmp(base_rate, "display") ? d->refresh_rate : !strcmp(base_rate, "unlimited") ? 1000000 : atof(base_rate);
		if (atoi(scene_rate) > 0 && atoi(scene_rate) < target) target = atoi(scene_rate);
		if (pixels > field(f, "maximum_pixels").u.arr.elem[i].u.int64 ||
			target > number(f, "maximum_refresh_hertz", 1, 1000)) return 0;
		/* External displays have no hardware identity. Bound their allocation at
		   the renderer's widest frame shape, rather than the built-in panel's shape. */
		int has_external = 0;
		for (int v = 0; v < p->count; v++) if (p->values[v].external) has_external = 1;
		if (has_external)
		{
			const char *external_height = setting(p, "display.render_height", scenes[i], 1,
				setting(p, "display.render_height", "", 1, height));
			const char *external_scale = setting(p, "display.render_scale", scenes[i], 1,
				setting(p, "display.render_scale", "", 1, scale));
			long output_height = atol(setting(p, "display.output_height", "", 1, "0"));
			if (!output_height) output_height = 8192;
			size = halo_display_render_size(lround(output_height * (1600.0 / 480.0) * atof(external_scale)),
				lround(output_height * atof(external_scale)), 1600, 0, atoi(external_height), 8192);
			if ((double)size.width * size.height > field(f, "maximum_pixels").u.arr.elem[i].u.int64) return 0;
		}
	}
	return 1;
}

int host_profile_inherit(const struct host_device *device, const char *folder,
	void (*log)(const char *), struct host_profile *profile)
{
	char path[1024], message[512];
	toml_result_t catalog;
	toml_datum_t hardware, fallbacks, target;
	int i, target_index = -1, best = 0, found = 0;
	if (!folder) return 0;
	snprintf(path, sizeof(path), "%s/hardware/catalog.toml", folder);
	catalog = read_table(path);
	if (!catalog.ok || !valid_catalog(catalog.toptab))
	{
		if (log) log("device profile: missing or invalid hardware catalog; class fallback retained");
		toml_free(catalog); return 0;
	}
	hardware = field(catalog.toptab, "hardware"); fallbacks = field(catalog.toptab, "fallback");
	for (i = 0; i < hardware.u.arr.size; i++) if (identity(hardware.u.arr.elem[i], device)) target_index = i;
	if (target_index < 0 || number(hardware.u.arr.elem[target_index], "tier", 1, 1000000) < 0)
	{
		if (log) log("device profile: unknown or unordered hardware; class fallback retained");
		toml_free(catalog); return 0;
	}
	target = hardware.u.arr.elem[target_index];
	for (i = 0; i < fallbacks.u.arr.size; i++)
	{
		toml_datum_t f = fallbacks.u.arr.elem[i];
		int h = hardware_index(hardware, string(f, "hardware"));
		toml_datum_t source = hardware.u.arr.elem[h];
		int tier = number(source, "tier", 1, 1000000), k;
		toml_result_t source_file;
		struct host_device original = *device;
		struct host_profile inherited;
		const char *selector = NULL;
		if (tier <= best || tier > number(target, "tier", 1, 1000000) || strcmp(string(source, "platform"), device->platform)) continue;
		snprintf(path, sizeof(path), "%s/%s", folder, string(f, "profile")); source_file = read_table(path);
		if (!source_file.ok) { toml_free(source_file); continue; }
		if (number(source_file.toptab, "schema", 1, 1) == 1 && !strcmp(string(source_file.toptab, "kind"), "model") &&
			!strcmp(string(field(source_file.toptab, "match"), "platform"), device->platform))
		{
			static const char *fields[] = { "models", "chips" };
			for (k = 0; k < 2 && !selector; k++)
			{
				toml_datum_t names = field(source, fields[k]); int n;
				for (n = 0; names.type == TOML_ARRAY && n < names.u.arr.size; n++)
					if (contains(field(field(source_file.toptab, "match"), fields[k]), names.u.arr.elem[n].u.s))
					{ selector = names.u.arr.elem[n].u.s; break; }
			}
		}
		if (!selector) { toml_free(source_file); continue; }
		original.gpu_family = number(source, "gpu_family", 1, 32);
		host_profile_resolve(&original, folder, NULL, selector, NULL, &inherited);
		snprintf(path, sizeof(path), "%s/%s", folder, inherited.class_file);
		toml_result_t class_file = read_table(path);
		toml_datum_t family = toml_seek(class_file.toptab, "match.gpu_family");
		int class_covers = class_file.ok && family.type == TOML_ARRAY && family.u.arr.size == 2 &&
			family.u.arr.elem[0].type == TOML_INT64 && family.u.arr.elem[1].type == TOML_INT64 &&
			family.u.arr.elem[0].u.int64 <= original.gpu_family &&
			(family.u.arr.elem[1].u.int64 == 0 || family.u.arr.elem[1].u.int64 >= original.gpu_family);
		toml_free(class_file);
		if (!class_covers || strcmp(inherited.model_file, string(f, "profile")) || !*inherited.class_file ||
			!compatible(device, &inherited, f))
		{
			snprintf(message, sizeof(message), "device profile: skipped %s; incompatible profile envelope or source class", string(f, "profile"));
			if (log) log(message);
			toml_free(source_file); continue;
		}
		*profile = inherited;
		profile->inherited = 1;
		profile->source_tier = tier; profile->target_tier = number(target, "tier", 1, 1000000);
		snprintf(profile->source_hardware, sizeof(profile->source_hardware), "%s", string(source, "id"));
		snprintf(profile->target_hardware, sizeof(profile->target_hardware), "%s", string(target, "id"));
		snprintf(profile->catalog_version, sizeof(profile->catalog_version), "%s", string(catalog.toptab, "version"));
		best = tier; found = 1;
		toml_free(source_file);
	}
	if (found)
	{
		snprintf(message, sizeof(message), "device profile: inherited %s; target %s (tier %d), source %s (tier %d); catalog %s", profile->model_file,
			profile->target_hardware, profile->target_tier, profile->source_hardware, profile->source_tier, profile->catalog_version);
		if (log) log(message);
	}
	else if (log) log("device profile: no compatible profile at or below hardware tier; class fallback retained");
	toml_free(catalog); return found;
}


int host_profile_catalog_validate(const char *folder)
{
	char path[1024]; int valid = 0;
	snprintf(path, sizeof(path), "%s/hardware/catalog.toml", folder);
	toml_result_t catalog = read_table(path);
	if (!catalog.ok || !valid_catalog(catalog.toptab)) goto done;
	toml_datum_t hardware = field(catalog.toptab, "hardware"), fallback = field(catalog.toptab, "fallback");
	for (int i = 0; i < fallback.u.arr.size; i++)
	{
		toml_datum_t f = fallback.u.arr.elem[i];
		int h = hardware_index(hardware, string(f, "hardware"));
		toml_datum_t source = hardware.u.arr.elem[h];
		snprintf(path, sizeof(path), "%s/%s", folder, string(f, "profile"));
		toml_result_t profile = read_table(path);
		int match = profile.ok && !strcmp(string(profile.toptab, "kind"), "model") &&
			!strcmp(string(field(profile.toptab, "match"), "platform"), string(source, "platform")) &&
			(overlaps(field(source, "models"), toml_seek(profile.toptab, "match.models")) ||
			 overlaps(field(source, "chips"), toml_seek(profile.toptab, "match.chips")));
		toml_free(profile);
		if (!match) goto done;
	}
	valid = 1;
done:
	toml_free(catalog);
	return valid;
}

/* A device shard carries only its target and the conservative source envelope it may inherit. */
int host_profile_catalog_device_validate(const char *folder, const struct host_device *device, const char *profile)
{
    char path[1024]; int valid = 0, target = -1;
    snprintf(path,sizeof(path),"%s/hardware/catalog.toml",folder);
    toml_result_t catalog = read_table(path);
    if (!catalog.ok || !valid_catalog(catalog.toptab)) goto done;
    toml_datum_t hardware = field(catalog.toptab,"hardware"), fallbacks = field(catalog.toptab,"fallback");
    for (int i = 0; i < hardware.u.arr.size; i++) if (identity(hardware.u.arr.elem[i],device)) target = i;
    for (int i = 0; i < fallbacks.u.arr.size; i++) {
        toml_datum_t f = fallbacks.u.arr.elem[i];
        int source = hardware_index(hardware,string(f,"hardware"));
        if (target < 0 || source < 0 || strcmp(string(hardware.u.arr.elem[source],"platform"),device->platform) ||
            number(hardware.u.arr.elem[source],"tier",1,1000000) > number(hardware.u.arr.elem[target],"tier",1,1000000)) goto done;
    }
    for (int i = 0; i < hardware.u.arr.size; i++) {
        if (i == target) continue;
        int used = 0;
        for (int j = 0; j < fallbacks.u.arr.size; j++) if (!strcmp(string(fallbacks.u.arr.elem[j],"hardware"),string(hardware.u.arr.elem[i],"id"))) used = 1;
        if (!used) goto done;
    }
    if (profile) {
        for (int i = 0; i < fallbacks.u.arr.size; i++) if (!strcmp(string(fallbacks.u.arr.elem[i],"profile"),profile)) valid = 1;
    } else valid = 1;
done:
    toml_free(catalog); return valid;
}
