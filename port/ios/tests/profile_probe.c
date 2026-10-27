/* Device profiles (port/ios/host/host_profile.c): which files a device gets, the layers' order, what a profile
   may not set, and that nothing in a profile folder can leave the loader without an answer. The shipped
   profiles/ folder is checked for the devices we have measured: tools/ios_test.py passes its path. */
#include "host_profile.h"
#include "host_profile_merge.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char logged[1 << 16];

static void log_line(const char *line)
{
	strncat(logged, line, sizeof(logged) - strlen(logged) - 2);
	strcat(logged, "\n");
}

static char folder[64];

static void write_file(const char *name, const char *text)
{
	char path[256];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", folder, name);
	file = fopen(path, "wb");
	assert(file);
	fputs(text, file);
	fclose(file);
}

static struct host_device device(const char *platform, const char *model, const char *chip, int family)
{
	struct host_device made;

	memset(&made, 0, sizeof(made));
	snprintf(made.platform, sizeof(made.platform), "%s", platform);
	snprintf(made.model, sizeof(made.model), "%s", model);
	snprintf(made.chip, sizeof(made.chip), "%s", chip);
	made.gpu_family = family;
	return made;
}

static struct host_profile resolve(const char *in, struct host_device made, const char *drop_in, const char *selector)
{
	struct host_profile profile;

	logged[0] = 0;
	host_profile_resolve(&made, in, drop_in, selector, log_line, &profile);
	return profile;
}

static int has(const struct host_profile *profile, const char *name, const char *expected)
{
	char text[80];

	return host_profile_value(profile, name, text, sizeof(text)) && !strcmp(text, expected);
}

static int unset(const struct host_profile *profile, const char *name)
{
	char text[80];

	return !host_profile_value(profile, name, text, sizeof(text));
}

static int descriptor_count(void)
{
	DIR *directory = opendir("/dev/fd");
	struct dirent *entry;
	int count = 0;

	assert(directory);
	while ((entry = readdir(directory)))
		if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') count++;
	closedir(directory);
	return count;
}

static void check_file_read_paths(void)
{
	char root[] = "/tmp/profile-file-read-XXXXXX";
	char hardware[256], catalog[256], catalog_target[256], profile_path[256], drop_in[256];
	FILE *file;
	struct host_profile profile;

	assert(mkdtemp(root));
	snprintf(hardware, sizeof(hardware), "%s/hardware", root);
	assert(!mkdir(hardware, 0700));
	snprintf(catalog, sizeof(catalog), "%s/catalog.toml", hardware);
	snprintf(catalog_target, sizeof(catalog_target), "%s/catalog-source.toml", root);
	file = fopen(catalog_target, "wb"); assert(file);
	fputs("schema=1\nversion=\"test\"\nhardware=[]\nfallback=[]\n", file);
	fclose(file);
	file = fopen(catalog, "wb"); assert(file);
	fputs("schema=1\nversion=\"test\"\nhardware=[]\nfallback=[]\n", file);
	fclose(file);
	assert(host_profile_catalog_validate(root));
	assert(!unlink(catalog) && !symlink(catalog_target, catalog));
	assert(!host_profile_catalog_validate(root));
	assert(!unlink(catalog) && !mkfifo(catalog, 0600));
	assert(!host_profile_catalog_validate(root));
	assert(!unlink(catalog));
	file = fopen(catalog, "wb"); assert(file);
	assert(!ftruncate(fileno(file), HOST_PROFILE_MAXIMUM_FILE_BYTES + 1));
	fclose(file);
	assert(!host_profile_catalog_validate(root));
	int before = descriptor_count();
	for (int i = 0; i < 32; i++) assert(!host_profile_catalog_validate(root));
	assert(descriptor_count() == before);

	snprintf(profile_path, sizeof(profile_path), "%s/profile.toml", root);
	file = fopen(profile_path, "wb"); assert(file);
	fputs("schema=1\nname=\"Drop-in\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Test1,1\"]\n[display]\nrender_height=720\n", file);
	fclose(file);
	profile = resolve(NULL, device("ios", "Test1,1", "", 1), profile_path, "");
	assert(profile.drop_in && has(&profile, "display.render_height", "720"));
	snprintf(drop_in, sizeof(drop_in), "%s/drop-in.toml", root);
	assert(!symlink(profile_path, drop_in));
	profile = resolve(NULL, device("ios", "Test1,1", "", 1), drop_in, "");
	assert(!profile.drop_in && unset(&profile, "display.render_height"));
	assert(!unlink(drop_in) && !mkfifo(drop_in, 0600));
	profile = resolve(NULL, device("ios", "Test1,1", "", 1), drop_in, "");
	assert(!profile.drop_in && unset(&profile, "display.render_height"));
	before = descriptor_count();
	for (int i = 0; i < 32; i++)
	{
		profile = resolve(NULL, device("ios", "Test1,1", "", 1), drop_in, "");
		assert(!profile.drop_in);
	}
	assert(descriptor_count() == before);
	assert(!unlink(drop_in));
	file = fopen(drop_in, "wb"); assert(file);
	assert(!ftruncate(fileno(file), HOST_PROFILE_MAXIMUM_FILE_BYTES + 1));
	fclose(file);
	profile = resolve(NULL, device("ios", "Test1,1", "", 1), drop_in, "");
	assert(!profile.drop_in && unset(&profile, "display.render_height"));
	before = descriptor_count();
	for (int i = 0; i < 32; i++)
	{
		profile = resolve(NULL, device("ios", "Test1,1", "", 1), drop_in, "");
		assert(!profile.drop_in);
	}
	assert(descriptor_count() == before);
	assert(!unlink(drop_in));
	assert(!unlink(profile_path) && !unlink(catalog) && !unlink(catalog_target));
	assert(!rmdir(hardware) && !rmdir(root));
}

static void check_layers(void)
{
	char drop_in[256], missing[256], encoded[4096], boundary_name[64], overlong_name[65];
	struct host_profile profile;

	write_file("old.toml", "schema = 1\nname = \"Old\"\nkind = \"class\"\n[match]\nplatform = \"tvos\"\n"
		"gpu_family = [1, 2]\n[display]\nrender_height = 720\nanisotropic_filtering = 4\n");
	write_file("new.toml", "schema = 1\nname = \"New\"\nkind = \"class\"\n[match]\nplatform = \"tvos\"\n"
		"gpu_family = [3, 0]\n[display]\nrender_height = 1080\nrenderer = \"metal\"\n");
	write_file("box.toml", "schema = 1\nname = \"Box\"\nkind = \"model\"\n[match]\nplatform = \"tvos\"\n"
		"models = [\"Box1,1\", \"Box1,2\"]\n[display]\nrender_height = 540\n"
		"[scene.menu.display]\nrender_scale = 0.75\nshadow_map_size = 128\n"
		"[scene.cutscene.display]\nrender_scale = 0.5\n"
		"[scene.\"cutscene@a10\".display]\nrender_scale = 1.0\n"
		"[evidence.\"display.render_height\"]\nbasis = \"measured\"\nwhy = \"it is\"\n");
	write_file("phone.toml", "schema = 1\nname = \"Phones\"\nkind = \"class\"\n[match]\nplatform = \"ios\"\n"
		"gpu_family = [4, 0]\n[display]\nshadow_map_size = 256\n");
	write_file("desk.toml", "schema = 1\nname = \"Desk\"\nkind = \"model\"\n[match]\nplatform = \"mac\"\n"
		"chips = [\"Apple M9\"]\n[display]\nframe_rate = \"240\"\n");
	write_file("TEMPLATE.toml", "schema = 1\nname = \"Template\"\nkind = \"model\"\n[match]\nplatform = \"tvos\"\n"
		"models = [\"Box1,1\"]\n[display]\nrender_height = 4320\n");
	write_file("notes.txt", "not a profile\n");
	snprintf(missing, sizeof(missing), "%s/none.toml", folder);

	/* the class by GPU family, then the model's values over it */
	profile = resolve(folder, device("tvos", "Box1,2", "", 2), missing, "");
	assert(has(&profile, "display.render_height", "540") && has(&profile, "display.anisotropic_filtering", "4"));
	assert(unset(&profile, "display.renderer") && unset(&profile, "display.shadow_map_size"));
	assert(!strcmp(profile.class_file, "old.toml") && !strcmp(profile.model_file, "box.toml") && !profile.drop_in);
	assert(strstr(logged, "class \"Old\" (old.toml), model \"Box\" (box.toml)"));
	/* (both layers reach the game, lowest first, each with where it came from) */
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "display.render_height\t720\tclass Old\n"));
	assert(strstr(encoded, "display.render_height\t540\tmodel Box\n"));
	assert(strstr(encoded, "class Old") < strstr(encoded, "model Box"));
	/* a scene class's values go to the game marked with the class, and are not the setting's own value; one
	that cannot change by scene, and a class this build does not know, are left out and named */
	assert(strstr(encoded, "display.render_scale\t0.75\tmodel Box\tmenu\n"));
	assert(strstr(encoded, "display.render_scale\t0.5\tmodel Box\tcutscene\n"));
	assert(unset(&profile, "display.render_scale") && !strstr(encoded, "display.shadow_map_size\t128"));
	assert(strstr(logged, "display.shadow_map_size cannot change by scene class"));
	assert(strstr(logged, "scene class \"cutscene@a10\" is not one this build knows"));

	/* a model no file names: its class alone */
	profile = resolve(folder, device("tvos", "Box9,9", "", 3), missing, "");
	assert(has(&profile, "display.render_height", "1080") && has(&profile, "display.renderer", "metal"));
	assert(!profile.model_file[0] && strstr(logged, "no model profile"));
	/* a GPU family newer than any we know: the newest class, which has no upper bound */
	profile = resolve(folder, device("tvos", "Box20,1", "", 19), missing, "");
	assert(!strcmp(profile.class_file, "new.toml") && !strstr(logged, "nearest"));
	/* a family below every class (or none read): the nearest class, and the log says so */
	profile = resolve(folder, device("ios", "Phone1,1", "", 0), missing, "");
	assert(has(&profile, "display.shadow_map_size", "256") && strstr(logged, "the nearest, \"Phones\""));
	/* another platform's model profile is not this device's, whatever it is called */
	profile = resolve(folder, device("ios", "Box1,1", "", 5), missing, "");
	assert(!profile.model_file[0] && unset(&profile, "display.render_height"));
	/* a Mac by its chip */
	profile = resolve(folder, device("mac", "Mac99,1", "Apple M9", 12), missing, "");
	assert(has(&profile, "display.frame_rate", "240") && !profile.class_file[0]);
	profile = resolve(folder, device("mac", "Mac99,1", "Apple M9 Pro", 12), missing, "");
	assert(profile.count == 0 && strstr(logged, "no class profile, no model profile"));

	/* A 63-byte profile filename fits; a 64-byte filename is outside the buffer. */
	memset(boundary_name, 'a', sizeof(boundary_name) - 6);
	memcpy(boundary_name + sizeof(boundary_name) - 6, ".toml", 6);
	memset(overlong_name, 'b', sizeof(overlong_name) - 6);
	memcpy(overlong_name + sizeof(overlong_name) - 6, ".toml", 6);
	write_file(boundary_name, "schema = 1\nname = \"Boundary\"\nkind = \"model\"\n[match]\nplatform = \"tvos\"\n"
		"models = [\"Box1,1\"]\n[display]\nrender_height = 1440\n");
	write_file(overlong_name, "schema = 1\nname = \"Overlong\"\nkind = \"model\"\n[match]\nplatform = \"tvos\"\n"
		"models = [\"Box1,1\"]\n[display]\nrender_height = 2160\n");
	profile = resolve(folder, device("tvos", "Box1,1", "", 2), missing, "");
	assert(!strcmp(profile.model_file, boundary_name) && has(&profile, "display.render_height", "1440"));
	assert(!strstr(logged, overlong_name));
	snprintf(drop_in, sizeof(drop_in), "%s/%s", folder, boundary_name);
	assert(!unlink(drop_in));
	snprintf(drop_in, sizeof(drop_in), "%s/%s", folder, overlong_name);
	assert(!unlink(drop_in));

	/* the drop-in file over both, key by key, and announced */
	snprintf(drop_in, sizeof(drop_in), "%s/device-profile.toml", folder);
	write_file("device-profile.toml", "schema = 1\nname = \"Mine\"\nkind = \"model\"\n[match]\nplatform = \"tvos\"\n"
		"[display]\nrender_height = 900\n");
	/* (the folder's own listing must not take it for a shipped profile) */
	profile = resolve(folder, device("tvos", "Box1,1", "", 2), drop_in, "");
	assert(has(&profile, "display.render_height", "900") && has(&profile, "display.anisotropic_filtering", "4"));
	assert(profile.drop_in && strstr(logged, "DROP-IN FILE device-profile.toml IN FORCE (1 values)"));
	assert(!strcmp(profile.model_file, "box.toml"));
	unlink(drop_in);

	/* debug.device_profile: none; and as another device, on its platform */
	profile = resolve(folder, device("tvos", "Box1,1", "", 2), missing, "none");
	assert(profile.count == 0 && strstr(logged, "built-in defaults"));
	profile = resolve(folder, device("mac", "Mac99,1", "Apple M1", 2), missing, "Box1,1");
	assert(has(&profile, "display.render_height", "540") && !strcmp(profile.class_file, "old.toml"));
	assert(strstr(logged, "choosing as \"Box1,1\""));
}

static void check_refusals(void)
{
	char missing[256], big[256];
	struct host_profile profile;
	FILE *file;
	int index;

	snprintf(missing, sizeof(missing), "%s/none.toml", folder);
	/* what a profile may not set, and values a setting does not accept: each left out and named, the rest kept */
	write_file("bad.toml", "schema = 1\nname = \"Bad\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Bad1,1\"]\n"
		"[display]\nrender_quality = 0.5\nshadow_map_size = 2048\nrender_scale = 0.01\nrenderer = \"vulkan\"\n"
		"stereo = \"head\"\nframe_rate = \"29\"\nanisotropic_filtering = \"16\"\nfoveation = 1\nlod_scale = nan\n"
		"frame_pacing = \"tick\"\neffect_resolution = \"double\"\nanti_aliasing = \"taa\"\n"
		"[network]\nonline = true\n[paths]\ndata = \"/tmp\"\n[debug]\nexit_after = 1.0\n[input]\nturn = \"off\"\n");
	profile = resolve(folder, device("visionos", "Bad1,1", "", 8), missing, "");
	assert(profile.count == 2 && has(&profile, "display.render_quality", "0.5") && has(&profile, "display.frame_pacing", "tick"));
	assert(strstr(logged, "display.shadow_map_size has a value of the wrong type or outside"));
	assert(strstr(logged, "display.renderer has a value") && strstr(logged, "display.frame_rate has a value"));
	assert(strstr(logged, "display.anti_aliasing has a value"));
	assert(strstr(logged, "display.lod_scale has a value") && strstr(logged, "display.foveation has a value"));
	assert(strstr(logged, "display.stereo is not a setting a profile may set"));
	assert(strstr(logged, "network.online is not a setting") && strstr(logged, "paths.data is not a setting"));
	assert(strstr(logged, "debug.exit_after is not a setting") && strstr(logged, "input.turn is not a setting"));
	/* the tracking-loss grace is the one non-graphics value a profile sets, within 0 to 5 seconds */
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[input]\naim_loss_grace = 1.5\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 1 && has(&profile, "input.aim_loss_grace", "1.5"));
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[input]\naim_loss_grace = 9.0\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 0 && strstr(logged, "input.aim_loss_grace has a value"));
	/* the texture cache's idle time is the other, within 10 to 600 seconds */
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[display]\ntexture_idle_seconds = 30\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 1 && has(&profile, "display.texture_idle_seconds", "30"));
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[display]\ntexture_idle_seconds = 5\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 0 && strstr(logged, "display.texture_idle_seconds has a value"));

	/* the player capacity is within 2 to 128 players */
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[network]\nplayer_capacity = 16\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 1 && has(&profile, "network.player_capacity", "16"));
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[network]\nplayer_capacity = 1\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 0 && strstr(logged, "network.player_capacity has a value"));
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[network]\nplayer_capacity = 129\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 0 && strstr(logged, "network.player_capacity has a value"));

	/* the clutter distance is within 0 to 200 world units */
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[display]\nclutter_distance = 40\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 1 && has(&profile, "display.clutter_distance", "40"));
	write_file("grace.toml", "schema = 1\nname = \"Grace\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Grace1,1\"]\n[display]\nclutter_distance = 500\n");
	profile = resolve(folder, device("visionos", "Grace1,1", "", 8), missing, "");
	assert(profile.count == 0 && strstr(logged, "display.clutter_distance has a value"));

	/* files that are no profile: not parsed, another schema, no heading, too large. The launch goes on */
	write_file("broken.toml", "schema = 1\nname = \"Broken\nkind = [[[\n");
	write_file("future.toml", "schema = 2\nname = \"Future\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\n"
		"models = [\"Bad1,1\"]\n[display]\nrender_quality = 1.0\n");
	write_file("headless.toml", "[display]\nrender_quality = 1.0\n");
	snprintf(big, sizeof(big), "%s/big.toml", folder);
	file = fopen(big, "wb");
	assert(file);
	fputs("schema = 1\nname = \"Big\"\nkind = \"model\"\n[match]\nplatform = \"visionos\"\nmodels = [\"Big1,1\"]\n", file);
	for (index = 0; index < 4000; index++)
		fputs("# padding padding padding\n", file);
	fclose(file);
	profile = resolve(folder, device("visionos", "Bad1,1", "", 8), missing, "");
	assert(profile.count == 2 && !strcmp(profile.model_file, "bad.toml"));
	assert(strstr(logged, "broken.toml does not parse") && strstr(logged, "future.toml is not schema 1"));
	assert(strstr(logged, "headless.toml is not schema 1") && strstr(logged, "big.toml is"));
	/* a broken drop-in file is not used and says so */
	profile = resolve(folder, device("visionos", "Bad1,1", "", 8), big, "");
	assert(profile.count == 2 && !profile.drop_in);
	snprintf(big, sizeof(big), "%s/broken.toml", folder);
	profile = resolve(folder, device("visionos", "Bad1,1", "", 8), big, "");
	assert(!profile.drop_in && strstr(logged, "the drop-in file device-profile.toml does not parse"));

	/* no folder at all: no values, a line in the log, no crash */
	profile = resolve("/nonexistent/profiles", device("tvos", "Box1,1", "", 2), missing, "");
	assert(profile.count == 0 && strstr(logged, "no profile folder"));
	profile = resolve(NULL, device("tvos", "Box1,1", "", 2), NULL, NULL);
	assert(profile.count == 0);
}

/* Which profile real models without one of their own resolve to: the hardware catalog's nearest weaker
   measured profile, the newest measured profile's and_newer rule, or the class floor. */
static struct host_profile resolve_model(const char *shipped, const char *platform, const char *model,
	int family, unsigned memory, int width, int height, int hertz)
{
	struct host_device made = device(platform, model, "", family);

	made.memory_megabytes = memory; made.pixel_width = width; made.pixel_height = height; made.refresh_rate = hertz;
	return resolve(shipped, made, NULL, "");
}

static void check_unprofiled_models(const char *shipped)
{
	struct host_profile profile;

	/* iPhone 15 Pro Max (A17 Pro, 120 Hz): its own profile, 120 in a predictable workload (Steve, 2026-10-27). */
	profile = resolve_model(shipped, "ios", "iPhone16,2", 9, 7655, 2796, 1290, 120);
	assert(!profile.inherited && !strcmp(profile.model_file, "iphone-15-pro.toml") && has(&profile, "display.frame_rate", "120"));
	/* iPhone 16 (A18, 5-core GPU): the A17 Pro profile just below it, inside its 60 Hz and pixel envelope. */
	profile = resolve_model(shipped, "ios", "iPhone17,3", 9, 7600, 2556, 1179, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "iphone-15-pro.toml") &&
		!strcmp(profile.source_hardware, "iphone-a17-pro") && !strcmp(profile.target_hardware, "iphone-16"));
	/* iPhone 17 Pro Max: the same chip as the measured 17 Pro; the profile is inherited, not an and_newer match. */
	profile = resolve_model(shipped, "ios", "iPhone18,2", 10, 11722, 2868, 1320, 120);
	assert(profile.inherited && !strcmp(profile.model_file, "iphone-17-pro.toml"));
	/* A model a later generation introduces: the 17 Pro's and_newer rule. */
	profile = resolve_model(shipped, "ios", "iPhone19,1", 11, 12000, 2622, 1206, 120);
	assert(!profile.inherited && !strcmp(profile.model_file, "iphone-17-pro.toml") && has(&profile, "display.anti_aliasing", "fxaa") &&
		strstr(logged, "and_newer iphone-17-pro.toml: iPhone19,1"));
	/* The oldest supported iPhone sits below every measured profile: the class floor. */
	profile = resolve_model(shipped, "ios", "iPhone10,1", 4, 1950, 1334, 750, 60);
	assert(!profile.model_file[0] && !strcmp(profile.class_file, "iphone-ipad.toml") &&
		has(&profile, "display.render_height", "720") &&
		has(&profile, "display.frame_rate", "30"));
	/* iPhone 17 (120 Hz): matched by the 15 Pro's profile, 120 in a predictable workload (Steve, 2026-10-27). */
	profile = resolve_model(shipped, "ios", "iPhone18,3", 10, 7600, 2622, 1206, 120);
	assert(!profile.inherited && !strcmp(profile.model_file, "iphone-15-pro.toml") && has(&profile, "display.frame_rate", "120"));
	/* iPhone 16 Pro (A18 Pro, 120 Hz): matched by the 15 Pro's profile itself, 120 in a predictable workload. */
	profile = resolve_model(shipped, "ios", "iPhone17,1", 9, 7600, 2622, 1206, 120);
	assert(!profile.inherited && !strcmp(profile.model_file, "iphone-15-pro.toml") && has(&profile, "display.frame_rate", "120"));
	/* iPad Air 3 (A12): the iPad Pro (A10X) profile, 900 lines at 30; nothing measured is closer. */
	profile = resolve_model(shipped, "ios", "iPad11,3", 5, 2906, 2224, 1668, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "ipad-pro-12-9-2nd.toml") &&
		has(&profile, "display.render_height", "900") && has(&profile, "display.frame_rate", "30") &&
		!strcmp(profile.target_hardware, "ipad-a12"));
	/* iPad Pro 12.9-inch 3rd generation (A12X, 7-core GPU): the A10X profile; its 2732x2048 menu is outside the Air 4's envelope. */
	profile = resolve_model(shipped, "ios", "iPad8,5", 5, 3900, 2732, 2048, 120);
	assert(profile.inherited && !strcmp(profile.model_file, "ipad-pro-12-9-2nd.toml"));
	/* iPad mini 6 and iPad (A16): the iPad Air 4 (A14) profile below them; the catalog's answer comes before the
	   M1 iPad Pro's and_newer rule, which would otherwise match their higher identifiers. */
	profile = resolve_model(shipped, "ios", "iPad14,1", 8, 3900, 2266, 1488, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "ipad-air-4.toml") && !strstr(logged, "and_newer"));
	profile = resolve_model(shipped, "ios", "iPad15,7", 8, 5800, 2360, 1640, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "ipad-air-4.toml") && !strstr(logged, "and_newer"));
	/* iPad 10th generation: the same A14 as the measured Air 4. */
	profile = resolve_model(shipped, "ios", "iPad13,18", 7, 3900, 2360, 1640, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "ipad-air-4.toml") && has(&profile, "display.render_height", "1080"));
	/* iPhone 12 Pro Max: the measured 12 Pro's chip and memory; the 4 GB iPhone 12 keeps the class floor. */
	profile = resolve_model(shipped, "ios", "iPhone13,4", 7, 5700, 2778, 1284, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "iphone-12-pro.toml") && has(&profile, "display.frame_rate", "30"));
	profile = resolve_model(shipped, "ios", "iPhone13,2", 7, 3800, 2532, 1170, 60);
	assert(!profile.model_file[0] && !strcmp(profile.class_file, "iphone-ipad.toml"));
	/* iPad 5th generation (A9), the oldest: below every measured iPad profile. */
	profile = resolve_model(shipped, "ios", "iPad6,11", 3, 1950, 2048, 1536, 60);
	assert(!profile.model_file[0] && !strcmp(profile.class_file, "iphone-ipad.toml") &&
		has(&profile, "display.render_height", "720"));
	/* iPad Air 5 (M1) shares the measured iPad Pro chip. */
	profile = resolve_model(shipped, "ios", "iPad13,16", 7, 7600, 2360, 1640, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "ipad-pro-m1.toml"));
	/* Upscaled textures on Apple TVs (Steve, 2026-10-25): on for the 4K 3rd generation and anything newer, off for
	   the HD, the 4K 1st generation and the 4K 2nd generation, whose value is the Apple TV class's. */
	profile = resolve_model(shipped, "tvos", "AppleTV14,1", 8, 4096, 3840, 2160, 60);
	assert(!profile.inherited && !strcmp(profile.model_file, "apple-tv-4k-3rd.toml") && has(&profile, "display.upscaled_textures", "true"));
	profile = resolve_model(shipped, "tvos", "AppleTV16,1", 10, 8192, 3840, 2160, 60);
	assert(!strcmp(profile.model_file, "apple-tv-4k-3rd.toml") && has(&profile, "display.upscaled_textures", "true") &&
		strstr(logged, "and_newer apple-tv-4k-3rd.toml: AppleTV16,1"));
	profile = resolve_model(shipped, "tvos", "AppleTV11,1", 5, 3072, 3840, 2160, 60);
	assert(strcmp(profile.model_file, "apple-tv-4k-3rd.toml") && has(&profile, "display.upscaled_textures", "false"));
	profile = resolve_model(shipped, "tvos", "AppleTV6,2", 3, 3072, 3840, 2160, 60);
	assert(!strcmp(profile.model_file, "apple-tv-4k-1st.toml") && has(&profile, "display.upscaled_textures", "false"));
	profile = resolve_model(shipped, "tvos", "AppleTV5,3", 2, 2048, 1920, 1080, 60);
	assert(!strcmp(profile.model_file, "apple-tv-hd.toml") && has(&profile, "display.upscaled_textures", "false"));
	/* iPads that inherit the A10X profile (the A12 iPad Air 3) keep the game's default: the A10X itself is off only
	   through the Neural Engine check, so no profile turns an A12 off */
	profile = resolve_model(shipped, "ios", "iPad11,3", 5, 2906, 2224, 1668, 60);
	assert(!has(&profile, "display.upscaled_textures", "false"));
	/* A later iPad generation: the M1 iPad Pro's and_newer rule. */
	profile = resolve_model(shipped, "ios", "iPad19,1", 11, 16000, 2420, 1668, 120);
	assert(!profile.inherited && !strcmp(profile.model_file, "ipad-pro-m1.toml") &&
		strstr(logged, "and_newer ipad-pro-m1.toml: iPad19,1"));
	/* Apple TV 4K 2nd generation (A12): the 1st generation's 720-line profile. */
	profile = resolve_model(shipped, "tvos", "AppleTV11,1", 5, 2950, 3840, 2160, 60);
	assert(profile.inherited && !strcmp(profile.model_file, "apple-tv-4k-1st.toml") &&
		has(&profile, "display.render_height", "720"));
	/* An Apple TV below every measured profile and outside the catalog: the Apple TV HD's values. */
	profile = resolve_model(shipped, "tvos", "AppleTV9,9", 3, 2000, 1920, 1080, 60);
	assert(!profile.model_file[0] && has(&profile, "display.render_scale", "0.5") &&
		has(&profile, "display.anisotropic_filtering", "4") && has(&profile, "display.shadow_map_size", "128"));
	/* A later Apple TV: the 3rd generation's and_newer rule. */
	profile = resolve_model(shipped, "tvos", "AppleTV15,1", 11, 4000, 3840, 2160, 60);
	assert(!profile.inherited && !strcmp(profile.model_file, "apple-tv-4k-3rd.toml") &&
		has(&profile, "display.render_height", "1080"));
}

/* the shipped profiles, for the devices measured so far: what each resolves to */
static void check_shipped(const char *shipped)
{
	struct host_profile profile;

	char encoded[4096];

	/* Apple TV HD: Metal, half of 1080 each way (960x540), no upscaled textures; its menu and cutscene values */
	profile = resolve(shipped, device("tvos", "AppleTV5,3", "", 2), NULL, "");
	assert(has(&profile, "display.renderer", "metal") && has(&profile, "display.render_height", "1080"));
	assert(has(&profile, "display.render_scale", "0.5") && has(&profile, "display.upscaled_textures", "false"));
	assert(has(&profile, "display.anisotropic_filtering", "4") && has(&profile, "display.model_lod", "auto") &&
		has(&profile, "display.shadow_map_size", "128") && has(&profile, "display.frame_rate", "30"));
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "\tmodel Apple TV HD\tmenu\n") && strstr(encoded, "\tmodel Apple TV HD\tcutscene\n"));
	/* an Apple TV 4K without memory or display reported, and the tvOS simulator: the class floor, the Apple TV HD's values */
	profile = resolve(shipped, device("tvos", "AppleTV11,1", "", 5), NULL, "");
	assert(has(&profile, "display.render_height", "1080") && !profile.model_file[0] &&
		has(&profile, "display.render_scale", "0.5") && has(&profile, "display.anisotropic_filtering", "4"));
	/* the two Apple TV 4K models with estimated profiles: 720 in play on the first, with its built-in output capped
	at 1080 (a tvOS profile's own display.output_height, for the host and never the game), and 1080 on the third */
	profile = resolve(shipped, device("tvos", "AppleTV6,2", "", 3), NULL, "");
	assert(has(&profile, "display.render_height", "720") && has(&profile, "display.anisotropic_filtering", "4") &&
		has(&profile, "display.renderer", "metal") && !strcmp(profile.model_file, "apple-tv-4k-1st.toml"));
	assert(has(&profile, "display.output_height", "1080"));
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "display.output_height\t1080\tmodel Apple TV 4K (1st generation)\n"));
	profile = resolve(shipped, device("tvos", "AppleTV14,1", "", 8), NULL, "");
	assert(has(&profile, "display.render_height", "1080") && has(&profile, "display.anisotropic_filtering", "16") &&
		!strcmp(profile.model_file, "apple-tv-4k-3rd.toml"));
	/* Vision Pro: the headset sweep's values, which are the built-in defaults too */
	profile = resolve(shipped, device("visionos", "RealityDevice14,1", "", 8), NULL, "");
	assert(has(&profile, "display.render_quality", "0.6") && has(&profile, "display.foveation", "true"));
	assert(!strcmp(profile.model_file, "vision-pro-m2.toml") && profile.count == 5 &&
		has(&profile, "display.renderer", "metal"));
	/* iPhone 17 Pro, iPad Pro (M1) and the Macs: values equal to the built-in defaults, and the iPhone and iPad
	class profile's Metal; the 17 Pro and the M1 iPad Pro target 120 frames a second in a predictable workload */
	profile = resolve(shipped, device("ios", "iPhone18,1", "", 10), NULL, "");
	assert(has(&profile, "display.render_height", "1080") && has(&profile, "display.frame_rate", "120") &&
		has(&profile, "display.anisotropic_filtering", "16") &&
		has(&profile, "display.shadow_map_size", "512") && has(&profile, "display.renderer", "metal") &&
		has(&profile, "display.anti_aliasing", "fxaa"));
	profile = resolve(shipped, device("ios", "iPad13,9", "", 7), NULL, "");
	assert(has(&profile, "display.render_height", "1080") && has(&profile, "display.frame_rate", "120") &&
		has(&profile, "display.effect_resolution", "half") && has(&profile, "display.renderer", "metal"));
	/* iPad Air 4 and iPhone 12 Pro: their own measured profiles */
	profile = resolve(shipped, device("ios", "iPad13,1", "", 7), NULL, "");
	assert(!strcmp(profile.model_file, "ipad-air-4.toml") && has(&profile, "display.render_height", "1080") &&
		has(&profile, "display.frame_rate", "60"));
	profile = resolve(shipped, device("ios", "iPhone13,3", "", 7), NULL, "");
	assert(!strcmp(profile.model_file, "iphone-12-pro.toml") && has(&profile, "display.render_height", "900") &&
		has(&profile, "display.frame_rate", "30"));
	/* The smaller M1 iPad Pro inherits the measured M1 profile, subject to its envelope. */
	struct host_device sibling = device("ios", "iPad13,4", "", 7);
	sibling.memory_megabytes = 8192; sibling.pixel_width = 2388; sibling.pixel_height = 1668; sibling.refresh_rate = 120;
	profile = resolve(shipped, sibling, NULL, "");
	assert(profile.inherited && has(&profile, "display.effect_resolution", "half"));
	assert(!strcmp(profile.model_file, "ipad-pro-m1.toml"));
	/* The measured first-generation 12.9-inch iPad uses its own 720-line, 30 frames a second model profile. The neighboring
	   cellular identifier is cataloged, but a device reporting no memory or display cannot inherit and keeps the class floor. */
	profile = resolve(shipped, device("ios", "iPad6,7", "", 3), NULL, "");
	assert(!strcmp(profile.model_file, "ipad-pro-12-9-1st.toml") &&
		has(&profile, "display.render_height", "720") && has(&profile, "display.frame_rate", "30") &&
		has(&profile, "display.renderer", "metal"));
	profile = resolve(shipped, device("ios", "iPad6,8", "", 3), NULL, "");
	assert(!profile.model_file[0] && !profile.inherited);
	profile = resolve(shipped, device("mac", "Mac14,5", "Apple M2 Max", 8), NULL, "");
	assert(profile.count == 3 && has(&profile, "display.model_lod", "max") &&
		has(&profile, "display.compressed_textures", "true") && has(&profile, "display.renderer", "metal"));
	profile = resolve(shipped, device("mac", "Mac16,10", "Apple M4", 9), NULL, "");
	assert(profile.count == 2 && has(&profile, "display.shadow_map_size", "512") && has(&profile, "display.renderer", "metal"));
	profile = resolve(shipped, device("mac", "Mac17,1", "Apple M6", 11), NULL, "");
	assert(profile.count == 1 && !strcmp(profile.model_file, "mac-m6.toml") && has(&profile, "display.renderer", "metal"));
	/* a Mac, an iPhone and a headset nobody has measured: their class, nothing refused */
	profile = resolve(shipped, device("mac", "Mac15,3", "Apple M3 Max", 9), NULL, "");
	assert(profile.count == 1 && !strcmp(profile.class_file, "mac.toml") && has(&profile, "display.renderer", "metal") &&
		!strstr(logged, "ignored"));
	profile = resolve(shipped, device("ios", "iPhone18,9", "", 15), NULL, "");
	assert(has(&profile, "display.renderer", "metal") && has(&profile, "display.render_height", "720") &&
		!strcmp(profile.class_file, "iphone-ipad.toml"));
	profile = resolve(shipped, device("visionos", "RealityDevice99,1", "", 15), NULL, "");
	assert(profile.count == 3 && !profile.model_file[0] && has(&profile, "display.renderer", "metal"));
	check_unprofiled_models(shipped);
}

/* Synthetic chip tiers are intentionally independent of real Apple rankings. */
static void check_capability_fallback(void)
{
	struct host_profile profile;
	struct host_device target = device("ios", "Chip11,1", "", 5);
	char path[256], encoded[4096];

	target.memory_megabytes = 4096;
	target.pixel_width = 1920; target.pixel_height = 1080; target.refresh_rate = 60;
	write_file("tier-class.toml", "schema=1\nname=\"Tier class\"\nkind=\"class\"\n[match]\nplatform=\"ios\"\ngpu_family=[1,3]\n[display]\nrender_height=720\n");
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nanisotropic_filtering=4\n[scene.menu.display]\nrender_scale=0.5\n");
	write_file("tier-a12.toml", "schema=1\nname=\"A12\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip12,1\"]\n[display]\nanisotropic_filtering=16\n");
	snprintf(path, sizeof(path), "%s/hardware", folder); assert(!mkdir(path, 0700));
	write_file("hardware/catalog.toml",
		"schema=1\nversion=\"test\"\n"
		"[[hardware]]\nid=\"a9\"\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\ntier=9\ngpu_family=2\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[hardware]]\nid=\"a11\"\nplatform=\"ios\"\nmodels=[\"Chip11,1\"]\ntier=11\ngpu_family=5\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[hardware]]\nid=\"a12\"\nplatform=\"ios\"\nmodels=[\"Chip12,1\"]\ntier=12\ngpu_family=6\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[fallback]]\nhardware=\"a9\"\nprofile=\"tier-a9.toml\"\nminimum_memory_mb=2048\nminimum_gpu_family=2\nmaximum_pixels=[1000000,1000000,1000000]\nmaximum_refresh_hertz=60\n"
		"[[fallback]]\nhardware=\"a12\"\nprofile=\"tier-a12.toml\"\nminimum_memory_mb=4096\nminimum_gpu_family=5\nmaximum_pixels=[2073600,2073600,2073600]\nmaximum_refresh_hertz=60\n");
	profile = resolve(folder, target, NULL, "");
	assert(has(&profile, "display.anisotropic_filtering", "4"));
	assert(has(&profile, "display.render_height", "720"));
	/* The receiving class's shadow setting must not leak into the source baseline. */
	assert(unset(&profile, "display.shadow_map_size"));
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "display.render_scale\t0.5\tmodel A9\tmenu\n"));
	assert(strstr(logged, "inherited") && strstr(logged, "a11") && strstr(logged, "a9"));
	/* The variable workload context is play on the source device: its render size must fit the gameplay envelope. */
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nanisotropic_filtering=4\n[variable.display]\nrender_height=540\n");
	profile = resolve(folder, target, NULL, "");
	assert(profile.inherited && has(&profile, "display.anisotropic_filtering", "4"));
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nanisotropic_filtering=4\n[variable.display]\nrender_height=1080\n");
	profile = resolve(folder, target, NULL, "");
	assert(!profile.inherited && !profile.model_file[0]);
	/* A model's external context is inherited with its baseline. */
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nanisotropic_filtering=4\n[external.display]\noutput_height=720\nrender_height=360\n");
	profile = resolve(folder, target, NULL, "");
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(profile.inherited && strstr(encoded, "display.render_height\t360\tmodel A9\t\texternal\n"));
	/* Exact/debug matching, unknown hardware, and insufficient memory keep their existing behavior. */
	target.memory_megabytes = 1024;
	profile = resolve(folder, target, NULL, "");
	assert(!profile.model_file[0] && has(&profile, "display.shadow_map_size", "256"));
	target.memory_megabytes = 4096; target.refresh_rate = 120;
	profile = resolve(folder, target, NULL, ""); assert(!profile.model_file[0]);
	/* display.frame_rate under the display's rate caps an iPhone too: "60" on a 120 Hz screen fits a 60 Hz tier,
	   and "unlimited" does not lift an iPhone above its display */
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nanisotropic_filtering=4\nframe_rate=\"60\"\n");
	profile = resolve(folder, target, NULL, ""); assert(profile.inherited && has(&profile, "display.frame_rate", "60"));
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nanisotropic_filtering=4\nframe_rate=\"unlimited\"\n");
	profile = resolve(folder, target, NULL, ""); assert(!profile.model_file[0]);
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nanisotropic_filtering=4\n[external.display]\noutput_height=720\nrender_height=360\n");
	target.refresh_rate = 60; target.pixel_width = 8192;
	profile = resolve(folder, target, NULL, ""); assert(!profile.model_file[0]);
	profile = resolve(folder, device("ios", "Chip12,1", "", 5), NULL, "");
	assert(has(&profile, "display.anisotropic_filtering", "16"));
	profile = resolve(folder, target, NULL, "Chip12,1");
	assert(has(&profile, "display.anisotropic_filtering", "16"));
	profile = resolve(folder, device("ios", "Unknown99,1", "", 5), NULL, ""); assert(!profile.model_file[0]);
	profile = resolve(folder, target, NULL, "none"); assert(profile.count == 0);
	/* A newly measured intermediate tier is selected automatically. */
	write_file("tier-a10.toml", "schema=1\nname=\"A10\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip10,1\"]\n[display]\nanisotropic_filtering=8\n");
	snprintf(path, sizeof(path), "%s/hardware/catalog.toml", folder);
	FILE *catalog = fopen(path, "ab"); assert(catalog);
	fputs("\n[[hardware]]\nid=\"a10\"\nplatform=\"ios\"\nmodels=[\"Chip10,1\"]\ntier=10\ngpu_family=2\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[fallback]]\nhardware=\"a10\"\nprofile=\"tier-a10.toml\"\nminimum_memory_mb=2048\nminimum_gpu_family=2\nmaximum_pixels=[1000000,1000000,1000000]\nmaximum_refresh_hertz=60\n", catalog);
	fclose(catalog);
	target.pixel_width = 1920;
	profile = resolve(folder, target, NULL, "");
	assert(has(&profile, "display.anisotropic_filtering", "8") && profile.inherited);
	assert(profile.source_tier == 10 && profile.target_tier == 11 && !strcmp(profile.catalog_version, "test"));
	/* Drop-ins remain above an inherited source. */
	write_file("tier-drop.toml", "[display]\nanisotropic_filtering=2\n");
	snprintf(path, sizeof(path), "%s/tier-drop.toml", folder);
	profile = resolve(folder, target, path, "");
	assert(has(&profile, "display.anisotropic_filtering", "2") && profile.inherited && profile.drop_in);
	/* Known devices below/equal/above the measured tiers, across platforms. */
	snprintf(path, sizeof(path), "%s/hardware/catalog.toml", folder);
	catalog = fopen(path, "ab"); assert(catalog);
	fputs("\n[[hardware]]\nid=\"a8\"\nplatform=\"ios\"\nmodels=[\"Chip8,1\"]\ntier=8\ngpu_family=2\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[hardware]]\nid=\"equal\"\nplatform=\"ios\"\nmodels=[\"Chip12,2\"]\ntier=12\ngpu_family=6\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[hardware]]\nid=\"above\"\nplatform=\"ios\"\nmodels=[\"Chip15,1\"]\ntier=15\ngpu_family=6\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[hardware]]\nid=\"other-platform\"\nplatform=\"tvos\"\nmodels=[\"Chip11,1\"]\ntier=15\ngpu_family=6\nbasis=\"assumed\"\nsource=\"synthetic\"\n", catalog);
	fclose(catalog);
	snprintf(target.model, sizeof(target.model), "Chip8,1");
	profile = resolve(folder, target, NULL, ""); assert(!profile.model_file[0]);
	snprintf(target.model, sizeof(target.model), "Chip12,2");
	profile = resolve(folder, target, NULL, ""); assert(profile.inherited && profile.source_tier == 12);
	snprintf(target.model, sizeof(target.model), "Chip15,1");
	profile = resolve(folder, target, NULL, ""); assert(profile.inherited && profile.source_tier == 12);
	snprintf(target.platform, sizeof(target.platform), "tvos");
	snprintf(target.model, sizeof(target.model), "Chip11,1");
	profile = resolve(folder, target, NULL, ""); assert(!profile.model_file[0]);
	snprintf(target.platform, sizeof(target.platform), "ios");
	/* A conflicting catalog is not resolved by filename or directory order. */
	snprintf(path, sizeof(path), "%s/hardware/catalog.toml", folder);
	catalog = fopen(path, "ab"); assert(catalog);
	fputs("\n[[fallback]]\nhardware=\"a9\"\nprofile=\"tier-a9.toml\"\nminimum_memory_mb=2048\nminimum_gpu_family=2\nmaximum_pixels=[1000000,1000000,1000000]\nmaximum_refresh_hertz=60\n", catalog);
	fclose(catalog);
	profile = resolve(folder, target, NULL, "");
	assert(!profile.inherited && !profile.model_file[0] && strstr(logged, "invalid hardware catalog"));
	write_file("hardware/catalog.toml", "schema=\"broken\"\n");
	profile = resolve(folder, target, NULL, ""); assert(!profile.inherited && !profile.model_file[0]);
}

static void check_minimum_height_envelope(void)
{
	struct host_device target = device("ios", "Chip11,1", "", 5);
	struct host_profile profile;
	target.memory_megabytes = 4096; target.pixel_width = 1920; target.pixel_height = 1080; target.refresh_rate = 60;
	write_file("tier-a9.toml", "schema=1\nname=\"A9\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\n[display]\nrender_height=120\n");
	write_file("hardware/catalog.toml", "schema=1\nversion=\"minimum-height\"\n"
		"[[hardware]]\nid=\"a9\"\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\ntier=9\ngpu_family=2\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[hardware]]\nid=\"a11\"\nplatform=\"ios\"\nmodels=[\"Chip11,1\"]\ntier=11\ngpu_family=5\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[fallback]]\nhardware=\"a9\"\nprofile=\"tier-a9.toml\"\nminimum_memory_mb=2048\nminimum_gpu_family=2\nmaximum_pixels=[30000,30000,30000]\nmaximum_refresh_hertz=60\n");
	/* 120 requested lines allocate at least 427x240, exceeding this 30,000-pixel envelope. */
	profile = resolve(folder, target, NULL, ""); assert(!profile.inherited);
	/* The built-in image fits, but an external render target must fit the same envelope. */
	write_file("hardware/catalog.toml", "schema=1\nversion=\"external-height\"\n"
		"[[hardware]]\nid=\"a9\"\nplatform=\"ios\"\nmodels=[\"Chip9,1\"]\ntier=9\ngpu_family=2\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[hardware]]\nid=\"a11\"\nplatform=\"ios\"\nmodels=[\"Chip11,1\"]\ntier=11\ngpu_family=5\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
		"[[fallback]]\nhardware=\"a9\"\nprofile=\"tier-a9.toml\"\nminimum_memory_mb=2048\nminimum_gpu_family=2\nmaximum_pixels=[1000000,1000000,1000000]\nmaximum_refresh_hertz=60\n");
	profile = resolve(folder, target, NULL, ""); assert(profile.inherited);
	char path[256]; snprintf(path, sizeof(path), "%s/tier-a9.toml", folder);
	FILE *file = fopen(path, "ab"); assert(file);
	fputs("[external.display]\noutput_height=1080\nrender_height=720\n", file); fclose(file);
	profile = resolve(folder, target, NULL, ""); assert(!profile.inherited);

}

static void check_refresh_envelope(void)
{
	char path[256];
	int mac, which;
	for (which = 0; which < 3; which++)
	{
		const char *platform = which == 1 ? "mac" : which == 2 ? "visionos" : "ios";
		mac = which == 1;
		struct host_device target = device(platform, "Rate11,1", "", 5);
		struct host_profile profile;
		FILE *file;
		target.memory_megabytes = 4096; target.pixel_width = 1920; target.pixel_height = 1080; target.refresh_rate = 120;
		snprintf(path, sizeof(path), "%s/rate-class.toml", folder); file = fopen(path, "wb"); assert(file);
		fprintf(file, "schema=1\nname=\"Rate class\"\nkind=\"class\"\n[match]\nplatform=\"%s\"\ngpu_family=[1,0]\n", platform); fclose(file);
		snprintf(path, sizeof(path), "%s/rate-source.toml", folder); file = fopen(path, "wb"); assert(file);
		fprintf(file, "schema=1\nname=\"Rate source\"\nkind=\"model\"\n[match]\nplatform=\"%s\"\nmodels=[\"Rate9,1\"]\n[display]\nframe_rate=\"60\"\n[scene.menu.display]\nframe_rate=\"%s\"\n[scene.cutscene.display]\nframe_rate=\"30\"\n", platform, mac ? "120" : "30"); fclose(file);
		snprintf(path, sizeof(path), "%s/hardware/catalog.toml", folder); file = fopen(path, "wb"); assert(file);
		fprintf(file, "schema=1\nversion=\"refresh\"\n"
			"[[hardware]]\nid=\"source\"\nplatform=\"%s\"\nmodels=[\"Rate9,1\"]\ntier=9\ngpu_family=2\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
			"[[hardware]]\nid=\"target\"\nplatform=\"%s\"\nmodels=[\"Rate11,1\"]\ntier=11\ngpu_family=5\nbasis=\"assumed\"\nsource=\"synthetic\"\n"
			"[[fallback]]\nhardware=\"source\"\nprofile=\"rate-source.toml\"\nminimum_memory_mb=2048\nminimum_gpu_family=2\nmaximum_pixels=[100000000,100000000,100000000]\nmaximum_refresh_hertz=60\n", platform, platform); fclose(file);
		profile = resolve(folder, target, NULL, "");
		/* The base 60 cap holds an iPhone's 120 Hz screen at 60, and on Mac it also caps the 120 menu; a Vision Pro
		   ignores it, so its 120 is over the tier's 60. */
		assert(profile.inherited == (which != 2));
		/* A source that states no rate gives a 120 Hz display nothing to run at, so the 60 Hz envelope rejects it. */
		snprintf(path, sizeof(path), "%s/rate-source.toml", folder); file = fopen(path, "wb"); assert(file);
		fprintf(file, "schema=1\nname=\"Rate source\"\nkind=\"model\"\n[match]\nplatform=\"%s\"\nmodels=[\"Rate9,1\"]\n[display]\nanisotropic_filtering=4\n", platform); fclose(file);
		profile = resolve(folder, target, NULL, ""); assert(!profile.inherited);
	}
}

static void check_external(void)
{
	struct host_profile profile;
	char encoded[4096];
	write_file("external.toml", "schema = 1\nname = \"External\"\nkind = \"model\"\n[match]\nplatform = \"ios\"\n"
		"models = [\"External1,1\"]\n[display]\nrender_height = 540\noutput_height = 1080\n"
		"[external.display]\nrender_height = 720\noutput_height = 1440\nrenderer = \"gl\"\n"
		"[external.scene.cutscene.display]\nrender_scale = 0.75\n"
		"[external.scene.loading.display]\nrender_scale = 0.5\n");
	profile = resolve(folder, device("ios", "External1,1", "", 5), NULL, "");
	assert(has(&profile, "display.render_height", "540"));
	assert(unset(&profile, "display.output_height"));
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "display.render_height\t720\tmodel External\t\texternal\n"));
	assert(strstr(encoded, "display.output_height\t1440\tmodel External\t\texternal\n"));
	assert(strstr(encoded, "display.render_scale\t0.75\tmodel External\tcutscene\texternal\n"));
	assert(!strstr(encoded, "display.renderer\tgl") && !strstr(encoded, "loading"));
	char value[32];
	assert(host_profile_external_value(&profile, "display.output_height", value, sizeof(value)) && !strcmp(value, "1440"));
	assert(host_profile_external_value(&profile, "display.render_height", value, sizeof(value)) && !strcmp(value, "720"));
	char path[256]; snprintf(path, sizeof(path), "%s/external.toml", folder); unlink(path);
	/* a tvOS profile's own [display] caps the built-in output; a scene class may not */
	write_file("builtin-cap.toml", "schema = 1\nname = \"Capped\"\nkind = \"model\"\n[match]\nplatform = \"tvos\"\n"
		"models = [\"Capped1,1\"]\n[display]\nrender_height = 720\noutput_height = 720\n"
		"[scene.menu.display]\noutput_height = 540\n");
	profile = resolve(folder, device("tvos", "Capped1,1", "", 3), NULL, "");
	assert(has(&profile, "display.output_height", "720") && has(&profile, "display.render_height", "720"));
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "display.output_height\t720\tmodel Capped\n") && !strstr(encoded, "\t540\t"));
	snprintf(path, sizeof(path), "%s/builtin-cap.toml", folder); unlink(path);
}

/* [variable.display]: the variable workload context's values, carried as the pseudo-class "variable"; only
PROFILE_VARIABLE settings, only [variable.display], and never the profile's own value */
static void check_variable(void)
{
	struct host_profile profile;
	char encoded[4096], value[32], path[256];

	write_file("variable.toml", "schema = 1\nname = \"Variable\"\nkind = \"model\"\n[match]\nplatform = \"ios\"\n"
		"models = [\"Variable1,1\"]\n[display]\nrender_height = 1080\ndynamic_resolution = false\n"
		"[variable.display]\ndynamic_resolution = true\ndynamic_resolution_floor = 540\nshadow_map_size = 128\n"
		"anti_aliasing = \"fxaa\"\nframe_rate = \"30\"\nanisotropic_filtering = 4\n"
		"[variable.network]\nplayer_capacity = 16\n"
		"[scene.variable.display]\nrender_scale = 0.5\n");
	logged[0] = 0;
	profile = resolve(folder, device("ios", "Variable1,1", "", 5), NULL, "");
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "display.dynamic_resolution\ttrue\tmodel Variable\tvariable\n"));
	assert(strstr(encoded, "display.dynamic_resolution_floor\t540\tmodel Variable\tvariable\n"));
	assert(strstr(encoded, "display.shadow_map_size\t128\tmodel Variable\tvariable\n"));
	assert(strstr(encoded, "display.anti_aliasing\tfxaa\tmodel Variable\tvariable\n"));
	assert(strstr(encoded, "display.frame_rate\t30\tmodel Variable\tvariable\n"));
	/* refused: a setting outside PROFILE_VARIABLE, a section other than display, a scene class named variable */
	assert(!strstr(encoded, "anisotropic_filtering") && !strstr(encoded, "player_capacity") && !strstr(encoded, "0.5"));
	assert(strstr(logged, "display.anisotropic_filtering cannot change by workload; ignored"));
	assert(strstr(logged, "network.player_capacity cannot change by workload; ignored"));
	assert(strstr(logged, "scene class \"variable\" is not one this build knows; ignored"));
	/* the profile's own value is not the variable one; the variable lookup finds it */
	assert(has(&profile, "display.dynamic_resolution", "false"));
	assert(host_profile_scene_value(&profile, "display.dynamic_resolution", "variable", value, sizeof(value)) &&
		!strcmp(value, "true"));
	assert(!host_profile_scene_value(&profile, "display.render_height", "variable", value, sizeof(value)));
	snprintf(path, sizeof(path), "%s/variable.toml", folder); unlink(path);
}

static void check_and_newer(void)
{
	char previous[64], path[256];
	struct host_profile profile;
	snprintf(previous, sizeof(previous), "%s", folder);
	snprintf(folder, sizeof(folder), "/tmp/profile-newer-XXXXXX"); assert(mkdtemp(folder));
	write_file("a-old.toml", "schema=1\nname=\"Older rule\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"iPhone18,1\",\"iPhone18,2\"]\nand_newer=true\n[display]\nshadow_map_size=256\n[external.display]\noutput_height=720\n");
	write_file("b-new.toml", "schema=1\nname=\"Newer rule\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"iPhone19,1\"]\nand_newer=true\n[display]\nshadow_map_size=512\n");
	profile = resolve(folder, device("ios", "iPhone20,2", "", 10), NULL, "");
	assert(!strcmp(profile.model_file, "b-new.toml") && has(&profile, "display.shadow_map_size", "512"));
	assert(strstr(logged, "and_newer b-new.toml: iPhone20,2") && strstr(logged, "iPhone19,1"));
	/* Another minor of the newest source generation is not newer than that rule. */
	profile = resolve(folder, device("ios", "iPhone19,9", "", 10), NULL, "");
	assert(!strcmp(profile.model_file, "a-old.toml"));
	char height[32]; assert(host_profile_external_value(&profile, "display.output_height", height, sizeof(height)) && !strcmp(height, "720"));
	profile = resolve(folder, device("ios", "iPhone18,9", "", 10), NULL, ""); assert(!profile.model_file[0]);
	profile = resolve(folder, device("ios", "iPad99,1", "", 10), NULL, ""); assert(!profile.model_file[0]);
	profile = resolve(folder, device("ios", "iPhone999999999999999999999,1", "", 10), NULL, ""); assert(!profile.model_file[0]);
	profile = resolve(folder, device("ios", "iPhone99,1", "", 10), NULL, ""); assert(!strcmp(profile.model_file, "b-new.toml"));
	/* Exact wins even though its filename sorts after both rules. */
	write_file("z-exact.toml", "schema=1\nname=\"Exact\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"iPhone20,2\"]\n[display]\nshadow_map_size=128\n");
	profile = resolve(folder, device("ios", "iPhone20,2", "", 10), NULL, "");
	assert(!strcmp(profile.model_file, "z-exact.toml") && has(&profile, "display.shadow_map_size", "128"));
	assert(!strstr(logged, "and_newer b-new.toml:"));
	write_file("mac.toml", "schema=1\nname=\"Pro rule\"\nkind=\"model\"\n[match]\nplatform=\"mac\"\nchips=[\"Apple M4 Pro\"]\nand_newer=true\n[display]\nshadow_map_size=256\n");
	const char *covered[] = {"Apple M4 Pro", "Apple M4 Max", "Apple M4 Ultra", "Apple M5 Pro", "Apple M99 Max"};
	const char *uncovered[] = {"Apple M4", "Apple M5", "Apple M3 Ultra", "Apple M5 Extreme", "Apple M99999999999999999999 Pro"};
	for (int i = 0; i < 5; i++)
	{
		profile = resolve(folder, device("mac", "Mac99,1", covered[i], 10), NULL, ""); assert(!strcmp(profile.model_file, "mac.toml"));
		profile = resolve(folder, device("mac", "Mac99,1", uncovered[i], 10), NULL, ""); assert(!profile.model_file[0]);
	}
	/* With the same generation threshold, the higher qualifying tier wins. */
	write_file("mac-max.toml", "schema=1\nname=\"Max rule\"\nkind=\"model\"\n[match]\nplatform=\"mac\"\nchips=[\"Apple M4 Max\"]\nand_newer=true\n[display]\nshadow_map_size=512\n");
	profile = resolve(folder, device("mac", "Mac99,1", "Apple M5 Ultra", 10), NULL, ""); assert(!strcmp(profile.model_file, "mac-max.toml"));
	profile = resolve(folder, device("mac", "Mac99,1", "Apple M5 Pro", 10), NULL, ""); assert(!strcmp(profile.model_file, "mac.toml"));
	/* A qualifying rule precedes catalog inheritance; removing it restores the
	   measured fallback and its origin metadata unchanged. */
	snprintf(path, sizeof(path), "%s/hardware", folder); assert(!mkdir(path, 0700));
	write_file("source-class.toml", "schema=1\nname=\"Class\"\nkind=\"class\"\n[match]\nplatform=\"ios\"\ngpu_family=[1,0]\n");
	write_file("source.toml", "schema=1\nname=\"Measured\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"iPhone17,1\"]\n[display]\nshadow_map_size=128\n");
	write_file("hardware/catalog.toml", "schema=1\nversion=\"newer-test\"\n"
		"[[hardware]]\nid=\"source\"\nplatform=\"ios\"\nmodels=[\"iPhone17,1\"]\ntier=1\ngpu_family=7\nbasis=\"measured\"\nsource=\"Synthetic fixture\"\n"
		"[[hardware]]\nid=\"target\"\nplatform=\"ios\"\nmodels=[\"iPhone21,1\"]\ntier=2\ngpu_family=10\nbasis=\"assumed\"\nsource=\"Synthetic fixture\"\n"
		"[[fallback]]\nhardware=\"source\"\nprofile=\"source.toml\"\nminimum_memory_mb=2048\nminimum_gpu_family=7\nmaximum_pixels=[1000000,1000000,1000000]\nmaximum_refresh_hertz=60\n");
	struct host_device target = device("ios", "iPhone21,1", "", 10);
	target.memory_megabytes = 8192; target.pixel_width = 1000; target.pixel_height = 500; target.refresh_rate = 60;
	/* The catalog lists the target, so its answer precedes the rule that would also match it. */
	profile = resolve(folder, target, NULL, "");
	assert(profile.inherited && !strcmp(profile.model_file, "source.toml") && !strstr(logged, "and_newer"));
	/* An identifier the catalog does not list still takes the rule. */
	struct host_device unlisted = target; snprintf(unlisted.model, sizeof(unlisted.model), "iPhone22,1");
	profile = resolve(folder, unlisted, NULL, "");
	assert(!profile.inherited && !strcmp(profile.model_file, "b-new.toml") && strstr(logged, "and_newer b-new.toml"));
	const char *rules[] = {"a-old.toml", "b-new.toml", "z-exact.toml", "mac.toml", "mac-max.toml"};
	for (int i = 0; i < 5; i++) { snprintf(path, sizeof(path), "%s/%s", folder, rules[i]); unlink(path); }
	profile = resolve(folder, target, NULL, "");
	assert(profile.inherited && !strcmp(profile.model_file, "source.toml") && has(&profile, "display.shadow_map_size", "128"));
	assert(!strcmp(profile.catalog_version, "newer-test") && !strcmp(profile.target_hardware, "target") && !strcmp(profile.source_hardware, "source"));
	const char *names[] = {"source.toml", "source-class.toml", "hardware/catalog.toml"};
	for (int i = 0; i < 3; i++) { snprintf(path, sizeof(path), "%s/%s", folder, names[i]); unlink(path); }
	snprintf(path, sizeof(path), "%s/hardware", folder); rmdir(path);
	rmdir(folder); snprintf(folder, sizeof(folder), "%s", previous);
}

static void check_long_profile_path(void)
{
	char root[] = "/tmp/profile-path-XXXXXX", folder_path[1024], next[1024];
	char filename[64], component[181];
	size_t root_length, folder_length;
	char *separator;
	int directory, file, accepted, reported;

	assert(mkdtemp(root));
	root_length = strlen(root);
	strcpy(folder_path, root);
	while ((folder_length = strlen(folder_path)) < 960)
	{
		size_t component_length = 180;
		if (folder_length + 1 + component_length > 960)
			component_length = 960 - folder_length - 1;
		memset(component, 'd', component_length);
		component[component_length] = 0;
		memcpy(next, folder_path, folder_length);
		next[folder_length] = '/';
		memcpy(next + folder_length + 1, component, component_length + 1);
		assert(!mkdir(next, 0700));
		strcpy(folder_path, next);
	}
	memset(filename, 'a', sizeof(filename) - 6);
	memcpy(filename + sizeof(filename) - 6, ".toml", 6);
	directory = open(folder_path, O_RDONLY | O_DIRECTORY); assert(directory >= 0);
	file = openat(directory, filename, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(file >= 0);
	assert(!close(file));
	logged[0] = 0;
	accepted = host_profile_validate_folder(folder_path, log_line);
	reported = strstr(logged, "profile update: path too long") != NULL;
	assert(!unlinkat(directory, filename, 0));
	assert(!close(directory));
	while (strcmp(folder_path, root))
	{
		assert(!rmdir(folder_path));
		separator = strrchr(folder_path, '/');
		assert(separator && (size_t)(separator - folder_path) >= root_length);
		*separator = 0;
	}
	assert(!rmdir(root));
	assert(!accepted);
	assert(reported);
}

static void hot_apply_merge(void)
{
    struct host_profile old = {0}, next = {0}, merged;
    old.count = HOST_PROFILE_MAXIMUM_VALUES;
    for (int i = 0; i < old.count; i++) {
        snprintf(old.values[i].name, sizeof(old.values[i].name), "display.high_res_hud");
        snprintf(old.values[i].text, sizeof(old.values[i].text), "%s", i == old.count - 1 ? "true" : "false");
    }
    next.count = 3;
    snprintf(next.values[0].name, sizeof(next.values[0].name), "display.render_height");
    snprintf(next.values[0].text, sizeof(next.values[0].text), "900");
    next.values[1] = next.values[0];
    snprintf(next.values[1].text, sizeof(next.values[1].text), "1080");
    next.values[2] = next.values[1];
    snprintf(next.values[2].scene, sizeof(next.values[2].scene), "cutscene");
    next.values[2].external = 1;
    host_profile_merge_at_break(&old, &next, 2, 0, &merged);
    assert(merged.count == 3);
    char text[80];
    assert(host_profile_value(&merged, "display.high_res_hud", text, sizeof(text)) && !strcmp(text, "true"));
    assert(host_profile_value(&merged, "display.render_height", text, sizeof(text)) && !strcmp(text, "1080"));
    assert(merged.values[2].external && !strcmp(merged.values[2].scene, "cutscene"));
    host_profile_merge_at_break(&merged, &next, 1, 0, &old);
    assert(old.count == 2);
    assert(!host_profile_value(&old, "display.high_res_hud", text, sizeof(text)));
}

int main(int argc, char **argv)
{
    hot_apply_merge();
	char command[128];

	snprintf(folder, sizeof(folder), "/tmp/profile-probe-XXXXXX");
	if (!mkdtemp(folder))
	{
		perror("mkdtemp");
		return 1;
	}
	check_and_newer();
	check_file_read_paths();
	check_layers();
	check_external();
	check_variable();
	check_refusals();
	check_capability_fallback();
	check_minimum_height_envelope();
	check_refresh_envelope();
	check_long_profile_path();
	if (argc > 1)
		check_shipped(argv[1]);
	snprintf(command, sizeof(command), "rm -r %s", folder);
	system(command);
	puts("PASS: device profiles: class by GPU family, model over class, the drop-in file over both; refusals; the shipped profiles");
	return 0;
}
