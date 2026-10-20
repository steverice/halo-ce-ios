/* a benchmark suite's launch leaves config.toml and config.origins alone: its settings come from the environment,
and config_write (the menus' path) changes the value in memory only */
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

#include "port_config.c"
#include <assert.h>
#include <dirent.h>
#include <unistd.h>

static void write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");

	fputs(text, file);
	fclose(file);
}

static int entries(const char *directory)
{
	DIR *handle = opendir(directory);
	int count = 0;

	while (readdir(handle))
		count++;
	closedir(handle);
	return count;
}

int main(void)
{
	char directory[] = "/tmp/benchmark-isolation-XXXXXX", path[1100], origins[1100];
	const char *text = "[display]\nanisotropic_filtering = 4\n";
	char *after;
	size_t size = 0;
	int before_count;

	assert(mkdtemp(directory));
	snprintf(path, sizeof(path), "%s/config.toml", directory);
	snprintf(origins, sizeof(origins), "%s/config.origins", directory);
	setenv("HALO_DATA_ROOT", directory, 1);
	write_file(path, text);
	before_count = entries(directory);
	setenv("HALO_BENCHMARK_SUITE", "all", 1);
	setenv("HALO_ANISOTROPIC_FILTERING", "8", 1);
	assert(config_benchmark_isolated());
	assert(config_integer("display.anisotropic_filtering") == 8);   /* the launch's value */
	assert(config_write("display.anisotropic_filtering", "2"));      /* a menu change during the suite */
	assert(config_write("display.shadow_batching", "false"));
	assert(entries(directory) == before_count);                      /* no config.origins, no temp files */
	after = config_file_read(path, &size);
	assert(after && !strcmp(after, text));                           /* byte-identical */
	free(after);
	unsetenv("HALO_BENCHMARK_SUITE");
	assert(!config_benchmark_isolated());
	unlink(path);
	unlink(origins);
	rmdir(directory);
	puts("benchmark_isolation_probe: ok");
	return 0;
}
