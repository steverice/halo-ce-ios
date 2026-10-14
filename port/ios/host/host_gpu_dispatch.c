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

#include <TargetConditionals.h>
#include <mach/mach_time.h>

/* host_gpu.c's, which writes the guest's log */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

extern const struct gpu_backend gpu_backend_metal;
/* nothing calls gpu.h before gpu_initialize (d3d8_device.c's gl_initialize) */
#if TARGET_OS_VISION
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

static uint32_t present_wrapped(gpu_texture back_buffer)
{
	uint32_t next_frame_due;

	host_lifecycle_hold();
	next_frame_due = wrapped_backend->present(back_buffer);
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
#if TARGET_OS_MACCATALYST
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

void gpu_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
#if TARGET_OS_VISION
	/* host_main.m sets HALO_RENDERER=metal, which overrides config.toml */
	if (!(flags & GPU_INITIALIZE_METAL))
		host_fatal("visionOS draws with Metal only, but display.renderer isn't \"metal\"");
	backend = &gpu_backend_metal;
#else
	backend = (flags & GPU_INITIALIZE_METAL) ? &gpu_backend_metal : &gpu_backend_gl;
#endif
	wrapped_backend = backend;
	wrapped = *backend;
	wrapped.present = present_wrapped;
	wrapped.present_stereo = present_stereo_wrapped;
	backend = &wrapped;
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
