/* Device profiles (port/ios/host/host_profile.c): which files a device gets, the layers' order, what a profile
   may not set, and that nothing in a profile folder can leave the loader without an answer. The shipped
   profiles/ folder is checked for the devices we have measured: tools/ios_test.py passes its path. */
#include "host_profile.h"

#include <assert.h>
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

static void check_layers(void)
{
	char drop_in[256], missing[256], encoded[4096];
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
		"frame_pacing = \"tick\"\nmirror_resolution = \"half|full\"\nanti_aliasing = \"taa\"\n"
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
		has(&profile, "display.shadow_map_size", "256"));
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "\tmodel Apple TV HD\tmenu\n") && strstr(encoded, "\tmodel Apple TV HD\tcutscene\n"));
	/* an Apple TV 4K, and the tvOS simulator: what the build's default was */
	profile = resolve(shipped, device("tvos", "AppleTV11,1", "", 5), NULL, "");
	assert(has(&profile, "display.render_height", "1080") && !profile.model_file[0] && profile.count == 3);
	/* the two Apple TV 4K models with estimated profiles: 720 in play on the first, 1080 on the third, and a
	menu at the display's height */
	profile = resolve(shipped, device("tvos", "AppleTV6,2", "", 3), NULL, "");
	assert(has(&profile, "display.render_height", "720") && has(&profile, "display.anisotropic_filtering", "4") &&
		has(&profile, "display.renderer", "metal") && !strcmp(profile.model_file, "apple-tv-4k-1st.toml"));
	host_profile_encode(&profile, encoded, sizeof(encoded));
	assert(strstr(encoded, "display.render_height\t1080\tmodel Apple TV 4K (1st generation)\tmenu\n"));
	profile = resolve(shipped, device("tvos", "AppleTV14,1", "", 8), NULL, "");
	assert(has(&profile, "display.render_height", "1080") && has(&profile, "display.anisotropic_filtering", "16") &&
		!strcmp(profile.model_file, "apple-tv-4k-3rd.toml"));
	/* Vision Pro: the headset sweep's values, which are the built-in defaults too */
	profile = resolve(shipped, device("visionos", "RealityDevice14,1", "", 8), NULL, "");
	assert(has(&profile, "display.render_quality", "0.6") && has(&profile, "display.foveation", "true"));
	assert(!strcmp(profile.model_file, "vision-pro-m2.toml") && profile.count == 5 &&
		has(&profile, "display.renderer", "metal"));
	/* iPhone 17 Pro, iPad Pro (M1) and the Macs: values equal to the built-in defaults, and the iPhone and iPad
	class profile's Metal */
	profile = resolve(shipped, device("ios", "iPhone18,1", "", 10), NULL, "");
	assert(profile.count == 4 && has(&profile, "display.anisotropic_filtering", "16") &&
		has(&profile, "display.shadow_map_size", "512") && has(&profile, "display.renderer", "metal") &&
		has(&profile, "display.anti_aliasing", "fxaa"));
	profile = resolve(shipped, device("ios", "iPad13,9", "", 7), NULL, "");
	assert(profile.count == 4 && has(&profile, "display.mirror_resolution", "half") &&
		has(&profile, "display.renderer", "metal"));
	/* The smaller M1 iPad Pro inherits the measured M1 profile, subject to its envelope. */
	struct host_device sibling = device("ios", "iPad13,4", "", 7);
	sibling.memory_megabytes = 8192; sibling.pixel_width = 2388; sibling.pixel_height = 1668; sibling.refresh_rate = 120;
	profile = resolve(shipped, sibling, NULL, "");
	assert(profile.inherited && has(&profile, "display.mirror_resolution", "half"));
	assert(!strcmp(profile.model_file, "ipad-pro-m1.toml"));
	profile = resolve(shipped, device("mac", "Mac14,5", "Apple M2 Max", 8), NULL, "");
	assert(profile.count == 2 && has(&profile, "display.model_lod", "max") &&
		has(&profile, "display.compressed_textures", "true"));
	profile = resolve(shipped, device("mac", "Mac16,10", "Apple M4", 9), NULL, "");
	assert(profile.count == 1 && has(&profile, "display.shadow_map_size", "512"));
	profile = resolve(shipped, device("mac", "Mac17,1", "Apple M6", 11), NULL, "");
	assert(profile.count == 0 && !strcmp(profile.model_file, "mac-m6.toml"));
	/* a Mac, an iPhone and a headset nobody has measured: their class, nothing refused */
	profile = resolve(shipped, device("mac", "Mac15,3", "Apple M3 Max", 9), NULL, "");
	assert(profile.count == 0 && !strcmp(profile.class_file, "mac.toml") && !strstr(logged, "ignored"));
	profile = resolve(shipped, device("ios", "iPhone99,1", "", 15), NULL, "");
	assert(profile.count == 1 && has(&profile, "display.renderer", "metal") &&
		!strcmp(profile.class_file, "iphone-ipad.toml"));
	profile = resolve(shipped, device("visionos", "RealityDevice99,1", "", 15), NULL, "");
	assert(profile.count == 3 && !profile.model_file[0] && has(&profile, "display.renderer", "metal"));
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
	/* Exact/debug matching, unknown hardware, and insufficient memory keep their existing behavior. */
	target.memory_megabytes = 1024;
	profile = resolve(folder, target, NULL, "");
	assert(!profile.model_file[0] && has(&profile, "display.shadow_map_size", "256"));
	target.memory_megabytes = 4096; target.refresh_rate = 120;
	profile = resolve(folder, target, NULL, ""); assert(!profile.model_file[0]);
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
}

static void check_refresh_envelope(void)
{
	char path[256];
	int mac;
	for (mac = 0; mac < 2; mac++)
	{
		const char *platform = mac ? "mac" : "ios";
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
		/* The base 60 cap is ignored on iOS; on Mac it also caps the 120 menu. */
		assert(profile.inherited == mac);
	}
}

int main(int argc, char **argv)
{
	char command[128];

	snprintf(folder, sizeof(folder), "/tmp/profile-probe-XXXXXX");
	if (!mkdtemp(folder))
	{
		perror("mkdtemp");
		return 1;
	}
	check_layers();
	check_refusals();
	check_capability_fallback();
	check_minimum_height_envelope();
	check_refresh_envelope();
	if (argc > 1)
		check_shipped(argv[1]);
	snprintf(command, sizeof(command), "rm -r %s", folder);
	system(command);
	puts("PASS: device profiles: class by GPU family, model over class, the drop-in file over both; refusals; the shipped profiles");
	return 0;
}
