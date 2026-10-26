/* The few settings the host itself needs (theater mode's, host_theater.m),
read from config.toml in the data folder (the working directory). The game
owns the file: it writes every setting with its default and its description
(port/linux/src/port_config.c), so the host reads only the simple
"key = value" lines of one section, and writes only the one boolean the
Settings app's switch sets (host_texture_settings.m), keeping every other line. */
#include "host_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a benchmark suite's launch environment, which wins over the file as it does in the game: HALO_ and the
key in capitals, bar the two settings the game names otherwise */
static const char *launch_value(const char *name)
{
	const char *suite = getenv("HALO_BENCHMARK_SUITE"), *dot = strchr(name, '.');
	char variable[96];
	size_t index;

	if (!suite || !*suite || !dot)
		return NULL;
	if (!strcmp(name, "display.mode"))
		return getenv("HALO_DISPLAY_MODE");
	if (!strcmp(name, "network.lan_broadcast"))
		return getenv("HALO_NET_LAN_BROADCAST");
	if (strlen(dot + 1) + 6 > sizeof(variable))
		return NULL;
	memcpy(variable, "HALO_", 5);
	for (index = 0; dot[1 + index]; index++)
		variable[5 + index] = (char)toupper((unsigned char)dot[1 + index]);
	variable[5 + index] = 0;
	return getenv(variable);
}

/* the raw value text of section.key (quotes kept), or 0 if it isn't there */
static int config_value(const char *name, char *value, size_t size)
{
	const char *dot = strchr(name, '.');
	const char *launch = launch_value(name);
	char line[512], section[64] = "";
	FILE *file;
	int found = 0;

	if (!dot)
		return 0;
	if (launch)
	{
		snprintf(value, size, "%s", launch);
		return 1;
	}
	file = fopen("config.toml", "r");
	if (!file)
		return 0;
	while (!found && fgets(line, sizeof(line), file))
	{
		char *start = line, *end;

		while (isspace((unsigned char)*start))
			start++;
		if (*start == '#' || !*start)
			continue;
		if (*start == '[')
		{
			end = strchr(start, ']');
			if (end)
				snprintf(section, sizeof(section), "%.*s", (int)(end - start - 1), start + 1);
			continue;
		}
		if (strlen(section) != (size_t)(dot - name) || strncmp(section, name, (size_t)(dot - name)))
			continue;
		end = strchr(start, '=');
		if (!end)
			continue;
		{
			char *key_end = end;

			while (key_end > start && isspace((unsigned char)key_end[-1]))
				key_end--;
			if ((size_t)(key_end - start) != strlen(dot + 1) || strncmp(start, dot + 1, (size_t)(key_end - start)))
				continue;
		}
		start = end + 1;
		while (isspace((unsigned char)*start))
			start++;
		/* up to a comment: a # outside a string (a hand-edited
		"online = true # on" is true, as the game reads it) */
		{
			char quote = 0;

			for (end = start; *end && (quote || *end != '#'); end++)
			{
				if (quote == '"' && *end == '\\' && end[1])
					end++;
				else if (*end == quote)
					quote = 0;
				else if (!quote && (*end == '"' || *end == '\''))
					quote = *end;
			}
		}
		while (end > start && isspace((unsigned char)end[-1]))
			end--;
		snprintf(value, size, "%.*s", (int)(end - start), start);
		found = 1;
	}
	fclose(file);
	return found;
}

static int (*profile_value)(const char *name, char *text, size_t size);

void host_config_set_profile(int (*value)(const char *name, char *text, size_t size))
{
	profile_value = value;
}

double host_config_real(const char *name, double fallback)
{
	char value[64];

	if (config_value(name, value, sizeof(value)) || (profile_value && profile_value(name, value, sizeof(value))))
		return atof(value);
	return fallback;
}

void host_config_string(const char *name, const char *fallback, char *out, size_t size)
{
	char value[256];

	if (!config_value(name, value, sizeof(value)))
	{
		/* (a profile's string has no quotes) */
		if (!profile_value || !profile_value(name, out, size))
			snprintf(out, size, "%s", fallback);
		return;
	}
	if (value[0] == '"')
	{
		char *close = strchr(value + 1, '"');

		snprintf(out, size, "%.*s", close ? (int)(close - value - 1) : (int)strlen(value + 1), value + 1);
	}
	else
		snprintf(out, size, "%s", value);
}

int host_config_write_boolean(const char *name, int value)
{
	const char *dot = strchr(name, '.');
	char section[64], key[64], header[80], setting[96], line[512];
	FILE *in, *out;
	int in_section = 0, done = 0, seen_section = 0, ends_line = 1;
	size_t key_length;

	if (!dot || (size_t)(dot - name) >= sizeof(section) || strlen(dot + 1) >= sizeof(key))
		return 0;
	if (getenv("HALO_BENCHMARK_SUITE") && *getenv("HALO_BENCHMARK_SUITE"))
		return 0;   /* a benchmark suite leaves config.toml alone */
	memcpy(section, name, (size_t)(dot - name));
	section[dot - name] = 0;
	snprintf(key, sizeof(key), "%s", dot + 1);
	snprintf(header, sizeof(header), "[%s]", section);
	snprintf(setting, sizeof(setting), "%s = %s\n", key, value ? "true" : "false");
	key_length = strlen(key);
	if (!(out = fopen("config.toml.tmp", "w")))
		return 0;
	if ((in = fopen("config.toml", "r")))
	{
		while (fgets(line, sizeof(line), in))
		{
			const char *text = line + strspn(line, " \t");

			if (*text == '[')
			{
				if (in_section && !done)   /* the key was missing from its section: add it at the end */
				{
					fputs(setting, out);
					done = 1;
				}
				in_section = !strncmp(text, header, strlen(header));
				seen_section |= in_section;
			}
			else if (in_section && !done && !strncmp(text, key, key_length) &&
				(text[key_length] == ' ' || text[key_length] == '\t' || text[key_length] == '='))
			{
				fputs(setting, out);
				done = 1;
				continue;
			}
			fputs(line, out);
			ends_line = line[strlen(line) - 1] == '\n';
		}
		fclose(in);
	}
	if (!done && !ends_line)         /* a last line without its newline */
		fputs("\n", out);
	if (!done && seen_section)       /* the section was the file's last */
		fputs(setting, out);
	else if (!done)
		fprintf(out, "\n%s\n%s", header, setting);
	if (fclose(out) || rename("config.toml.tmp", "config.toml"))
	{
		remove("config.toml.tmp");
		return 0;
	}
	return 1;
}
