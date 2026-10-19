/*
HOST_GPU_DISPATCH.C

gpu.h's entry points in the iOS, tvOS and visionOS host, which the guest's
host_gpu_* imports call (tools/ios_bridges.py): each calls the backend
gpu_initialize chose: GL (port/linux/src/gpu_gl.c) or, with
GPU_INITIALIZE_METAL (display.renderer = "metal"), Metal (gpu_metal.m).
visionOS has no OpenGL ES, and its build leaves the GL backend out.
*/

#include "gpu.h"
#include "ios_host.h"
#include "host_data_updates.h"
#include <string.h>
#include "host_benchmark.h"
#if HALO_RENDER_VALIDATION
#include "gpu_validate.h"
#include "gpu_capture.h"
#include "gpu_interface_hash.h"
#include "guest_image.h"
#include "host_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#endif

#include <TargetConditionals.h>
#include <mach/mach_time.h>
#include <time.h>

/* host_gpu.c's, which writes the guest's log */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

extern const struct gpu_backend gpu_backend_metal;
/* nothing calls gpu.h before gpu_initialize (d3d8_device.c's gl_initialize) */
#if TARGET_OS_VISION || defined(HALO_METAL_ONLY)
static const struct gpu_backend *backend = &gpu_backend_metal;
#else
extern const struct gpu_backend gpu_backend_gl;
static const struct gpu_backend *backend = &gpu_backend_gl;
#endif

/* the chosen backend's table with a gpu_present that first holds while the
app is in the background (host_lifecycle.m) and, with debug.frame_counter,
then counts the frame, so the count is the front end's frame number (one
Present each) under either backend */
static struct gpu_backend wrapped;
static const struct gpu_backend *wrapped_backend;
static int frame_counter;
static unsigned long presented;
static int ready_frame, updates_healthy;
void host_data_updates_frame_ready(int ready)
{
    /* main_loop sets this only after main_game_render returns. Loading frames
       can name a level too, but main_pregame_render clears it before drawing. */
    ready_frame = ready != 0;
}
static void data_updates_presented(void)
{
    if (ready_frame && !updates_healthy) {
        updates_healthy = 1;
        host_data_updates_healthy();
    }
    ready_frame = 0;
}

/* the device benchmark's frames under the GL backend, which iPhones and iPads use unless a profile asks for Metal: the
Metal backend reports its own, with the GPU's time (gpu_metal.m's pacing_schedule), and this must not count them twice.
The interval is from one Present's return to the next's; the CPU time is the game's work between them. */
static int metal_backend;

static double uptime_seconds(void)
{
	return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

static uint32_t present_wrapped(gpu_texture back_buffer)
{
	static double last_returned;
	uint32_t next_frame_due;
	double called = uptime_seconds();

	host_lifecycle_hold();
	next_frame_due = wrapped_backend->present(back_buffer);
	data_updates_presented();
	if (!metal_backend)
	{
		double returned = uptime_seconds();

		if (last_returned > 0.0)
			host_benchmark_frame(returned - last_returned, called - last_returned, -1.0, 0.0);
		last_returned = returned;
	}
	if (frame_counter)
		host_frame_counter_show(presented++);
	return next_frame_due;
}

/* a stereo frame is a Present too */
static uint32_t present_stereo_wrapped(const struct gpu_stereo_present *present)
{
	uint32_t next_frame_due;

	host_lifecycle_hold();
	next_frame_due = wrapped_backend->present_stereo(present);
	data_updates_presented();
	if (frame_counter)
		host_frame_counter_show(presented++);
	return next_frame_due;
}

/* display.frame_rate (GPU_INITIALIZE_FRAME_RATE_SHIFT). The Mac app alone
honors it: everywhere else the system paces frames (the display link on an
iPhone, iPad or Apple TV, the Compositor on a Vision Pro), and the setting
is ignored with a line in the log. A cap is held by sleeping before a frame
is shown (host_frame_rate_limit: gpu_metal.m's Present, host_gpu.c's swap);
going faster than the display is the Metal backend's to arrange
(frame_rate_sync), and the GL backend, which can't, holds only caps under
the display's rate. */
static struct
{
	int unlimited;
	double cap;
	/* mach_absolute_time's units: a frame's share of a second, and when the
	next frame may show */
	uint64_t interval, next;
} frame_rate;

static void frame_rate_initialize(uint32_t flags)
{
	uint32_t rate = flags >> GPU_INITIALIZE_FRAME_RATE_SHIFT;

	if (!rate)
		return;
#if TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
	if (rate == GPU_INITIALIZE_FRAME_RATE_UNLIMITED)
	{
		frame_rate.unlimited = 1;
		platform_log("display.frame_rate: unlimited%s", (flags & GPU_INITIALIZE_METAL) ? "; frames don't wait for the display" :
			" needs the Metal renderer; OpenGL ES waits for the display");
	}
	else
	{
		mach_timebase_info_data_t timebase;

		mach_timebase_info(&timebase);
		frame_rate.cap = (double)rate;
		frame_rate.interval = (uint64_t)(1e9 / (double)rate * (double)timebase.denom / (double)timebase.numer);
		platform_log("display.frame_rate: at most %u frames a second%s", (unsigned)rate, (flags & GPU_INITIALIZE_METAL) ? "" :
			" (OpenGL ES also waits for the display)");
	}
#else
	platform_log("display.frame_rate is for the Mac app; here the system paces frames, and the setting is ignored");
#endif
}

int host_frame_rate_unlimited(void)
{
	return frame_rate.unlimited;
}

double host_frame_rate_cap(void)
{
	return frame_rate.cap;
}

/* (set and read on the game's thread, which draws and presents) */
static double scene_frame_rate;

void host_frame_rate_scene(double rate)
{
	scene_frame_rate = rate;
}

double host_frame_rate_target(void)
{
	return scene_frame_rate > 0.0 && (frame_rate.cap <= 0.0 || scene_frame_rate < frame_rate.cap) ? scene_frame_rate :
		frame_rate.cap;
}

double host_frame_rate_limit(void)
{
	uint64_t now;

	if (!frame_rate.interval)
		return 0.0;
	now = mach_absolute_time();
	if (frame_rate.next > now)
	{
		mach_timebase_info_data_t timebase;
		uint64_t wait = frame_rate.next - now;

		mach_wait_until(frame_rate.next);
		frame_rate.next += frame_rate.interval;
		mach_timebase_info(&timebase);
		return (double)wait * (double)timebase.numer / (double)timebase.denom / 1e9;
	}
	/* a frame that came late doesn't earn the next ones a shorter wait */
	if (now - frame_rate.next > frame_rate.interval)
		frame_rate.next = now;
	frame_rate.next += frame_rate.interval;
	return 0.0;
}

#if HALO_RENDER_VALIDATION
/* render validation (gpu_validate.h), in a build configured with it: the
chosen backend's table with every function checked first. debug.render_validation
is "off", "log" (the default in such a build) or "strict"; the environment's
HALO_RENDER_VALIDATION overrides it, as the game's settings are overridden.
debug.render_validation_allow (HALO_RENDER_VALIDATION_ALLOW) names the
reasons a strict run goes on past. */
static struct gpu_backend validated;

static void validation_log(const char *line)
{
	platform_log("%s", line);
}

static void validation_stop(const char *line)
{
	platform_log("%s", line);
	host_exit(3);
}

static const struct gpu_backend *validation_wrap(const struct gpu_backend *chosen)
{
	static char mode[32], allow[512];
	struct gpu_validation_host host = { validation_log, validation_stop, GPU_VALIDATION_LOG, allow };

	host_config_string("debug.render_validation", "log", mode, sizeof(mode));
	if (getenv("HALO_RENDER_VALIDATION"))
		snprintf(mode, sizeof(mode), "%s", getenv("HALO_RENDER_VALIDATION"));
	host_config_string("debug.render_validation_allow", "", allow, sizeof(allow));
	if (getenv("HALO_RENDER_VALIDATION_ALLOW"))
		snprintf(allow, sizeof(allow), "%s", getenv("HALO_RENDER_VALIDATION_ALLOW"));
	if (!strcmp(mode, "off"))
	{
		platform_log("render validation: off (debug.render_validation)");
		return chosen;
	}
	if (!strcmp(mode, "strict"))
		host.mode = GPU_VALIDATION_STRICT;
	else if (!strcmp(mode, "reject"))
		host.mode = GPU_VALIDATION_REJECT;
	else if (strcmp(mode, "log"))
		platform_log("render validation: debug.render_validation = \"%s\" is not off, log or strict; taken as log", mode);
	validated = *chosen;
	gpu_validation_install(&validated, &host);
	return &validated;
}

/* render capture (gpu_capture.h): debug.render_capture names a folder (in
the data folder unless a full path) and debug.render_capture_frames the
windows of frames that go there, one file each ("600+30,1200+30"); the
environment's HALO_RENDER_CAPTURE and HALO_RENDER_CAPTURE_FRAMES override
them. The capture sees the calls before the validator does. A replay
(host_render_replay.c) is never captured. */
static struct gpu_backend captured;

static const struct gpu_backend *capture_wrap(const struct gpu_backend *chosen, uint32_t flags)
{
	static char folder[1024], frames[256], label[64];
	struct gpu_capture_host host = { validation_log, folder, frames, GPU_INTERFACE_HASH, GPU_FRONT_END_HASH, HALO_GUEST_SHA256, label };

	host_config_string("debug.render_capture", "", folder, sizeof(folder));
	if (getenv("HALO_RENDER_CAPTURE"))
		snprintf(folder, sizeof(folder), "%s", getenv("HALO_RENDER_CAPTURE"));
	host_config_string("debug.render_capture_frames", "", frames, sizeof(frames));
	if (getenv("HALO_RENDER_CAPTURE_FRAMES"))
		snprintf(frames, sizeof(frames), "%s", getenv("HALO_RENDER_CAPTURE_FRAMES"));
	if (!folder[0] || !frames[0] || getenv("HALO_RENDER_REPLAY"))
		return chosen;
	mkdir(folder, 0755);
	snprintf(label, sizeof(label), "%s%s%s", (flags & GPU_INITIALIZE_METAL) ? "metal" : "gl",
		getenv("HALO_RENDER_CAPTURE_LABEL") ? " " : "", getenv("HALO_RENDER_CAPTURE_LABEL") ? getenv("HALO_RENDER_CAPTURE_LABEL") : "");
	captured = *chosen;
	gpu_capture_install(&captured, &host);
	return &captured;
}
#endif

void gpu_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
#if defined(HALO_METAL_ONLY)
	if (!(flags & GPU_INITIALIZE_METAL))
		platform_log("display.renderer requests OpenGL ES, but this build includes Metal only; using Metal");
	flags |= GPU_INITIALIZE_METAL;
#endif
#if TARGET_OS_VISION
	/* host_main.m sets HALO_RENDERER=metal, which overrides config.toml */
	if (!(flags & GPU_INITIALIZE_METAL))
		host_fatal("visionOS draws with Metal only, but display.renderer isn't \"metal\"");
	backend = &gpu_backend_metal;
#elif defined(HALO_METAL_ONLY)
	backend = &gpu_backend_metal;
#else
	backend = (flags & GPU_INITIALIZE_METAL) ? &gpu_backend_metal : &gpu_backend_gl;
#endif
#if HALO_RENDER_VALIDATION
	backend = validation_wrap(backend);
	backend = capture_wrap(backend, flags);
#endif
	metal_backend = TARGET_OS_VISION || (flags & GPU_INITIALIZE_METAL) ? 1 : 0;
	wrapped_backend = backend;
	wrapped = *backend;
	wrapped.present = present_wrapped;
	wrapped.present_stereo = present_stereo_wrapped;
	backend = &wrapped;
	host_frame_counter_stop();
	presented = 0;
	frame_counter = (flags & GPU_INITIALIZE_FRAME_COUNTER) != 0;
	if (frame_counter)
		host_frame_counter_start((flags & GPU_INITIALIZE_FIXED_TIMESTEP) != 0, (flags & GPU_INITIALIZE_METAL) != 0);
	host_lifecycle_install();
	frame_rate_initialize(flags);
	backend->initialize(flags, capabilities);
}

#define DISPATCH_FUNCTION(type, name, parameters, arguments) type gpu_##name parameters { return backend->name arguments; }
#define DISPATCH_PROCEDURE(name, parameters, arguments) void gpu_##name parameters { backend->name arguments; }
GPU_OPERATIONS(DISPATCH_FUNCTION, DISPATCH_PROCEDURE)
