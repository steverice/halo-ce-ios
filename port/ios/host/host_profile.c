/* Device profiles: choosing and reading them (host_profile.h). Plain C with no Apple headers, so
   port/ios/tests/profile_probe.c runs it on any Mac. */
#include "host_profile.h"
#include "tomlc17.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the settings a profile may set (port/linux/src/port_profile_keys.h) */

enum profile_type
{
	_profile_boolean,
	_profile_integer,
	_profile_real,
	_profile_string,
};

struct profile_key
{
	const char *name;
	enum profile_type type;
	double minimum, maximum;
	const char *choices;
};

static const struct profile_key profile_keys[] =
{
#define PROFILE_BOOLEAN(name) { name, _profile_boolean, 0.0, 0.0, NULL },
#define PROFILE_INTEGER(name, minimum, maximum) { name, _profile_integer, minimum, maximum, NULL },
#define PROFILE_REAL(name, minimum, maximum) { name, _profile_real, minimum, maximum, NULL },
#define PROFILE_STRING(name, choices) { name, _profile_string, 0.0, 0.0, choices },
#define PROFILE_SCENE(name)
#include "../../linux/src/port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
};

/* those a profile may also set for a scene class, and the classes this build knows */
static const char *const scene_keys[] =
{
#define PROFILE_BOOLEAN(name)
#define PROFILE_INTEGER(name, minimum, maximum)
#define PROFILE_REAL(name, minimum, maximum)
#define PROFILE_STRING(name, choices)
#define PROFILE_SCENE(name) name,
#include "../../linux/src/port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
};

static const char *const scene_classes[] = { "menu", "cutscene" };

static int name_in(const char *name, const char *const *names, size_t count)
{
	size_t index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(names[index], name))
			return 1;
	}
	return 0;
}

#define NUMBER_OF_PROFILE_KEYS (sizeof(profile_keys) / sizeof(profile_keys[0]))

static const struct profile_key *profile_key(const char *name)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_PROFILE_KEYS; index++)
	{
		if (!strcmp(profile_keys[index].name, name))
			return &profile_keys[index];
	}
	return NULL;
}

/* whether text is one of choices ("a|b"; "#" there takes a whole number from 30 to 1000) */
static int string_is_choice(const char *text, const char *choices)
{
	size_t length = strlen(text);
	const char *choice = choices;

	if (!length || strpbrk(text, "|\t\n\"\\"))
		return 0;
	while (*choice)
	{
		size_t choice_length = strcspn(choice, "|");

		if (choice_length == 1 && *choice == '#')
		{
			char *end;
			long number = strtol(text, &end, 10);

			if (!*end && length <= 4 && text[0] >= '1' && text[0] <= '9' && number >= 30 && number <= 1000)
				return 1;
		}
		else if (choice_length == length && !strncmp(choice, text, length))
			return 1;
		choice += choice_length;
		if (*choice)
			choice++;
	}
	return 0;
}

/* ---------- reading */

struct profile_log
{
	void (*log)(const char *line);
};

static void profile_logf(const struct profile_log *log, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void profile_logf(const struct profile_log *log, const char *format, ...)
{
	char line[512];
	va_list arguments;

	if (!log->log)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	log->log(line);
}

/* the whole file, NUL terminated, or NULL (missing, unreadable or over the size limit); free() it */
static char *profile_read(const char *path, size_t *size, const struct profile_log *log, const char *label)
{
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		if (length > HOST_PROFILE_MAXIMUM_FILE_BYTES)
			profile_logf(log, "device profile: %s is %ld bytes, over the limit of %d; not read", label, length,
				HOST_PROFILE_MAXIMUM_FILE_BYTES);
		else if ((text = malloc((size_t)length + 1)) && fread(text, 1, (size_t)length, file) == (size_t)length)
		{
			text[length] = 0;
			*size = (size_t)length;
		}
		else
		{
			free(text);
			text = NULL;
		}
	}
	fclose(file);
	return text;
}

/* table's settings (its "section" tables' keys) added to profile as layer source; those that are no profile
   setting, of the wrong type or outside their range are left out and named in the log */
static void profile_add_settings(struct host_profile *profile, toml_datum_t table, const char *source,
	const char *label, const char *scene, const struct profile_log *log)
{
	int section_index;

	for (section_index = 0; section_index < table.u.tab.size; section_index++)
	{
		const char *section_name = table.u.tab.key[section_index];
		toml_datum_t section = table.u.tab.value[section_index];
		int key_index;

		/* [scene.menu.display]: a scene class's values, after the profile's own. A class this build does not
		   know (a later one, or one keyed to a level) is skipped and named */
		if (section.type == TOML_TABLE && !scene && !strcmp(section_name, "scene"))
		{
			int class_index;

			for (class_index = 0; class_index < section.u.tab.size; class_index++)
			{
				const char *class_name = section.u.tab.key[class_index];

				if (section.u.tab.value[class_index].type != TOML_TABLE ||
					!name_in(class_name, scene_classes, sizeof(scene_classes) / sizeof(scene_classes[0])))
					profile_logf(log, "device profile: %s: scene class \"%.40s\" is not one this build knows; ignored",
						label, class_name);
				else
					profile_add_settings(profile, section.u.tab.value[class_index], source, label, class_name, log);
			}
			continue;
		}
		/* (the file's own fields and tables: tools/profile_check.py checks those) */
		if (section.type != TOML_TABLE || !strcmp(section_name, "match") || !strcmp(section_name, "evidence") ||
			!strcmp(section_name, "scene"))
			continue;
		for (key_index = 0; key_index < section.u.tab.size; key_index++)
		{
			toml_datum_t datum = section.u.tab.value[key_index];
			struct host_profile_value value;
			const struct profile_key *key;
			int accepted = 0;

			memset(&value, 0, sizeof(value));
			if (snprintf(value.name, sizeof(value.name), "%s.%s", section_name, section.u.tab.key[key_index]) >=
				(int)sizeof(value.name) || !(key = profile_key(value.name)))
			{
				profile_logf(log, "device profile: %s line %d: %s.%s is not a setting a profile may set; ignored",
					label, datum.lineno, section_name, section.u.tab.key[key_index]);
				continue;
			}
			if (scene && !name_in(value.name, scene_keys, sizeof(scene_keys) / sizeof(scene_keys[0])))
			{
				profile_logf(log, "device profile: %s line %d: %s cannot change by scene class; ignored", label,
					datum.lineno, value.name);
				continue;
			}
			switch (key->type)
			{
			case _profile_boolean:
				if ((accepted = datum.type == TOML_BOOLEAN))
					snprintf(value.text, sizeof(value.text), "%s", datum.u.boolean ? "true" : "false");
				break;
			case _profile_integer:
				if ((accepted = datum.type == TOML_INT64 && (double)datum.u.int64 >= key->minimum &&
					(double)datum.u.int64 <= key->maximum))
					snprintf(value.text, sizeof(value.text), "%lld", (long long)datum.u.int64);
				break;
			case _profile_real:
			{
				double real = datum.type == TOML_FP64 ? datum.u.fp64 : (double)datum.u.int64;

				/* (a comparison with a NaN is false) */
				if ((accepted = (datum.type == TOML_FP64 || datum.type == TOML_INT64) && real >= key->minimum &&
					real <= key->maximum))
					snprintf(value.text, sizeof(value.text), "%.15g", real);
				break;
			}
			case _profile_string:
				if ((accepted = datum.type == TOML_STRING && string_is_choice(datum.u.s, key->choices)))
					snprintf(value.text, sizeof(value.text), "%s", datum.u.s);
				break;
			}
			if (!accepted)
			{
				profile_logf(log, "device profile: %s line %d: %s has a value of the wrong type or outside what it "
					"accepts; ignored", label, datum.lineno, value.name);
				continue;
			}
			if (profile->count == HOST_PROFILE_MAXIMUM_VALUES)
			{
				profile_logf(log, "device profile: %s: too many values; %s ignored", label, value.name);
				continue;
			}
			snprintf(value.source, sizeof(value.source), "%s", source);
			snprintf(value.scene, sizeof(value.scene), "%s", scene ? scene : "");
			profile->values[profile->count++] = value;
		}
	}
}

/* ---------- choosing */

struct profile_file
{
	char file[64];
	char name[64];
	char platform[12];
	int is_class;
	/* a class's GPU families, lowest to highest; 0 for no upper bound */
	int family_minimum, family_maximum;
	/* whether a model profile names the device's (or the selector's) model or chip */
	int names_device;
};

static int string_in_array(toml_datum_t array, const char *text)
{
	int index;

	if (array.type != TOML_ARRAY || !*text)
		return 0;
	for (index = 0; index < array.u.arr.size; index++)
	{
		if (array.u.arr.elem[index].type == TOML_STRING && !strcmp(array.u.arr.elem[index].u.s, text))
			return 1;
	}
	return 0;
}

/* the file's heading: 0 if it is no profile this build reads */
static int profile_describe(toml_datum_t table, const char *file, const char *model, const char *chip,
	struct profile_file *described, const struct profile_log *log)
{
	toml_datum_t schema = toml_seek(table, "schema"), name = toml_seek(table, "name"), kind = toml_seek(table, "kind");
	toml_datum_t platform = toml_seek(table, "match.platform"), family = toml_seek(table, "match.gpu_family");

	memset(described, 0, sizeof(*described));
	snprintf(described->file, sizeof(described->file), "%s", file);
	if (schema.type != TOML_INT64 || schema.u.int64 != 1)
	{
		profile_logf(log, "device profile: %s is not schema 1; not used", file);
		return 0;
	}
	if (name.type != TOML_STRING || kind.type != TOML_STRING || platform.type != TOML_STRING ||
		(strcmp(kind.u.s, "class") && strcmp(kind.u.s, "model")))
	{
		profile_logf(log, "device profile: %s lacks a name, a kind (\"class\" or \"model\") or match.platform; not used",
			file);
		return 0;
	}
	snprintf(described->name, sizeof(described->name), "%s", name.u.s);
	snprintf(described->platform, sizeof(described->platform), "%s", platform.u.s);
	described->is_class = !strcmp(kind.u.s, "class");
	if (described->is_class)
	{
		if (family.type != TOML_ARRAY || family.u.arr.size != 2 || family.u.arr.elem[0].type != TOML_INT64 ||
			family.u.arr.elem[1].type != TOML_INT64)
		{
			profile_logf(log, "device profile: %s: a class needs match.gpu_family = [lowest, highest]; not used", file);
			return 0;
		}
		described->family_minimum = (int)family.u.arr.elem[0].u.int64;
		described->family_maximum = (int)family.u.arr.elem[1].u.int64;
	}
	else
	{
		described->names_device = string_in_array(toml_seek(table, "match.models"), model) ||
			string_in_array(toml_seek(table, "match.chips"), chip);
	}
	return 1;
}

static int compare_names(const void *left, const void *right)
{
	return strcmp((const char *)left, (const char *)right);
}

void host_profile_resolve(const struct host_device *device, const char *folder, const char *drop_in_path,
	const char *selector, void (*log_line)(const char *line), struct host_profile *profile)
{
	enum { MAXIMUM_FILES = 128 };
	struct profile_log log = { log_line };
	char (*names)[64] = NULL;
	int name_count = 0, index;
	const char *model = device->model, *chip = device->chip;
	char platform[12];
	struct profile_file chosen_class, chosen_model;
	int have_class = 0, have_model = 0, class_distance = 0;
	DIR *directory;

	memset(profile, 0, sizeof(*profile));
	memset(&chosen_class, 0, sizeof(chosen_class));
	memset(&chosen_model, 0, sizeof(chosen_model));
	snprintf(platform, sizeof(platform), "%s", device->platform);
	if (selector && !strcmp(selector, "none"))
	{
		profile_logf(&log, "device profile: none (debug.device_profile = \"none\"): the built-in defaults");
		return;
	}
	if (selector && *selector)
	{
		/* (as that model or chip, whatever this is) */
		model = chip = selector;
		profile_logf(&log, "device profile: choosing as \"%s\" (debug.device_profile), not as this device", selector);
	}

	directory = folder ? opendir(folder) : NULL;
	if (directory)
	{
		struct dirent *entry;

		names = calloc(MAXIMUM_FILES, sizeof(*names));
		while (names && name_count < MAXIMUM_FILES && (entry = readdir(directory)))
		{
			size_t length = strlen(entry->d_name);

			/* (TEMPLATE.toml is the contributor's starting point, not a profile) */
			if (length > 5 && length < sizeof(names[0]) && !strcmp(entry->d_name + length - 5, ".toml") &&
				strcmp(entry->d_name, "TEMPLATE.toml"))
				snprintf(names[name_count++], sizeof(names[0]), "%s", entry->d_name);
		}
		closedir(directory);
		/* (in name order, so the choice never depends on the folder's own order) */
		if (names)
			qsort(names, (size_t)name_count, sizeof(names[0]), compare_names);
	}
	else
	{
		profile_logf(&log, "device profile: no profile folder%s%s; the built-in defaults", folder ? " at " : "",
			folder ? folder : "");
	}

	/* two passes: the model profile first, since under a selector its platform is the one to choose the class
	   for; then the class */
	for (index = 0; index < name_count * 2; index++)
	{
		int class_pass = index >= name_count;
		const char *file = names[index % name_count];
		char path[1024];
		size_t size = 0;
		char *text;
		toml_result_t result;
		struct profile_file described;

		snprintf(path, sizeof(path), "%s/%s", folder, file);
		text = profile_read(path, &size, &log, file);
		if (!text)
			continue;
		result = toml_parse(text, (int)size);
		if (!result.ok)
		{
			if (!class_pass)
				profile_logf(&log, "device profile: %s does not parse (%s); not used", file, result.errmsg);
		}
		else if (profile_describe(result.toptab, file, model, chip, &described, class_pass ? &(struct profile_log){ NULL } : &log))
		{
			if (!class_pass && !described.is_class && described.names_device &&
				(selector && *selector ? 1 : !strcmp(described.platform, platform)))
			{
				if (have_model)
					profile_logf(&log, "device profile: %s also names %s; %s is used", file, model, chosen_model.file);
				else
				{
					chosen_model = described;
					have_model = 1;
					if (selector && *selector)
						snprintf(platform, sizeof(platform), "%s", described.platform);
				}
			}
			else if (class_pass && described.is_class && !strcmp(described.platform, platform))
			{
				/* the class whose families hold this GPU's; failing that the nearest, so a GPU no class
				   names (none read, or one below every class) still gets its platform's profile */
				int family = device->gpu_family;
				int distance = family < described.family_minimum ? described.family_minimum - family :
					described.family_maximum && family > described.family_maximum ? family - described.family_maximum : 0;

				if (!have_class || distance < class_distance)
				{
					chosen_class = described;
					class_distance = distance;
					have_class = 1;
				}
			}
		}
		toml_free(result);
		free(text);
	}

	if (!have_model && (!selector || !*selector) && host_profile_inherit(device, folder, log_line, profile))
	{
		snprintf(chosen_class.file, sizeof(chosen_class.file), "%s", profile->class_file);
		snprintf(chosen_class.name, sizeof(chosen_class.name), "%s", profile->class_name);
		snprintf(chosen_model.file, sizeof(chosen_model.file), "%s", profile->model_file);
		snprintf(chosen_model.name, sizeof(chosen_model.name), "%s", profile->model_name);
		have_class = have_model = 1;
		class_distance = 0;
	}

	/* the layers, lowest first: the class, the model, the drop-in file */
	for (index = 0; index < 3; index++)
	{
		const struct profile_file *chosen = index == 0 ? &chosen_class : &chosen_model;
		char path[1024], source[80];
		const char *label;
		size_t size = 0;
		char *text;
		toml_result_t result;

		if (index < 2 && profile->inherited)
			continue;
		if (index == 2)
		{
			if (!drop_in_path)
				continue;
			snprintf(path, sizeof(path), "%s", drop_in_path);
			snprintf(source, sizeof(source), "DROP-IN FILE");
			label = "device-profile.toml";
		}
		else
		{
			if (!(index == 0 ? have_class : have_model))
				continue;
			snprintf(path, sizeof(path), "%s/%s", folder, chosen->file);
			snprintf(source, sizeof(source), "%s %.60s", index == 0 ? "class" : "model", chosen->name);
			label = chosen->file;
		}
		text = profile_read(path, &size, &log, label);
		if (!text)
			continue;
		result = toml_parse(text, (int)size);
		if (result.ok)
		{
			int before = profile->count;

			profile_add_settings(profile, result.toptab, source, label, NULL, &log);
			if (index == 2)
			{
				profile->drop_in = 1;
				profile_logf(&log, "device profile: DROP-IN FILE device-profile.toml IN FORCE (%d values) over the "
					"app's own profile: delete it from the data folder to go back", profile->count - before);
			}
		}
		else if (index == 2)
			profile_logf(&log, "device profile: the drop-in file device-profile.toml does not parse (%s); not used",
				result.errmsg);
		toml_free(result);
		free(text);
	}
	if (have_class)
	{
		snprintf(profile->class_name, sizeof(profile->class_name), "%s", chosen_class.name);
		snprintf(profile->class_file, sizeof(profile->class_file), "%s", chosen_class.file);
	}
	if (have_model)
	{
		snprintf(profile->model_name, sizeof(profile->model_name), "%s", chosen_model.name);
		snprintf(profile->model_file, sizeof(profile->model_file), "%s", chosen_model.file);
	}
	if (have_class && class_distance)
		profile_logf(&log, "device profile: no %s class names GPU family %d; the nearest, \"%s\", is used", platform,
			device->gpu_family, chosen_class.name);
	if (directory)
	{
		char class_text[160] = "no class profile", model_text[160] = "no model profile";

		if (have_class)
			snprintf(class_text, sizeof(class_text), "class \"%s\" (%s)", chosen_class.name, chosen_class.file);
		if (have_model)
			snprintf(model_text, sizeof(model_text), "model \"%s\" (%s)", chosen_model.name, chosen_model.file);
		profile_logf(&log, "device profile: %s, %s", class_text, model_text);
	}
	free(names);
}

int host_profile_value(const struct host_profile *profile, const char *name, char *text, size_t size)
{
	int index;

	for (index = profile->count - 1; index >= 0; index--)
	{
		if (!profile->values[index].scene[0] && !strcmp(profile->values[index].name, name))
		{
			snprintf(text, size, "%s", profile->values[index].text);
			return 1;
		}
	}
	return 0;
}

size_t host_profile_encode(const struct host_profile *profile, char *text, size_t size)
{
	size_t length = 0;
	int index;

	if (size)
		text[0] = 0;
	for (index = 0; index < profile->count; index++)
	{
		const struct host_profile_value *value = &profile->values[index];
		int written = snprintf(text + length, size - length, "%s\t%s\t%s%s%s\n", value->name, value->text, value->source,
			value->scene[0] ? "\t" : "", value->scene);

		if (written < 0 || (size_t)written >= size - length)
		{
			text[length] = 0;
			break;
		}
		length += (size_t)written;
	}
	return length;
}
