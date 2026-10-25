/* What this device is, and the device profile chosen for it (host_profile.h). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#if HALO_NATIVE_MAC
#include <SDL3/SDL.h>
#endif
#if HALO_NATIVE_MAC
#import <AppKit/AppKit.h>
#include "host_macos.h"
#else
#import <UIKit/UIKit.h>
#endif
#include "host_profile.h"
#import "host_data_updates.h"
#include "host_config.h"
#include "ios_host.h"
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "host_profile_merge.h"
#include "host_perf_reports.h"
#include <time.h>

static void sysctl_string(const char *name, char *text, size_t size)
{
	size_t length = size;

	if (sysctlbyname(name, text, &length, NULL, 0) != 0 || !length)
		text[0] = 0;
	else
		text[size - 1] = 0;
}

void host_device_identify(struct host_device *device)
{
	memset(device, 0, sizeof(*device));
#if TARGET_OS_TV
	snprintf(device->platform, sizeof(device->platform), "tvos");
#elif TARGET_OS_VISION
	snprintf(device->platform, sizeof(device->platform), "visionos");
#elif TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
	snprintf(device->platform, sizeof(device->platform), "mac");
#else
	snprintf(device->platform, sizeof(device->platform), "ios");
#endif
#if TARGET_OS_SIMULATOR
	{
		/* (hw.machine is the Mac's here; the simulated device's is in the environment) */
		const char *model = getenv("SIMULATOR_MODEL_IDENTIFIER");

		device->simulator = 1;
		snprintf(device->model, sizeof(device->model), "%s", model ? model : "");
	}
#elif TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
	/* (hw.machine is "arm64" on a Mac; one Mac identifier ships with several chips, so the chip is read too) */
	sysctl_string("hw.model", device->model, sizeof(device->model));
	sysctl_string("machdep.cpu.brand_string", device->chip, sizeof(device->chip));
#else
	sysctl_string("hw.machine", device->model, sizeof(device->model));
#endif
	device->memory_megabytes = (unsigned)(NSProcessInfo.processInfo.physicalMemory >> 20);
	@autoreleasepool
	{
		id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
		int family;

		if (gpu)
		{
			snprintf(device->gpu_name, sizeof(device->gpu_name), "%s", gpu.name.UTF8String ?: "");
			/* MTLGPUFamilyApple1 is 1001 and each later family the next number: asked by number, so a
			   family newer than the SDK this was built with still counts */
			for (family = 20; family >= 1 && !device->gpu_family; family--)
			{
				if ([gpu supportsFamily:(MTLGPUFamily)(1000 + family)])
					device->gpu_family = family;
			}
		}
#if HALO_NATIVE_MAC
        {
            host_macos_screen_size(&device->pixel_width, &device->pixel_height);
            device->refresh_rate=(int)host_macos_refresh_rate();
        }
#elif !TARGET_OS_VISION
		{
			UIScreen *screen = UIScreen.mainScreen;

			device->pixel_width = (int)screen.nativeBounds.size.width;
			device->pixel_height = (int)screen.nativeBounds.size.height;
			device->refresh_rate = (int)screen.maximumFramesPerSecond;
		}
#endif
	}
}

static void log_line(const char *line)
{
	host_logf(HOST_LOG_INFO, "%s", line);
}

static struct host_profile installed_profile;
static struct host_profile pending_profile, offered_profile;
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t installed_lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_values[HOST_PROFILE_MAXIMUM_VALUES * 256], pending_version[64];
static int pending_ready, offered_ready;
static char offered_version[64];
static char last_resolved_values[HOST_PROFILE_MAXIMUM_VALUES * 256];

int host_device_profile_pending(char *values, unsigned long capacity, char *version, unsigned long version_size)
{
    offered_ready = 0;
    if (!values || !capacity || (version_size && !version)) return 0;
    if (pthread_mutex_trylock(&pending_lock)) return 0;
    if (pending_ready) {
        size_t length = strlen(pending_values);
        /* Never offer a truncated snapshot: applied must refer to exactly the bytes the guest saw. */
        if (length < capacity) {
            offered_profile = pending_profile;
            snprintf(offered_version, sizeof(offered_version), "%s", pending_version);
            memcpy(values, pending_values, length + 1);
            if (version_size) snprintf(version, version_size, "%s", pending_version);
            offered_ready = 1;
        }
    }
    pthread_mutex_unlock(&pending_lock);
    return offered_ready;
}

void host_device_profile_applied(int point, int networked, int changed, int metalfx, int frame_rate)
{
    if (!offered_ready) return;
    /* Preserve old values that require a later break, including scene and external layers.
       Rebuild allowed layers from scratch so removed profile settings also disappear. */
    struct host_profile merged;
    pthread_mutex_lock(&installed_lock);
    host_profile_merge_at_break(&installed_profile, &offered_profile, point, networked, &merged);
    installed_profile = merged;
    pthread_mutex_unlock(&installed_lock);
    host_frame_rate_profile(frame_rate);
    /* Level loading permits the scaler's lazy allocation on the next presentation.
       The guest supplies the effective setting so explicit user overrides still win. */
    if (point == 1 && host_gpu_metal_active() && gpu_metal_upscaler() != (metalfx != 0))
        gpu_metal_set_upscaler(metalfx);
    if (changed) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t hash = UINT64_C(14695981039346656037);
        for (const unsigned char *p = (const unsigned char *)offered_version; *p; p++) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
        game_perf_event event = {
            .elapsed_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000,
            .type = GAME_PERF_EVENT_PROFILE_CHANGE, .flags = (unsigned)point, .value = hash
        };
        host_perf_reports_event(&event);
    }
    offered_ready = 0;
}

static int installed_value(const char *name, char *text, size_t size)
{
	pthread_mutex_lock(&installed_lock);
    int found = host_profile_value(&installed_profile, name, text, size);
    pthread_mutex_unlock(&installed_lock);
    return found;
}

/* Readers on UI/report threads own a consistent snapshot after releasing the lock. */
void host_device_profile_copy(struct host_profile *profile)
{
    pthread_mutex_lock(&installed_lock);
    *profile = installed_profile;
    pthread_mutex_unlock(&installed_lock);
}

static int profile_installed;

int host_device_profile_peek(const char *data_root, const char *name, char *text, size_t size)
{
	if (profile_installed)
		return installed_value(name, text, size);
	struct host_device device;
	struct host_profile *profile = calloc(1, sizeof(*profile));
	char selector[64], drop_in[1100];
	const char *environment = getenv("HALO_DEVICE_PROFILE");
	NSString *folder = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"profiles"];

	if (!profile)
		return 0;
	host_device_identify(&device);
	if (environment)
		snprintf(selector, sizeof(selector), "%s", environment);
	else
		host_config_string("debug.device_profile", "", selector, sizeof(selector));
	if (!selector[0])
		folder = host_profile_update_folder(folder);
	snprintf(drop_in, sizeof(drop_in), "%s/device-profile.toml", data_root);
	host_profile_resolve(&device, folder.fileSystemRepresentation, drop_in, selector, NULL, profile);
	int found = host_profile_value(profile, name, text, size);
	free(profile);
	return found;
}

const char *host_device_profile_install(const char *data_root)
{
	static char values[HOST_PROFILE_MAXIMUM_VALUES * 256];
	struct host_device device;
	char selector[64], drop_in[1100], display[64] = "";
	const char *environment = getenv("HALO_DEVICE_PROFILE");
	NSString *folder = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"profiles"];

	host_device_identify(&device);
	NSString *bundledFolder = folder;
	folder = host_profile_update_folder(folder);
	if (device.pixel_width)
		snprintf(display, sizeof(display), ", %dx%d at %d Hz", device.pixel_width, device.pixel_height, device.refresh_rate);
	host_logf(HOST_LOG_INFO, "device: %s%s%s (%s%s, GPU family %d \"%s\", %u MB%s)",
		device.model[0] ? device.model : "unknown model", device.chip[0] ? ", " : "", device.chip,
		device.platform, device.simulator ? " simulator" : "", device.gpu_family, device.gpu_name,
		device.memory_megabytes, display);
	/* debug.device_profile: the launch's environment, else config.toml (the working directory is the data folder) */
	if (environment)
		snprintf(selector, sizeof(selector), "%s", environment);
	else
		host_config_string("debug.device_profile", "", selector, sizeof(selector));
	snprintf(drop_in, sizeof(drop_in), "%s/device-profile.toml", data_root);
	if (selector[0]) folder = bundledFolder;
    struct host_profile initial;
    host_profile_resolve(&device, folder.fileSystemRepresentation, drop_in, selector, log_line, &initial);
    host_profile_encode(&initial, values, sizeof(values));
    pthread_mutex_lock(&installed_lock);
    installed_profile = initial;
    pthread_mutex_unlock(&installed_lock);
    host_config_set_profile(installed_value);
    profile_installed = 1;
    /* A selector is a deliberate frozen profile, including "none". Signed device packs
       always target actual hardware and must not override that choice. */
    if (!selector[0]) {
        NSString *dropInPath = @(drop_in);
        snprintf(last_resolved_values, sizeof(last_resolved_values), "%s", values);
        host_profile_updates_watch(dropInPath, ^(NSString *snapshotFolder, NSDictionary *identity) {
            struct host_profile resolved;
            char encoded[sizeof(pending_values)], version[sizeof(pending_version)];
            host_profile_resolve(&device, snapshotFolder.fileSystemRepresentation,
                dropInPath.fileSystemRepresentation, "", NULL, &resolved);
            host_profile_encode(&resolved, encoded, sizeof(encoded));
            if (!strcmp(encoded, last_resolved_values)) return;
            snprintf(last_resolved_values, sizeof(last_resolved_values), "%s", encoded);
            /* The catalog version identifies hardware fallback rules, not the signed generation. */
            if ([identity[@"bundled"] boolValue]) snprintf(version, sizeof(version), "local");
            else snprintf(version, sizeof(version), "%llu", [identity[@"sequence"] unsignedLongLongValue]);
            pthread_mutex_lock(&pending_lock);
            pending_profile = resolved;
            memcpy(pending_values, encoded, sizeof(pending_values));
            snprintf(pending_version, sizeof(pending_version), "%s", version);
            pending_ready = 1;
            pthread_mutex_unlock(&pending_lock);
        });
    }
	return values;
}
