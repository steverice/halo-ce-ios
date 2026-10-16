/* config.toml: the file the game writes is one TOML accepts, with each table
once, and a file with a table or key repeated (as the writer once made on
iOS and visionOS) is read with the first of each, not thrown away. Built
twice by tools/ios_test.py: with HALO_ILP32 for the iOS and visionOS
settings, including the visionOS-only ones, and without it for the
desktop's. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* platform.h reads the Xbox SDK's headers; port_config.c needs only its log */
#define __HALO_LINUX_PLATFORM_H
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static char logged[1 << 16];

void platform_log(const char *format, ...)
{
	size_t length = strlen(logged);
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(logged + length, sizeof(logged) - length, format, arguments);
	va_end(arguments);
	length = strlen(logged);
	if (length + 1 < sizeof(logged))
		strcat(logged, "\n");
}

int host_external_active(void) { return 0; }

#include "port_config.c"
#include "tomlc17.c"

static int count(const char *text, const char *needle)
{
	int found = 0;

	for (text = strstr(text, needle); text; text = strstr(text + 1, needle))
		found++;
	return found;
}

/* the file as the writer made it before the fix: a header whenever the
section changes in the table's order */
static char *old_default_text(void)
{
	struct config_text text = { NULL, 0, 0 };
	char section[32] = "";
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		char buffer[64];

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot)
			continue;
		if (strncmp(section, setting->name, (size_t)(dot - setting->name)) || section[dot - setting->name] != 0)
		{
			snprintf(section, sizeof(section), "%.*s", (int)(dot - setting->name), setting->name);
			snprintf(buffer, sizeof(buffer), "\n[%s]\n", section);
			config_append(&text, buffer);
		}
		config_append_setting(&text, setting);
	}
	return text.buffer;
}

/* every setting of this build is in the parsed file, once, at its default */
static void check_every_setting(toml_datum_t table)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		struct config_value expected = { 0 }, read = { 0 };

		if (!(setting->platforms & CONFIG_PLATFORM))
			continue;
		if (toml_seek(table, setting->name).type == TOML_UNKNOWN)
		{
			fprintf(stderr, "FAIL: %s is missing from the written file\n", setting->name);
			exit(1);
		}
		if (setting->type == _config_string)
		{
			size_t length = strlen(setting->default_value);

			expected.string = config_copy(setting->default_value + 1, length - 2);
		}
		else
		{
			config_set_from_text(&expected, setting->type, setting->default_value);
		}
		config_set_from_file(&read, setting, table);
		assert(expected.boolean == read.boolean && expected.integer == read.integer && expected.real == read.real);
		assert(!expected.string == !read.string && (!read.string || !strcmp(expected.string, read.string)));
		free(expected.string);
		free(read.string);
	}
}

static void check_written_file(void)
{
	char *text = config_default_text();
	toml_result_t result = toml_parse(text, (int)strlen(text));
	const char *const sections[] = { "[display]", "[input]", "[audio]", "[game]", "[network]", "[debug]" };
	size_t index;

	if (!result.ok)
	{
		fprintf(stderr, "FAIL: the written file doesn't parse: %s\n", result.errmsg);
		exit(1);
	}
	for (index = 0; index < sizeof(sections) / sizeof(sections[0]); index++)
		assert(count(text, sections[index]) == 1);
	check_every_setting(result.toptab);
#ifdef HALO_ILP32
	/* the visionOS and iOS settings that came after [input] in the table */
	assert(toml_seek(result.toptab, "display.theater_width").type == TOML_FP64);
	assert(toml_seek(result.toptab, "display.renderer").type == TOML_STRING);
	assert(toml_seek(result.toptab, "display.foveation").type == TOML_BOOLEAN);
	assert(toml_seek(result.toptab, "display.render_quality").type == TOML_FP64);
	assert(toml_seek(result.toptab, "debug.foveation_eye_passes").type == TOML_BOOLEAN);
	assert(toml_seek(result.toptab, "debug.rate_map_test").type == TOML_BOOLEAN);
	assert(toml_seek(result.toptab, "input.turn").type == TOML_STRING);
	assert(toml_seek(result.toptab, "input.stick_dead_zone").type == TOML_FP64);
#endif
	/* nothing to fold in it */
	assert(!config_fold_repeats(text));
	toml_free(result);
	free(text);
}

static void check_folding(void)
{
	char *old = old_default_text();
	struct config_text text = { NULL, 0, 0 };
	toml_result_t result;
	char *folded;

	config_append(&text, old);
	/* and a key set again in its table, as an earlier merge left one */
	config_append(&text, "\n[audio]\nvolume = 0.25\nenabled = false\n");
	result = toml_parse(text.buffer, (int)text.length);
#ifdef HALO_ILP32
	/* the bug itself: the old writer's file doesn't parse */
	assert(!result.ok);
#endif
	toml_free(result);
	logged[0] = 0;
	folded = config_fold_repeats(text.buffer);
	assert(folded);
	assert(strstr(logged, "audio.volume is set again"));
	assert(strstr(logged, "audio.enabled is set again"));
	assert(strstr(logged, "[audio] is defined again"));
#ifdef HALO_ILP32
	assert(strstr(logged, "[display] is defined again"));
	assert(strstr(logged, "[input] is defined again"));
#endif
	result = toml_parse(folded, (int)strlen(folded));
	if (!result.ok)
	{
		fprintf(stderr, "FAIL: the folded file doesn't parse: %s\n", result.errmsg);
		exit(1);
	}
	/* every setting kept, the first value of each */
	check_every_setting(result.toptab);
	assert(!config_fold_repeats(folded));
	toml_free(result);
	free(folded);
	free(old);
	free(text.buffer);
}

#ifdef HALO_ILP32
static void write_text(const char *path, const char *text)
{
	FILE *file = fopen(path, "wb");

	assert(file);
	fputs(text, file);
	fclose(file);
}

static void reload(void)
{
	config_loaded = 0;
	logged[0] = 0;
}

/* the game reading a file with repeats: the first values, the rest of the
file kept, and the file written back folded */
static void check_reading(void)
{
	char directory[] = "/tmp/config-probe-XXXXXX";
	char path[256];
	char *text;
	size_t size;
	toml_result_t result;
	size_t index;
	int written;

	if (!mkdtemp(directory))
	{
		perror("mkdtemp");
		exit(1);
	}
	setenv("HALO_DATA_ROOT", directory, 1);
	snprintf(path, sizeof(path), "%s/config.toml", directory);

	write_text(path,
		"# mine\n"
		"[display]\n"
		"stereo = \"head\"\n"
		"foveation = true\n"
		"\n[input]\n"
		"turn = \"smooth\"\n"
		"\n[display]\n"
		"# my width\n"
		"theater_width = 75.0\n"
		"stereo = \"off\"\n"
		"render_quality = 0.8\n"
		"\n[debug]\n"
		"foveation_eye_passes = false\n"
		"\n[input]\n"
		"stick_dead_zone = 0.1\n");
	reload();
	assert(!strcmp(config_string("display.stereo"), "head"));
	assert(config_boolean("display.foveation"));
	assert(config_real("display.theater_width") == 75.0);
	assert(config_real("display.render_quality") == 0.8);
	assert(!config_boolean("debug.foveation_eye_passes"));
	assert(!strcmp(config_string("input.turn"), "smooth"));
	assert(config_real("input.stick_dead_zone") == 0.1);
	/* (an unset one at its default) */
	assert(!config_boolean("debug.rate_map_test"));
	assert(strstr(logged, "config.toml line 9: [display] is defined again"));
	assert(strstr(logged, "config.toml line 12: display.stereo is set again"));
	assert(strstr(logged, "config.toml line 18: [input] is defined again"));
	assert(!strstr(logged, "using the defaults"));

	/* written back folded, with the comments, and completed */
	text = config_read_file(path, &size);
	assert(text);
	assert(count(text, "[display]") == 1 && count(text, "[input]") == 1 && count(text, "\nstereo =") == 1);
	assert(strstr(text, "# mine\n") && strstr(text, "# my width\n"));
	result = toml_parse(text, (int)size);
	assert(result.ok);
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		if (config_settings[index].platforms & CONFIG_PLATFORM)
			assert(toml_seek(result.toptab, config_settings[index].name).type != TOML_UNKNOWN);
	}
	toml_free(result);
	free(text);

	/* the settings screen's write lands in the one table */
	written = config_write_boolean("display.foveation", 0);
	assert(written);
	text = config_read_file(path, &size);
	assert(count(text, "\nfoveation =") == 1 && count(text, "[display]") == 1);
	result = toml_parse(text, (int)size);
	assert(result.ok && toml_seek(result.toptab, "display.foveation").u.boolean == 0);
	toml_free(result);
	free(text);

	/* a missing file is written with the defaults, and read back whole */
	unlink(path);
	reload();
	assert(config_real("display.render_quality") == 0.6);
	/* foveation is on by default in HEAD mode, with the eye passes through the
	rate maps: the configuration accepted on the headset (Task 10) */
	assert(config_boolean("display.foveation"));
	assert(config_boolean("debug.foveation_eye_passes"));
	assert(strstr(logged, "wrote the defaults"));
	reload();
	assert(config_real("display.render_quality") == 0.6);
	assert(!strstr(logged, "using the defaults") && !strstr(logged, "again"));
	text = config_read_file(path, &size);
	assert(count(text, "[display]") == 1 && count(text, "[input]") == 1);
	free(text);

	unlink(path);
	rmdir(directory);
}
#endif

int main(void)
{
	check_written_file();
	check_folding();
#ifdef HALO_IOS
	/* an Apple build's defaults: a fresh install neither downloads maps from
	a game's players nor helps send them (port_config.c's DEFAULT_MAP_*) */
	{
		char *text = config_default_text();

		assert(strstr(text, "\nmap_download = false\n") && strstr(text, "\nmap_seed = false\n"));
		assert(strstr(text, "\nmap_share = true\n"));
		puts("PASS: an Apple build's config.toml starts with map_download and map_seed off");
		return 0;
	}
#elif !defined(HALO_ILP32)
	{
		char *text = config_default_text();

		assert(strstr(text, "\nmap_download = true\n") && strstr(text, "\nmap_seed = true\n"));
	}
#endif
#ifdef HALO_ILP32
	check_reading();
	puts("PASS: config.toml written with each table once (iOS and visionOS settings), and repeats read as the first");
#else
	puts("PASS: config.toml written with each table once (desktop settings), and repeats read as the first");
#endif
	return 0;
}
