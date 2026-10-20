/*
PORT_CONFIG.H

The native ports' settings, read from config.toml (port_config.c): next to
the executable on the desktop, in the data folder (the one holding maps/)
on iOS. A missing file is written with the defaults. Each setting can
also be set for one run with its HALO_* environment variable, which wins
over the file (the tools and the iOS app pass settings that way).

Settings are named "section.key", as in the file: "display.vsync".
*/

#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#include <stddef.h>

int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
/* never NULL; "" when unset */
const char *config_string(const char *name);
/* a boolean setting read again from config.toml, for settings another
process changes while the game runs; the environment still wins */
int config_reload_boolean(const char *name);
/* whether this launch is a benchmark suite (HALO_BENCHMARK_SUITE set in the launch's environment). Such a
launch takes its settings from the environment alone: nothing is written to config.toml or config.origins,
the saves go to benchmark/run/save in the data folder, and init.txt is read from benchmark/run/ */
int config_benchmark_isolated(void);
/* sets a setting from its value as text ("true", "60", "1.5", "all"), and
writes it into config.toml (only its line changes); 1 on success */
int config_write(const char *name, const char *value);
int config_write_boolean(const char *name, int value);
/* a setting's value as text ("true", "60", "1.5", "all"); 0 if there is no
such setting */
int config_text(const char *name, char *text, size_t size);
/* the folder config.toml is in, with its separator */
void config_folder(char *path, size_t size);
/* a whole file (a UTF-8 path), NUL terminated, or NULL; free() it (the Android
guest has no SDL_LoadFile) */
char *config_file_read(const char *path, size_t *size);
/* a setting's value when nobody has set it, as text: its default with the
device's profile (profiles/) over it; 0 if there is no such setting */
int config_default(const char *name, char *text, size_t size);

/* The scene classes a device profile may give some settings their own
values for ([scene.menu.display] in a profile; port/linux/game/scene_class.c
says which one a frame is). */
enum
{
	CONFIG_SCENE_GAMEPLAY,
	CONFIG_SCENE_MENU,
	CONFIG_SCENE_CUTSCENE,
	NUMBER_OF_CONFIG_SCENES
};

/* a setting's value in a scene class: the profile's value for that class
with the external output context over it while connected,
while the setting's own value comes from its default or the profile; else
(the player or the environment set it, or the profile has none) as
config_real and config_boolean */
double config_scene_real(const char *name, int scene);
int config_scene_boolean(const char *name, int scene);
long config_scene_integer(const char *name, int scene);
/* a string setting's, into text; 1 when it is a scene or external context value, 0 when the setting's own */
int config_scene_string(const char *name, int scene, char *text, size_t size);
/* every setting a device profile may set at its value in force, a "section.key = value" line each (what a
recording notes of the settings it was made under) */
void config_profile_settings_text(char *text, size_t size);
/* "gameplay", "menu", "cutscene" */
const char *config_scene_name(int scene);
/* what the scene class changes, for the log ("display.render_scale 0.667
[model Apple TV HD]"); 0 if nothing */
int config_scene_describe(int scene, char *text, size_t size);
/* how many times config_write has changed a setting: what keeps one reads
it again when this moves */
unsigned long config_changes(void);

#include "../../runtime/graphics_capture.h"
/* Bounded JSON of the supported display namespace after chosen/env/profile/context resolution.
   Returns 0 with an empty buffer on unsupported or nonfinite/unbounded data. */
int config_graphics_settings_json(int scene, char *text, size_t size);

#endif
