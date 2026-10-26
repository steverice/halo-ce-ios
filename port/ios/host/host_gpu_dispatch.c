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
#include "host_perf_hooks.h"
#include "host_external.h"
#include "host_config.h"
#include "host_low_power.h"
#include "low_power.h"
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
#include <stdatomic.h>
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

int host_gpu_metal_active(void)
{
    return metal_backend && wrapped_backend != NULL;
}

static double uptime_seconds(void)
{
	return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

static void low_power_frame(void);
static void low_power_scene(uint32_t scene_class);

static uint32_t present_wrapped(gpu_texture back_buffer)
{
	static double last_returned;
	uint32_t next_frame_due;
	double called = uptime_seconds();

	/* (the frame's work ends here: a background hold mustn't count against its deadline) */
	if (metal_backend)
		gpu_metal_frame_deadline_end();
	host_lifecycle_hold();
	host_perf_hooks_context(1, metal_backend ? 1 : 2);
	host_perf_hooks_present_begin();
	next_frame_due = wrapped_backend->present(back_buffer);
	host_perf_hooks_present_end();
	data_updates_presented();
	/* (between frames: Low Power Mode's frame rate for the next one) */
	low_power_frame();
	if (!metal_backend)
	{
		double returned = uptime_seconds();

		double simulation, render, translation;
		int ticks;

		host_frame_cpu_split(&simulation, &render, &translation, &ticks);
		if (last_returned > 0.0)
			host_benchmark_frame(returned - last_returned, called - last_returned, -1.0, 0.0, simulation, render,
				translation, ticks);
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
	host_perf_hooks_context(1, metal_backend ? 1 : 2);
	host_perf_hooks_present_begin();
	next_frame_due = wrapped_backend->present_stereo(present);
	host_perf_hooks_present_end();
	data_updates_presented();
	low_power_frame();
	if (frame_counter)
		host_frame_counter_show(presented++);
	return next_frame_due;
}

/* the scene class's frame rate is the backend's to hold; Low Power Mode's cap follows the class at once (menus keep
the display's pace) */
static void frame_context_wrapped(uint32_t scene_class, uint32_t frame_rate, const char *level)
{
	wrapped_backend->frame_context(scene_class, frame_rate, level);
	low_power_scene(scene_class);
}

/* display.frame_rate (GPU_INITIALIZE_FRAME_RATE_SHIFT). In the Mac app it
is any cap or "unlimited". On an iPhone, an iPad and an Apple TV it is a cap
under the display's highest rate (a profile's "60" on a 120 Hz screen): the
display link asks the system for that rate (gpu_metal.m, pacing_start), and
frames are spaced and judged for it (host_frame_rate_target); "unlimited"
and a cap at or above the display's rate leave the display's own pace. A
Vision Pro's Compositor paces frames itself, and there the setting is
ignored with a line in the log. The Mac app holds a cap by sleeping before a
frame is shown (host_frame_rate_limit: gpu_metal.m's Present, host_gpu.c's swap);
going faster than the display is the Mac app's Metal backend's to arrange
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

__attribute__((unused)) static void frame_rate_cap_set(uint32_t rate)
{
	mach_timebase_info_data_t timebase;

	mach_timebase_info(&timebase);
	frame_rate.cap = (double)rate;
	frame_rate.interval = (uint64_t)(1e9 / (double)rate * (double)timebase.denom / (double)timebase.numer);
}

static uint32_t frame_rate_requested;
static void low_power_update(int display_changed);

static void frame_rate_initialize(uint32_t flags)
{
	uint32_t rate = flags >> GPU_INITIALIZE_FRAME_RATE_SHIFT;
    frame_rate_requested = rate;

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
		frame_rate_cap_set(rate);
		platform_log("display.frame_rate: at most %u frames a second%s", (unsigned)rate, (flags & GPU_INITIALIZE_METAL) ? "" :
			" (OpenGL ES also waits for the display)");
	}
#elif TARGET_OS_VISION
	platform_log("display.frame_rate is not for a Vision Pro, where the system paces frames; the setting is ignored");
#else
	{
		struct host_external_snapshot output;

		host_external_snapshot(&output);
		if (rate == GPU_INITIALIZE_FRAME_RATE_UNLIMITED)
			platform_log("display.frame_rate: \"unlimited\" is for the Mac app; frames keep the display's pace");
		else if (output.refresh_hertz > 0 && rate >= (uint32_t)output.refresh_hertz)
			platform_log("display.frame_rate: %u is not under the display's %d a second; frames keep the display's pace",
				(unsigned)rate, output.refresh_hertz);
		else
		{
			frame_rate_cap_set(rate);
			if (output.refresh_hertz > 0)
				platform_log("display.frame_rate: at most %u frames a second (the display's highest: %d)", (unsigned)rate,
					output.refresh_hertz);
			else
				platform_log("display.frame_rate: at most %u frames a second", (unsigned)rate);
		}
	}
#endif
}

/* Safe points run on the drawing thread, as do the pacing reads and benchmark overrides. */
void host_frame_rate_profile(int rate)
{
    if (rate < -1 || rate > 1000) return;
    uint32_t requested = rate == -1 ? GPU_INITIALIZE_FRAME_RATE_UNLIMITED : (uint32_t)rate;
    if (requested == frame_rate_requested) return;
    memset(&frame_rate, 0, sizeof(frame_rate));
    frame_rate_initialize((requested << GPU_INITIALIZE_FRAME_RATE_SHIFT) |
        (metal_backend ? GPU_INITIALIZE_METAL : 0));
    low_power_update(0);
}

/* what the configuration asked for, which a benchmark case's override (host_frame_rate_override) returns to */
static struct
{
	int unlimited;
	double cap;
	uint64_t interval;
} frame_rate_configured;

static void frame_rate_override(double rate)
{
	static int overridden;

	if (!overridden)
	{
		frame_rate_configured.unlimited = frame_rate.unlimited;
		frame_rate_configured.cap = frame_rate.cap;
		frame_rate_configured.interval = frame_rate.interval;
	}
	if (rate <= 0.0)
	{
		if (!overridden)
			return;
		overridden = 0;
		frame_rate.unlimited = frame_rate_configured.unlimited;
		frame_rate.cap = frame_rate_configured.cap;
		frame_rate.interval = frame_rate_configured.interval;
		platform_log("display.frame_rate: back to the configuration's (%.0f; 0 for the display's rate)", frame_rate.cap);
		return;
	}
#if TARGET_OS_VISION
	platform_log("display.frame_rate %.0f: not for a Vision Pro; ignored", rate);
	return;
#else
#if !TARGET_OS_MACCATALYST && !HALO_NATIVE_MAC
	{
		struct host_external_snapshot output;

		host_external_snapshot(&output);
		if (output.refresh_hertz > 0 && rate >= (double)output.refresh_hertz)
		{
			/* (not under the display's rate: the display's own pace, as with no cap) */
			overridden = 1;
			frame_rate.unlimited = 0;
			frame_rate.cap = 0.0;
			frame_rate.interval = 0;
			platform_log("display.frame_rate %.0f for this run: not under the display's %d; the display's pace", rate,
				output.refresh_hertz);
			return;
		}
	}
#endif
	overridden = 1;
	frame_rate.unlimited = 0;
	frame_rate_cap_set((uint32_t)rate);
	platform_log("display.frame_rate: at most %.0f frames a second for this run", rate);
#endif
}

void host_frame_rate_override(double rate)
{
	frame_rate_override(rate);
	low_power_update(0);
}

/* ---------- Low Power Mode (low_power.h)

The rate is latched between frames on the game's thread (after each Present, and as a frame's scene class is
chosen, before its targets are), and the cap goes to the pacing thread's display link through low_power_cap_now. */
#if !TARGET_OS_VISION
static struct
{
	int ready;
	struct low_power state;
	int scene_class;         /* the frame's (port_config.h: 0 gameplay, 1 menu, 2 cutscene) */
	double display;          /* host_low_power_display_rate's, read again on a notice or a change; while Low Power
	                            Mode is on, the highest seen for this output (a screen may report its lowered rate) */
	unsigned output;         /* host_low_power_display_rate's output then */
	unsigned long notices;   /* host_low_power_notices' when display was read */
	unsigned long frames;
	uint64_t interval;       /* the Mac app's cap's (host_frame_rate_limit), mach_absolute_time's units; 0 none */
} low_power = { .scene_class = 1 };
/* frames a second, 0 none: read on any thread */
static _Atomic uint32_t low_power_cap_now;
#endif

static double scene_frame_rate;

/* the configured rate: display.frame_rate's cap together with the scene class's rate, 0 for the display's */
static double frame_rate_configured_target(void)
{
	return scene_frame_rate > 0.0 && (frame_rate.cap <= 0.0 || scene_frame_rate < frame_rate.cap) ? scene_frame_rate :
		frame_rate.cap;
}

static void low_power_install(void)
{
#if !TARGET_OS_VISION
	char value[16];

	if (low_power.ready)
		return;
	host_config_string("debug.low_power_frame_rate", "true", value, sizeof(value));
	low_power.state.override = low_power_override_setting(value);
	if (low_power.state.override == LOW_POWER_IGNORE)
		platform_log("Low Power Mode: frames keep their rate in it (debug.low_power_frame_rate = false)");
	else if (low_power.state.override == LOW_POWER_ALWAYS)
		platform_log("Low Power Mode: taken as on (debug.low_power_frame_rate = always)");
	host_low_power_install();
	low_power.ready = 1;
	low_power_update(1);
#endif
}

/* display_changed: read the display's rate again */
static void low_power_update(int display_changed)
{
#if !TARGET_OS_VISION
	double configured, target;
	int change;

	if (!low_power.ready)
		return;
	if (display_changed || host_low_power_notices() != low_power.notices)
	{
		unsigned output;
		double display = host_low_power_display_rate(&output);

		low_power.notices = host_low_power_notices();
		/* (the halving is of the display's own highest rate: in Low Power Mode a ProMotion screen may say 60) */
		if (output != low_power.output || !host_low_power_enabled() || display > low_power.display)
			low_power.display = display;
		low_power.output = output;
	}
	configured = frame_rate.unlimited ? 0.0 : frame_rate_configured_target();
	change = low_power_latch(&low_power.state, host_low_power_enabled(), configured, low_power.display,
		host_low_power_display_limited(), low_power.scene_class == 1);
	if (change == LOW_POWER_UNCHANGED)
		return;
	atomic_store(&low_power_cap_now, (uint32_t)low_power.state.cap);
	low_power.interval = 0;
	if (low_power.state.cap > 0.0)
	{
		mach_timebase_info_data_t timebase;

		mach_timebase_info(&timebase);
		low_power.interval = (uint64_t)(1e9 / low_power.state.cap * (double)timebase.denom / (double)timebase.numer);
	}
	target = low_power_target(configured, low_power.state.cap);
	if (change == LOW_POWER_TURNED_OFF)
	{
		if (configured > 0.0)
			platform_log("Low Power Mode off: at most %.0f frames a second again", configured);
		else
			platform_log("Low Power Mode off: frames keep the display's pace again%s",
				frame_rate.unlimited ? " (display.frame_rate: unlimited)" : "");
	}
	else if (target > 0.0)
		platform_log("Low Power Mode%s: at most %.0f frames a second%s (the display's highest: %.0f)",
			change == LOW_POWER_TURNED_ON ? " on" : "", target,
			low_power.scene_class == 1 ? " in the menus" : low_power.scene_class == 2 ? " in cutscenes" : "",
			low_power_display_rate(low_power.display, host_low_power_display_limited()));
	else
		platform_log("Low Power Mode%s: frames keep the display's pace%s", change == LOW_POWER_TURNED_ON ? " on" : "",
			low_power.scene_class == 1 ? " in the menus" : "");
#else
	(void)display_changed;
#endif
}

static void low_power_frame(void)
{
#if !TARGET_OS_VISION
	double target;

	/* (every 600 frames the display's rate again: an external display may have come or gone) */
	low_power_update(++low_power.frames % 600 == 0);
	target = host_frame_rate_effective();
	host_perf_hooks_context(10, (uint64_t)low_power.state.on);
	host_perf_hooks_context(11, target > 0.0 ? (uint64_t)(target + 0.5) : 0);
#endif
}

static void low_power_scene(uint32_t scene_class)
{
#if !TARGET_OS_VISION
	if ((int)scene_class == low_power.scene_class)
		return;
	low_power.scene_class = (int)scene_class;
	low_power_update(1);
#else
	(void)scene_class;
#endif
}

double host_frame_rate_effective(void)
{
	double target = host_frame_rate_target();

#if !TARGET_OS_VISION
	if (target <= 0.0)
		target = low_power.state.on ? low_power_display_rate(low_power.display, host_low_power_display_limited()) :
			low_power.display;
#endif
	return target;
}

int host_frame_rate_low_power(void)
{
#if !TARGET_OS_VISION
	return low_power.state.on;
#else
	return 0;
#endif
}

/* Low Power Mode's cap, 0 for none (any thread) */
static double low_power_cap_any_thread(void)
{
#if !TARGET_OS_VISION
	return (double)atomic_load(&low_power_cap_now);
#else
	return 0.0;
#endif
}

int host_frame_rate_unlimited(void)
{
	return frame_rate.unlimited && low_power_cap_any_thread() <= 0.0;
}

double host_frame_rate_cap(void)
{
	return low_power_target(frame_rate.cap, low_power_cap_any_thread());
}

/* (set and read on the game's thread, which draws and presents) */
void host_frame_rate_scene(double rate)
{
	scene_frame_rate = rate;
}

double host_frame_rate_target(void)
{
	return low_power_target(frame_rate_configured_target(), low_power_cap_any_thread());
}

double host_frame_rate_limit(void)
{
	uint64_t now, interval;

#if !TARGET_OS_MACCATALYST && !HALO_NATIVE_MAC
	/* (an iPhone, iPad or Apple TV holds its cap at the display: the display link
	runs at it, and under 60 the pacer's frame spacing holds each frame for its
	refreshes, as for a scene class's rate. A sleep here as well drifts against
	the refreshes and makes frames miss one) */
	return 0.0;
#endif
	interval = frame_rate.interval;
#if !TARGET_OS_VISION
	/* (Low Power Mode's cap where it is lower) */
	if (low_power.interval && (!interval || low_power.interval > interval))
		interval = low_power.interval;
#endif
	if (!interval)
		return 0.0;
	now = mach_absolute_time();
	if (frame_rate.next > now)
	{
		mach_timebase_info_data_t timebase;
		uint64_t wait = frame_rate.next - now;

		mach_wait_until(frame_rate.next);
		frame_rate.next += interval;
		mach_timebase_info(&timebase);
		return (double)wait * (double)timebase.numer / (double)timebase.denom / 1e9;
	}
	/* a frame that came late doesn't earn the next ones a shorter wait */
	if (now - frame_rate.next > interval)
		frame_rate.next = now;
	frame_rate.next += interval;
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
	if (backend->frame_context)
		wrapped.frame_context = frame_context_wrapped;
	backend = &wrapped;
	host_frame_counter_stop();
	presented = 0;
	frame_counter = (flags & GPU_INITIALIZE_FRAME_COUNTER) != 0;
	if (frame_counter)
		host_frame_counter_start((flags & GPU_INITIALIZE_FIXED_TIMESTEP) != 0, (flags & GPU_INITIALIZE_METAL) != 0);
	host_lifecycle_install();
	frame_rate_initialize(flags);
	/* (before the backend starts its pacing: the display link asks for Low Power Mode's rate from the first frame) */
	low_power_install();
	low_power_update(1);
	backend->initialize(flags, capabilities);
}

#define DISPATCH_FUNCTION(type, name, parameters, arguments) type gpu_##name parameters { return backend->name arguments; }
#define DISPATCH_PROCEDURE(name, parameters, arguments) void gpu_##name parameters { backend->name arguments; }
GPU_OPERATIONS(DISPATCH_FUNCTION, DISPATCH_PROCEDURE)
