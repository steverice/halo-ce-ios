/* config_reload_boolean: one boolean read again from config.toml after another process changed it (the Settings
   app's switch, written by the host), the environment still winning; config_boolean alone reads the file once */
#define __HALO_LINUX_PLATFORM_H   /* port_config.c needs only platform_log from the platform layer */
#include <stdarg.h>
#include <stdio.h>

void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

int host_external_active(void) { return 0; }

int host_device_profile_pending(char *values, unsigned long capacity, char *version, unsigned long size) { (void)values; (void)capacity; (void)version; (void)size; return 0; }
void host_device_profile_applied(int point, int networked, int changed, int metalfx, int frame_rate) { (void)point; (void)networked; (void)changed; (void)metalfx; (void)frame_rate; }
#include "port_config.c"
#include <assert.h>
#include <unistd.h>

static void write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");

	fputs(text, file);
	fclose(file);
}

int main(void)
{
	char directory[] = "/tmp/config-reload-XXXXXX", path[1100];
	const char *name = "display.upscaled_textures";

	assert(mkdtemp(directory));
	snprintf(path, sizeof(path), "%s/config.toml", directory);
	setenv("HALO_DATA_ROOT", directory, 1);
	unsetenv("HALO_UPSCALED_TEXTURES");
	write_file(path, "[display]\nupscaled_textures = true\n");
	assert(config_boolean(name));
	write_file(path, "[display]\nupscaled_textures = false\n");
	assert(config_boolean(name));            /* read once: the change is not seen */
	assert(!config_reload_boolean(name));    /* until the key is read again */
	assert(!config_boolean(name));           /* which every later question then sees */
	setenv("HALO_UPSCALED_TEXTURES", "true", 1);
	assert(config_reload_boolean(name));     /* the environment wins over the file */
	unsetenv("HALO_UPSCALED_TEXTURES");
	assert(!config_reload_boolean(name));
	write_file(path, "[display]\nhigh_res_hud = true\n");
	assert(config_reload_boolean(name));     /* a key taken out of the file is its default again */
	write_file(path, "[display]\nupscaled_textures = false\n");
	assert(!config_reload_boolean(name));
	write_file(path, "[display\nupscaled_textures = false\n");
	assert(config_reload_boolean(name));     /* a file that does not parse: the default, as at start-up */
	assert(config_reload_boolean("display.no_such_setting") == 0);
	unlink(path);
	rmdir(directory);
	puts("config_reload_probe: ok");
	return 0;
}
