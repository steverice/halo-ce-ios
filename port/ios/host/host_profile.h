/* Device profiles (profiles/ in the repository, the app's Resources/profiles): the graphics settings chosen for
   one device model or class, in config.toml's vocabulary. The host finds out what it runs on (host_device.m),
   picks the class profile and the model profile that match, lays a drop-in file from the data folder over them,
   and hands the result to the game (port/linux/src/port_config.c reads HALO_DEVICE_PROFILE_VALUES) and to its
   own reader (host_config.c). Nothing here can stop a launch: a folder that is missing or a file that does not
   parse leaves its layer empty and says so in the log. */
#pragma once
#include <stddef.h>

struct host_device
{
	/* "ios" (iPhone and iPad), "tvos", "visionos" or "mac" */
	char platform[12];
	/* hw.machine ("AppleTV5,3"); hw.model on a Mac ("Mac14,5"); a simulator's SIMULATOR_MODEL_IDENTIFIER */
	char model[48];
	/* machdep.cpu.brand_string on a Mac ("Apple M2 Max"); empty elsewhere */
	char chip[64];
	/* the highest n for which the Metal device supports GPU family Apple n; 0 when there is none */
	int gpu_family;
	char gpu_name[64];
	unsigned memory_megabytes;
	int pixel_width, pixel_height, refresh_rate;
	int simulator;
};

enum
{
	HOST_PROFILE_MAXIMUM_VALUES = 128,
	/* a profile file larger than this is not read */
	HOST_PROFILE_MAXIMUM_FILE_BYTES = 64 * 1024,
};

struct host_profile_value
{
	/* "display.render_height" */
	char name[48];
	/* as config.toml writes it, a string without its quotes: true, 540, 0.6, metal */
	char text[80];
	/* the layer it came from, for the log: class "Apple TV" */
	char source[80];
	/* "" for the profile's own value; "menu" or "cutscene" for a scene class's ([scene.menu.display]) */
	char scene[12];
	/* values for an external output never replace the built-in display defaults */
	int external;
};

struct host_profile
{
	/* every layer's values, lowest layer first: a later one of the same name wins */
	struct host_profile_value values[HOST_PROFILE_MAXIMUM_VALUES];
	int count;
	/* the files in force ("" for none), for the log */
	char class_name[64], class_file[64], model_name[64], model_file[64];
	int drop_in;
	/* Actual and source hardware stay distinct in inherited-profile reports. */
	int inherited, source_tier, target_tier;
	char source_hardware[64], target_hardware[64], catalog_version[64];
};

/* The profile for device from the *.toml files in folder (NULL or missing: none), with the drop-in file's
   values over them (NULL or missing: none). selector is debug.device_profile: "" chooses by the device,
   "none" leaves every layer empty, anything else is a model identifier or chip name to choose as. Each log
   line goes to log (no newline). */
void host_profile_resolve(const struct host_device *device, const char *folder, const char *drop_in_path,
	const char *selector, void (*log)(const char *line), struct host_profile *profile);

/* the value in force for name (not a scene class's), as text; 0 if no profile sets it */
int host_profile_value(const struct host_profile *profile, const char *name, char *text, size_t size);

/* The highest non-scene external value, falling back to the built-in profile. */
int host_profile_external_value(const struct host_profile *profile, const char *name, char *text, size_t size);

/* every value as a "name\tvalue\tsource\n" line (a scene class's with "\tclass" after the source), lowest layer
   first, for the game; external values always have a fourth field (possibly empty)
   and a fifth field "external"; the length written */
size_t host_profile_encode(const struct host_profile *profile, char *text, size_t size);

/* host_device.m: what this is; and the profile resolved for it, once (host_main.m), with host_config.c's
   reader told to ask it. values is HALO_DEVICE_PROFILE_VALUES for the game's environment */
void host_device_identify(struct host_device *device);
const char *host_device_profile_install(const char *data_root);
const struct host_profile *host_device_profile(void);

/* Internal catalog selection: resolves a source without drop-ins or recursive fallback. */
int host_profile_inherit(const struct host_device *device, const char *folder,
	void (*log)(const char *), struct host_profile *profile);

/* Downloaded profile snapshots are accepted only as a complete valid set. */
int host_profile_validate_folder(const char *folder, void (*log)(const char *));
int host_profile_catalog_validate(const char *folder);
int host_profile_catalog_lists(const struct host_device *device, const char *folder);

/* Profile envelopes bind to exactly one canonical platform/model/chip key. */
int host_profile_device_key(const struct host_device *device, char *key, size_t size);
int host_profile_validate_device_folder(const char *folder, const struct host_device *device, void (*log)(const char *));
int host_profile_capacity_validate(const char *path);

int host_profile_catalog_device_validate(const char *folder, const struct host_device *device, const char *profile);
