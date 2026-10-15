/* What this device is, and the device profile chosen for it (host_profile.h). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include "host_profile.h"
#import "host_data_updates.h"
#include "host_config.h"
#include "ios_host.h"
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#elif TARGET_OS_MACCATALYST
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
#elif TARGET_OS_MACCATALYST
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
#if !TARGET_OS_VISION
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

static int installed_value(const char *name, char *text, size_t size)
{
	return host_profile_value(&installed_profile, name, text, size);
}

/* the profile host_device_profile_install resolved (empty before it has run), for the device benchmark's report */
const struct host_profile *host_device_profile(void)
{
	return &installed_profile;
}

const char *host_device_profile_install(const char *data_root)
{
	static char values[HOST_PROFILE_MAXIMUM_VALUES * 216];
	struct host_device device;
	char selector[64], drop_in[1100], display[64] = "";
	const char *environment = getenv("HALO_DEVICE_PROFILE");
	NSString *folder = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"profiles"];

	folder = host_profile_update_folder(folder);
	host_device_identify(&device);
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
	host_profile_resolve(&device, folder.fileSystemRepresentation, drop_in, selector, log_line, &installed_profile);
	host_config_set_profile(installed_value);
	host_profile_encode(&installed_profile, values, sizeof(values));
	return values;
}
