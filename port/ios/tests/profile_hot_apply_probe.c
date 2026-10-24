/* Profile replacement must preserve choices and deferred settings. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define __HALO_LINUX_PLATFORM_H
void platform_log(const char *format, ...) { (void)format; }
int host_external_active(void) { return 0; }
static const char *pending;
static int pending_reads, last_cap, applies;
int host_device_profile_pending(char *values, unsigned long capacity, char *version, unsigned long size)
{
	pending_reads++;
	if (!pending) return 0;
	snprintf(values,capacity,"%s",pending);
	snprintf(version,size,"pending-test");
	return 1;
}
void host_device_profile_applied(int point, int networked, int changed, int metalfx, int frame_rate)
{
	(void)point; (void)networked; (void)changed; (void)metalfx;
	last_cap = frame_rate;
	applies++;
}
#include "port_config.c"
#include "tomlc17.c"
int main(void)
{
	char folder[] = "/tmp/visr-hot-profile-XXXXXX";
	assert(mkdtemp(folder));
	assert(!chdir(folder));
	setenv("HALO_DEVICE_PROFILE_VALUES", "display.render_height\t900\told\ndisplay.shadow_map_size\t256\told\ndisplay.model_lod\tauto\told\n", 1);
	assert(config_integer("display.render_height") == 900);
	const char *next = "display.render_height\t1080\tnew\ndisplay.shadow_map_size\t512\tnew\ndisplay.model_lod\tmax\tnew\ndisplay.render_height\t540\tnew\tcutscene\n";
	assert(!config_profile_apply_snapshot(next,"2",3,1));
	assert(config_integer("display.render_height") == 900);
	assert(config_profile_apply_snapshot(next,"2",3,0));
	assert(config_integer("display.render_height") == 1080);
	assert(config_integer("display.shadow_map_size") == 256);
	assert(config_scene_integer("display.render_height",2) == 540);
	assert(!config_profile_apply_snapshot(next,"2",3,0));
	assert(config_write("display.render_height","720"));
	const char *latest = "display.render_height\t1440\tlatest\ndisplay.shadow_map_size\t1024\tlatest\n";
	assert(config_profile_apply_snapshot(latest,"3",1,0));
	assert(config_integer("display.render_height") == 720);
	assert(config_integer("display.shadow_map_size") == 1024);
	assert(!strcmp(config_string("display.model_lod"),"max")); /* builtin after removal */
	assert(config_scene_integer("display.render_height",2) == 720);
	assert(!config_profile_apply_snapshot(latest,"3",1,0));
	assert(config_profile_apply_snapshot("input.aim_loss_grace\t4.0\tnew\n","4",1,2));
	assert(config_real("input.aim_loss_grace") != 4.0);
	assert(config_profile_apply_snapshot("display.upscaled_textures\tfalse\tlatest\n","5",1,0));
	assert(!config_boolean("display.upscaled_textures"));
	assert(!config_reload_boolean("display.upscaled_textures")); /* generated old file default loses */
	setenv("HALO_RENDER_HEIGHT","600",1);
	config_set_from_environment((size_t)config_setting_index("display.render_height"));
	assert(config_profile_apply_snapshot("display.render_height\t900\tnew\n","6",1,0));
	assert(config_integer("display.render_height")==600);
	assert(config_scene_integer("display.render_height",CONFIG_SCENE_CUTSCENE)==600);
	FILE *edited = fopen("config.toml","w");
	assert(edited); fputs("[display]\nupscaled_textures = false\n",edited); fclose(edited);
	assert(!config_reload_boolean("display.upscaled_textures"));
	assert(config_profile_apply_snapshot("display.upscaled_textures\ttrue\tlatest\n","7",1,0));
	assert(!config_boolean("display.upscaled_textures")); /* a live file edit becomes a choice */
	pending = "display.frame_rate\t30\tpending\n";
	int reads = pending_reads;
	config_profile_safe_point(3,CONFIG_PROFILE_FROZEN);
	assert(pending_reads == reads);
	config_profile_safe_point(3,0);
	assert(last_cap == 30 && applies == 1);
	assert(config_write("display.frame_rate","120"));
	config_profile_safe_point(3,0);
	assert(last_cap == 120);
	assert(config_write("display.frame_rate","display"));
	config_profile_safe_point(3,0);
	assert(last_cap == 0);
	assert(config_write("display.frame_rate","unlimited"));
	config_profile_safe_point(3,0);
	assert(last_cap == -1);
	puts("profile hot apply: PASS");
	return 0;
}
