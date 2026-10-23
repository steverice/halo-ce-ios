/* The settings' layers (port/linux/src/port_config.c): the built-in default, the device's profile over it
(HALO_DEVICE_PROFILE_VALUES, as the iOS host passes it), a value somebody chose in config.toml over that, and the
environment over all; config.origins, which tells a chosen value from a default the game wrote; and a scene
class's values. Each case is a sequence of launches over one data folder, as a device sees them. Built by
tools/ios_test.py with HALO_ILP32, the Apple builds' settings. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* platform.h reads the Xbox SDK's headers; port_config.c needs only its log */
#define __HALO_LINUX_PLATFORM_H
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static char logged[1 << 18];

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

static int external_active;
int host_external_active(void) { return external_active; }

#include "port_config.c"
#include "tomlc17.c"

static char directory[64], path[128], origins[128];

static void write_text(const char *file, const char *text)
{
	FILE *out = fopen(file, "wb");

	assert(out);
	fputs(text, out);
	fclose(out);
}

static char *read_text(const char *file)
{
	size_t size = 0;
	char *text = config_read_file(file, &size);

	return text ? text : strdup("");
}

static int file_has(const char *file, const char *needle)
{
	char *text = read_text(file);
	int found = strstr(text, needle) != NULL;

	free(text);
	return found;
}

static int count(const char *text, const char *needle)
{
	int found = 0;

	for (text = strstr(text, needle); text; text = strstr(text + 1, needle))
		found++;
	return found;
}

/* the next launch: everything the game keeps in memory forgotten, with this profile from the host ("" for none) */
static void launch(const char *profile)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		struct config_setting_state *state = &config_states[index];
		int layer;

		for (layer = 0; layer < state->profile_count; layer++)
		{
			free(state->profile[layer].text);
			free(state->profile[layer].source);
		}
		for (layer = 0; layer < NUMBER_OF_CONFIG_SCENES; layer++)
		{
			free(state->scene[layer].text);
			free(state->scene[layer].source);
			free(state->scene_value[layer].string);
			free(state->external[layer].text);
			free(state->external[layer].source);
			free(state->external_value[layer].string);
		}
		free(state->device.string);
		free(state->device_literal);
		free(state->origin_literal);
		memset(state, 0, sizeof(*state));
	}
	if (*profile)
		setenv("HALO_DEVICE_PROFILE_VALUES", profile, 1);
	else
		unsetenv("HALO_DEVICE_PROFILE_VALUES");
	config_loaded = 0;
	logged[0] = 0;
	/* (any question loads) */
	(void)config_boolean("display.vsync");
}

static void fresh(void)
{
	unlink(path);
	unlink(origins);
}

#define TV_CLASS "display.renderer\tmetal\tclass Apple TV\n" "display.render_height\t1080\tclass Apple TV\n" \
	"display.upscaled_textures\tfalse\tclass Apple TV\n"
#define TV_MODEL TV_CLASS "display.render_scale\t0.5\tmodel Apple TV HD\n" \
	"display.render_scale\t0.667\tmodel Apple TV HD\tmenu\n" "display.render_scale\t0.5\tmodel Apple TV HD\tcutscene\n"

/* the layers on a first launch, and what the file and the log then hold */
static void check_first_launch(void)
{
	char *text;

	fresh();
	launch(TV_MODEL);
	assert(!strcmp(config_string("display.renderer"), "metal") && config_integer("display.render_height") == 1080);
	assert(!config_boolean("display.upscaled_textures") && config_real("display.render_scale") == 0.5);
	/* (a setting no profile names: its default) */
	assert(config_integer("display.shadow_map_size") == 512 && config_real("display.render_quality") == 0.6);
	/* the file is written with the values in force, every line live as it always was */
	assert(file_has(path, "\nrenderer = \"metal\"\n") && file_has(path, "\nrender_height = 1080\n"));
	assert(file_has(path, "\nupscaled_textures = false\n") && file_has(path, "\nrender_scale = 0.5\n"));
	assert(file_has(path, "\nshadow_map_size = 512\n") && file_has(path, "\nrender_quality = 0.6\n"));
	/* and each recorded as a default the game wrote */
	assert(file_has(origins, "display.render_height\tdefault\t1080\n") && file_has(origins, "display.renderer\tdefault\t\"metal\"\n"));
	assert(file_has(origins, "display.shadow_map_size\tdefault\t512\n") && !file_has(origins, "\tchosen"));
	/* the log names the layer of each value a profile set, and only settings a profile may set */
	assert(strstr(logged, "settings: display.render_height = 1080 [class Apple TV] over 0 [default]"));
	assert(strstr(logged, "settings: display.render_scale = 0.5 [model Apple TV HD] over 1.0 [default]; menu 0.667 "
		"[model Apple TV HD]; cutscene 0.5 [model Apple TV HD]"));
	assert(strstr(logged, "settings: display.renderer = \"metal\" [class Apple TV] over \"metal\" [default]"));
	assert(!strstr(logged, "settings: display.shadow_map_size") && !strstr(logged, "settings: network.") &&
		!strstr(logged, "settings: paths."));
	/* Defaults in the menus shows the device's value */
	{
		char shown[64];

		assert(config_default("display.render_height", shown, sizeof(shown)) && !strcmp(shown, "1080"));
		assert(config_default("display.renderer", shown, sizeof(shown)) && !strcmp(shown, "metal"));
		assert(config_default("display.shadow_map_size", shown, sizeof(shown)) && !strcmp(shown, "512"));
	}

	/* the same launch again changes nothing on disk and says nothing new */
	text = read_text(path);
	launch(TV_MODEL);
	assert(file_has(path, text) && !strstr(logged, "it is now") && !strstr(logged, "added"));
	assert(config_integer("display.render_height") == 1080);
	free(text);

	/* a model's value over its class's, and the layers in the log */
	launch(TV_CLASS "display.render_height\t720\tmodel Small\n");
	assert(config_integer("display.render_height") == 720);
	assert(strstr(logged, "settings: display.render_height = 720 [model Small] over 1080 [class Apple TV] over 0 [default]"));
}

/* a profile that changes (an app update, a drop-in file arriving and leaving) reaches a setting nobody chose,
and never one somebody did */
static void check_profile_changes(void)
{
	fresh();
	launch(TV_MODEL);
	/* the player turns upscaled textures on with the Settings app's switch: the host writes the line */
	{
		char *text = read_text(path), *changed = config_text_set(text, "display.upscaled_textures", "upscaled_textures = true\n");

		write_text(path, changed);
		free(text);
		free(changed);
	}
	/* the level load's reread sees it at once */
	assert(config_reload_boolean("display.upscaled_textures"));
	launch(TV_MODEL);
	assert(config_boolean("display.upscaled_textures"));
	assert(file_has(origins, "display.upscaled_textures\tchosen\n"));
	assert(strstr(logged, "settings: display.upscaled_textures = true [config.toml, chosen] over false [class Apple TV] "
		"over true [default]"));

	/* a drop-in file arrives: 900 lines, upscaled textures off again, a new shadow size */
	launch(TV_MODEL "display.render_height\t900\tDROP-IN FILE\n" "display.upscaled_textures\tfalse\tDROP-IN FILE\n"
		"display.shadow_map_size\t256\tDROP-IN FILE\n");
	assert(config_integer("display.render_height") == 900 && config_integer("display.shadow_map_size") == 256);
	assert(strstr(logged, "settings: display.render_height was the default 1080 in config.toml; it is now this device's 900"));
	assert(file_has(path, "\nrender_height = 900\n") && file_has(path, "\nshadow_map_size = 256\n"));
	assert(strstr(logged, "settings: display.render_height = 900 [DROP-IN FILE] over 1080 [class Apple TV] over 0 [default]"));
	/* (the player's choice stands, in the game and in the file) */
	assert(config_boolean("display.upscaled_textures") && file_has(path, "\nupscaled_textures = true\n"));
	assert(config_reload_boolean("display.upscaled_textures"));

	/* the drop-in file is deleted: the app's own values come back */
	launch(TV_MODEL);
	assert(config_integer("display.render_height") == 1080 && config_integer("display.shadow_map_size") == 512);
	assert(file_has(path, "\nrender_height = 1080\n") && file_has(path, "\nshadow_map_size = 512\n"));
	assert(config_boolean("display.upscaled_textures"));

	/* debug.device_profile = "none" (the host passes no values): the built-in defaults, the choice still kept */
	launch("");
	assert(config_integer("display.render_height") == 0 && !strcmp(config_string("display.renderer"), "metal"));
	assert(config_real("display.render_scale") == 1.0 && config_boolean("display.upscaled_textures"));
	assert(file_has(path, "\nrender_height = 0\n") && file_has(path, "\nrenderer = \"metal\"\n"));
	launch(TV_MODEL);
	assert(config_integer("display.render_height") == 1080 && !strcmp(config_string("display.renderer"), "metal"));
}

/* the level-load reread of a setting the profile turns off and nobody chose (the Apple TV's upscaled textures) */
static void check_reload(void)
{
	fresh();
	launch(TV_MODEL);
	assert(!config_boolean("display.upscaled_textures"));
	assert(!config_reload_boolean("display.upscaled_textures"));
	/* and with the file gone under the running game (tvOS empties Caches): still the device's value */
	unlink(path);
	assert(!config_reload_boolean("display.upscaled_textures"));
	/* the environment still wins */
	setenv("HALO_UPSCALED_TEXTURES", "true", 1);
	assert(config_reload_boolean("display.upscaled_textures"));
	unsetenv("HALO_UPSCALED_TEXTURES");
}

/* an install from before profiles: every setting live at whatever its build's default was, and no config.origins */
static void check_old_install(void)
{
	fresh();
	write_text(path,
		"[display]\n"
		"# a comment of mine\n"
		"render_height = 0\n"            /* the default: written, not chosen */
		"frame_pacing = \"tick\"\n"      /* a default main once shipped */
		"foveation = false\n"            /* another */
		"anisotropic_filtering = 4\n"    /* never a default: chosen */
		"shadow_map_size = 512\n"
		"render_scale = 1.0\n"
		"renderer = \"metal\"\n"         /* the default and what the profile says: follows it from now on */
		"upscaled_textures = true\n"
		"\n[input]\n"
		"turn = \"smooth\"\n"            /* once the default, but a player's setting: kept */
		"\n[network]\n"
		"online = true\n");              /* likewise */
	launch(TV_MODEL);
	assert(config_integer("display.render_height") == 1080 && !strcmp(config_string("display.frame_pacing"), "off"));
	assert(config_boolean("display.foveation") && config_integer("display.anisotropic_filtering") == 4);
	assert(config_real("display.render_scale") == 0.5 && !config_boolean("display.upscaled_textures"));
	assert(!strcmp(config_string("input.turn"), "smooth") && config_boolean("network.online"));
	/* said once, with what to do about it */
	assert(strstr(logged, "settings: display.frame_pacing = \"tick\" in config.toml is a default an earlier version wrote, "
		"not a choice: it becomes this device's \"off\". If you chose \"tick\", set it again and it stays"));
	assert(strstr(logged, "settings: display.render_height = 0 in config.toml is a default an earlier version wrote"));
	assert(strstr(logged, "settings: display.upscaled_textures = true in config.toml is a default"));
	assert(!strstr(logged, "input.turn = ") && !strstr(logged, "network.online = "));
	assert(file_has(path, "# a comment of mine\nrender_height = 1080\nframe_pacing = \"off\"\nfoveation = true\n"
		"anisotropic_filtering = 4\n"));
	assert(file_has(origins, "display.anisotropic_filtering\tchosen\n") && file_has(origins, "input.turn\tchosen\n"));
	assert(file_has(origins, "display.renderer\tdefault\t\"metal\"\n"));
	launch(TV_MODEL);
	assert(!strstr(logged, "an earlier version wrote") && !strstr(logged, "it is now"));

	/* the player who did want "tick" sets it again, by hand or in a menu: it stays */
	{
		char *text = read_text(path), *changed = config_text_set(text, "display.frame_pacing", "frame_pacing = \"tick\"\n");

		write_text(path, changed);
		free(text);
		free(changed);
	}
	launch(TV_MODEL);
	assert(!strcmp(config_string("display.frame_pacing"), "tick") && file_has(origins, "display.frame_pacing\tchosen\n"));
	launch(TV_MODEL);
	assert(!strcmp(config_string("display.frame_pacing"), "tick") && file_has(path, "\nframe_pacing = \"tick\"\n"));
}

/* tools: a run's --set written into the file (mac_run.merge_config), a key a session removes
(device-run.py remove_keys), a runner's whole file with every value marked chosen (mac_run.py prepare), and
config.origins lost */
static void check_tools(void)
{
	char *text, *changed;
	size_t before;

	fresh();
	launch(TV_MODEL);
	/* --set display.render_scale=1.0 and --set display.upscaled_textures=true: both in force */
	text = read_text(path);
	changed = config_text_set(text, "display.render_scale", "render_scale = 1.0\n");
	free(text);
	text = config_text_set(changed, "display.upscaled_textures", "upscaled_textures = true\n");
	write_text(path, text);
	free(changed);
	free(text);
	launch(TV_MODEL);
	assert(config_real("display.render_scale") == 1.0 && config_boolean("display.upscaled_textures"));
	/* (a chosen render scale is the render scale in every scene) */
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 1.0);
	assert(strstr(logged, "menu 0.667 [model Apple TV HD] (not used: the value is set)"));

	/* the next run does not set them, and removes their lines with their comments: the device's values again,
	and the file no longer than it was */
	text = read_text(path);
	before = strlen(text);
	{
		struct config_text out = { NULL, 0, 0 };
		const char *line;

		for (line = text; *line;)
		{
			const char *end = line + strcspn(line, "\n");
			const char *next = *end ? end + 1 : end;
			char *copy = config_copy(line, (size_t)(next - line));

			if (strncmp(line, "render_scale =", 14) && strncmp(line, "upscaled_textures =", 19))
				config_append(&out, copy);
			free(copy);
			line = next;
		}
		write_text(path, out.buffer);
		free(out.buffer);
	}
	free(text);
	launch(TV_MODEL);
	assert(config_real("display.render_scale") == 0.5 && !config_boolean("display.upscaled_textures"));
	assert(file_has(path, "\nrender_scale = 0.5\n") && file_has(origins, "display.render_scale\tdefault\t0.5\n"));
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 0.667);
	text = read_text(path);
	assert(count(text, "\nrender_scale =") == 1 && count(text, "\nupscaled_textures =") == 1);
	free(text);
	(void)before;

	/* a runner's file: every value marked chosen, the ones equal to a default included, so no profile moves one */
	fresh();
	write_text(path, "[display]\nrender_height = 0\nrenderer = \"gl\"\nshadow_map_size = 128\nupscaled_textures = true\n"
		"render_scale = 1.0\n");
	write_text(origins, "display.render_height\tchosen\ndisplay.renderer\tchosen\ndisplay.shadow_map_size\tchosen\n"
		"display.upscaled_textures\tchosen\ndisplay.render_scale\tchosen\n");
	launch(TV_MODEL "display.shadow_map_size\t256\tmodel Apple TV HD\n");
	assert(config_integer("display.render_height") == 0 && !strcmp(config_string("display.renderer"), "gl"));
	assert(config_integer("display.shadow_map_size") == 128 && config_boolean("display.upscaled_textures"));
	assert(config_real("display.render_scale") == 1.0 && config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 1.0);
	assert(file_has(path, "render_height = 0\nrenderer = \"gl\"\nshadow_map_size = 128\nupscaled_textures = true\n"));
	assert(!strstr(logged, "it is now") && !strstr(logged, "an earlier version wrote"));

	/* config.origins lost (tvOS emptied part of Caches): values the profile wrote are still this device's, so
	they go on following it; a chosen one that is no default stays chosen */
	fresh();
	launch(TV_MODEL);
	text = read_text(path);
	changed = config_text_set(text, "display.anisotropic_filtering", "anisotropic_filtering = 2\n");
	write_text(path, changed);
	free(text);
	free(changed);
	launch(TV_MODEL);
	unlink(origins);
	launch(TV_CLASS "display.render_height\t720\tmodel Apple TV HD\n");
	assert(config_integer("display.anisotropic_filtering") == 2 && file_has(origins, "display.anisotropic_filtering\tchosen\n"));
	assert(!strcmp(config_string("display.renderer"), "metal") && file_has(origins, "display.renderer\tdefault\t\"metal\"\n"));
	/* (1080 was the old profile's value and is not this one's: with the record lost it reads as a choice. The
	log shows it as config.toml's, and Defaults in the menus hands it back) */
	assert(config_integer("display.render_height") == 1080 && file_has(origins, "display.render_height\tchosen\n"));
	assert(strstr(logged, "settings: display.render_height = 1080 [config.toml, chosen] over 720 [model Apple TV HD]"));
}

/* the menus: a row changed and saved is the player's; Defaults then OK gives it back to the profile */
static void check_menus(void)
{
	char shown[64];

	fresh();
	launch(TV_MODEL);
	assert(config_write("display.shadow_map_size", "1024"));
	assert(config_integer("display.shadow_map_size") == 1024 && file_has(origins, "display.shadow_map_size\tchosen\n"));
	assert(config_write_boolean("display.upscaled_textures", 1) && file_has(origins, "display.upscaled_textures\tchosen\n"));
	launch(TV_MODEL "display.shadow_map_size\t256\tmodel Apple TV HD\n");
	assert(config_integer("display.shadow_map_size") == 1024 && config_boolean("display.upscaled_textures"));
	/* Defaults shows the device's values, and OK writes them */
	assert(config_default("display.shadow_map_size", shown, sizeof(shown)) && !strcmp(shown, "256"));
	assert(config_write("display.shadow_map_size", shown));
	assert(config_default("display.upscaled_textures", shown, sizeof(shown)) && !strcmp(shown, "false"));
	assert(config_write("display.upscaled_textures", shown));
	assert(file_has(origins, "display.shadow_map_size\tdefault\t256\n") && !file_has(origins, "display.upscaled_textures\tchosen"));
	/* so a later profile reaches them again */
	launch(TV_MODEL "display.shadow_map_size\t128\tmodel Apple TV HD\n");
	assert(config_integer("display.shadow_map_size") == 128 && !config_boolean("display.upscaled_textures"));
	/* a setting no profile names behaves the same against its built-in default */
	assert(config_write("input.snap_angle", "45"));
	assert(file_has(origins, "input.snap_angle\tchosen\n"));
	assert(config_write("input.snap_angle", "30"));
	assert(file_has(origins, "input.snap_angle\tdefault\t30.0\n"));
}

/* the environment over everything, and what a profile may not do */
static void check_environment_and_refusals(void)
{
	fresh();
	setenv("HALO_RENDER_HEIGHT", "480", 1);
	launch(TV_MODEL);
	assert(config_integer("display.render_height") == 480);
	assert(strstr(logged, "settings: display.render_height = 480 [environment HALO_RENDER_HEIGHT] over 1080 [class Apple TV] "
		"over 0 [default]"));
	/* (the file keeps the device's value: the environment is one run's) */
	assert(file_has(path, "\nrender_height = 1080\n"));
	unsetenv("HALO_RENDER_HEIGHT");
	/* an environment render scale is the scale in every scene */
	setenv("HALO_RENDER_SCALE", "0.75", 1);
	launch(TV_MODEL);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 0.75);
	unsetenv("HALO_RENDER_SCALE");

	/* settings that are the player's, a setting that does not exist, a scene class this build has not, and a
	setting that cannot change by scene: each ignored and named */
	fresh();
	launch("network.coop_public\ttrue\tmodel Bad\n" "input.turn\tsmooth\tmodel Bad\n" "paths.data\t/tmp\tmodel Bad\n"
		"debug.exit_after\t1.0\tmodel Bad\n" "display.nonsense\t1\tmodel Bad\n" "display.stereo\thead\tmodel Bad\n"
		"display.render_scale\t0.5\tmodel Bad\tloading\n" "display.shadow_map_size\t128\tmodel Bad\tmenu\n"
		"display.shadow_map_size\t256\tmodel Bad\n" "broken line\n\n");
	assert(!config_boolean("network.coop_public") && !strcmp(config_string("input.turn"), "snap"));
	assert(config_real("debug.exit_after") == 0.0);
	assert(!strcmp(config_string("display.stereo"), "off") && config_integer("display.shadow_map_size") == 256);
	assert(strstr(logged, "the device profile sets network.coop_public, which is not a setting a profile may set"));
	assert(strstr(logged, "the device profile sets paths.data,") && strstr(logged, "the device profile sets display.stereo,"));
	assert(strstr(logged, "sets display.render_scale for scene class loading") &&
		strstr(logged, "sets display.shadow_map_size for scene class menu"));
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 1.0);
}

/* scene classes: the profile's value for the class, the gameplay value elsewhere */
static void check_scenes(void)
{
	char text[256];

	fresh();
	launch(TV_MODEL);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_GAMEPLAY) == 0.5);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 0.667);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_CUTSCENE) == 0.5);
	assert(config_scene_real("display.render_scale", 99) == 0.5 && config_scene_real("display.render_scale", -1) == 0.5);
	/* (a setting with no scene value) */
	assert(config_scene_real("display.render_quality", CONFIG_SCENE_MENU) == 0.6);
	assert(config_scene_describe(CONFIG_SCENE_MENU, text, sizeof(text)) &&
		!strcmp(text, "display.render_scale 0.667 [model Apple TV HD]"));
	assert(!config_scene_describe(CONFIG_SCENE_GAMEPLAY, text, sizeof(text)));
	assert(!strcmp(config_scene_name(CONFIG_SCENE_CUTSCENE), "cutscene"));
	/* a render height for a scene class (an integer setting) */
	launch(TV_CLASS "display.render_height\t720\tmodel 4K\n" "display.render_height\t1080\tmodel 4K\tmenu\n");
	assert(config_scene_integer("display.render_height", CONFIG_SCENE_GAMEPLAY) == 720);
	assert(config_scene_integer("display.render_height", CONFIG_SCENE_MENU) == 1080);
	assert(config_scene_integer("display.render_height", CONFIG_SCENE_CUTSCENE) == 720);
	assert(strstr(logged, "settings: display.render_height = 720 [model 4K] over 1080 [class Apple TV] over 0 [default]; "
		"menu 1080 [model 4K]"));
	launch(TV_MODEL);
	/* a drop-in file's scene value over the model's */
	launch(TV_MODEL "display.render_scale\t1\tDROP-IN FILE\tmenu\n");
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 1.0);
	/* a scene class's frame rate: the class's own value only, since the setting itself is the Mac app's cap */
	launch(TV_MODEL "display.frame_rate\t30\tmodel Apple TV HD\tcutscene\n");
	assert(config_scene_string("display.frame_rate", CONFIG_SCENE_CUTSCENE, text, sizeof(text)) && !strcmp(text, "30"));
	assert(!config_scene_string("display.frame_rate", CONFIG_SCENE_MENU, text, sizeof(text)) && !strcmp(text, "display"));
	assert(!config_scene_string("display.frame_rate", CONFIG_SCENE_GAMEPLAY, text, sizeof(text)));
	assert(config_scene_describe(CONFIG_SCENE_CUTSCENE, text, sizeof(text)) &&
		!strcmp(text, "display.render_scale 0.5 [model Apple TV HD], display.frame_rate \"30\" [model Apple TV HD]"));
	/* (one the player set is the rate everywhere: no class's value applies) */
	assert(config_write("display.frame_rate", "120"));
	assert(!config_scene_string("display.frame_rate", CONFIG_SCENE_CUTSCENE, text, sizeof(text)) && !strcmp(text, "120"));
	/* no profile: no scene values */
	launch("");
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 1.0);
	assert(!config_scene_describe(CONFIG_SCENE_MENU, text, sizeof(text)));
}

/* a built-in default that changes between builds reaches an install that never chose the setting (the bug of
2026-10-13), with no profile at all */
static void check_changed_default(void)
{
	fresh();
	launch("");
	assert(file_has(path, "\nsnap_angle = 30.0\n") && file_has(origins, "input.snap_angle\tdefault\t30.0\n"));
	/* as an earlier build with another default left it */
	{
		char *text = read_text(path), *changed = config_text_set(text, "input.snap_angle", "snap_angle = 45.0\n");
		char *record = read_text(origins), *line = strstr(record, "input.snap_angle\tdefault\t30.0");

		write_text(path, changed);
		assert(line);
		memcpy(line + strlen("input.snap_angle\tdefault\t"), "45.0", 4);
		write_text(origins, record);
		free(text);
		free(changed);
		free(record);
	}
	launch("");
	assert(config_real("input.snap_angle") == 30.0 && file_has(path, "\nsnap_angle = 30.0\n"));
	assert(strstr(logged, "settings: input.snap_angle was the default 45.0 in config.toml; it is now this device's 30.0"));
}

/* the renderer's default was OpenGL ES: a Mac's config.toml from before config.origins holds it, written by the game,
   and gets Metal; one marked chosen keeps OpenGL ES */
static void check_renderer_default(void)
{
	fresh();
	write_text(path, "[display]\nrenderer = \"gl\"\n");
	launch("");
	assert(!strcmp(config_string("display.renderer"), "metal") && file_has(path, "\nrenderer = \"metal\"\n"));
	assert(strstr(logged, "settings: display.renderer = \"gl\" in config.toml is a default an earlier version wrote"));
	fresh();
	write_text(path, "[display]\nrenderer = \"gl\"\n");
	write_text(origins, "display.renderer\tchosen\n");
	launch("");
	assert(!strcmp(config_string("display.renderer"), "gl") && file_has(path, "\nrenderer = \"gl\"\n"));
}

static void check_external(void)
{
	const char *profile = TV_MODEL
		"display.render_scale\t0.8\tclass Phone\t\texternal\n"
		"display.render_scale\t0.6\tmodel Phone\t\texternal\n"
		"display.render_scale\t0.4\tmodel Phone\tcutscene\texternal\n"
		"display.anti_aliasing\tfxaa\tmodel Phone\t\texternal\n";
	char *before, *before_origins, shown[32];
	fresh();
	external_active = 1;
	launch(profile);
	assert(config_real("display.render_scale") == 0.5);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_GAMEPLAY) == 0.6);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 0.6);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_CUTSCENE) == 0.4);
	assert(config_scene_string("display.anti_aliasing", CONFIG_SCENE_GAMEPLAY, shown, sizeof(shown)));
	assert(!strcmp(shown, "fxaa"));
	assert(file_has(path, "\nrender_scale = 0.5\n"));
	assert(file_has(origins, "display.render_scale\tdefault\t0.5\n"));
	before = read_text(path); before_origins = read_text(origins);
	external_active = 0;
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_MENU) == 0.667);
	external_active = 1;
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_GAMEPLAY) == 0.6);
	assert(file_has(path, before) && file_has(origins, before_origins));
	free(before); free(before_origins);
	assert(config_write("display.render_scale", "0.9"));
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_CUTSCENE) == 0.9);
	setenv("HALO_RENDER_SCALE", "0.75", 1);
	launch(profile);
	assert(config_scene_real("display.render_scale", CONFIG_SCENE_CUTSCENE) == 0.75);
	unsetenv("HALO_RENDER_SCALE");
	external_active = 0;
}

/* display.effect_resolution was a boolean beside display.mirror_resolution: the saved choice carries over */
static void check_effect_resolution_migration(void)
{
	static const struct { const char *effect, *mirror, *expected; int still_default; } cases[] = {
		{ "false", "\"half\"", "xbox", 0 }, { "false", "\"full\"", "xbox", 0 },
		{ "true", "\"half\"", "half", 1 }, { "true", "\"full\"", "full", 0 },
	};
	char text[256];
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		fresh();
		snprintf(text, sizeof(text), "[display]\neffect_resolution = %s\nmirror_resolution = %s\nshadow_map_size = 256\n",
			cases[index].effect, cases[index].mirror);
		write_text(path, text);
		launch("");
		assert(!strcmp(config_string("display.effect_resolution"), cases[index].expected));
		snprintf(text, sizeof(text), "\neffect_resolution = \"%s\"\n", cases[index].expected);
		assert(file_has(path, text) && !file_has(path, "mirror_resolution") && file_has(path, "\nshadow_map_size = 256\n"));
		assert(strstr(logged, "display.effect_resolution is now") && !strstr(logged, "unknown setting"));
		assert(file_has(origins, cases[index].still_default ? "display.effect_resolution\tdefault\t\"half\"\n" :
			"display.effect_resolution\tchosen\n"));
		/* (a second launch finds nothing to migrate) */
		launch("");
		assert(!strcmp(config_string("display.effect_resolution"), cases[index].expected) && !strstr(logged, "is now"));
	}
	/* with a stale origins line from the boolean, and the mirror line absent */
	fresh();
	write_text(path, "[display]\neffect_resolution = false\n");
	write_text(origins, "display.effect_resolution\tdefault\ttrue\n");
	launch("");
	assert(!strcmp(config_string("display.effect_resolution"), "xbox") && file_has(origins, "display.effect_resolution\tchosen\n"));
}

int main(void)
{
	snprintf(directory, sizeof(directory), "/tmp/config-origins-probe-XXXXXX");
	if (!mkdtemp(directory))
	{
		perror("mkdtemp");
		return 1;
	}
	setenv("HALO_DATA_ROOT", directory, 1);
	snprintf(path, sizeof(path), "%s/config.toml", directory);
	snprintf(origins, sizeof(origins), "%s/config.origins", directory);
	check_first_launch();
	check_profile_changes();
	check_reload();
	check_old_install();
	check_renderer_default();
	check_tools();
	check_menus();
	check_environment_and_refusals();
	check_scenes();
	check_changed_default();
	check_external();
	check_effect_resolution_migration();
	fresh();
	rmdir(directory);
	puts("PASS: settings layers: default, device profile, a chosen value, the environment; config.origins; scene classes");
	return 0;
}
