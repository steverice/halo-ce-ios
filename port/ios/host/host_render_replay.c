/*
HOST_RENDER_REPLAY.C

A capture replayed in the app, with no game (builds configured with
HALO_RENDER_VALIDATION; gpu_capture.h): started with HALO_RENDER_REPLAY naming
a capture file, the host makes the window the game would, initializes the
backend the capture was made on, plays the file into it through the same
entry points the game calls (so the validator checks every call), saves each
frame's picture as it is presented, and exits.

  HALO_RENDER_REPLAY        the capture file
  HALO_RENDER_REPLAY_OUT    the folder for the results (default render-replay, in the data folder):
                            frameNNNNN.png as debug.screenshot_every names them (-left and -right for a
                            stereo frame's eyes), frames.txt with each picture's size and hash, result.txt
  HALO_RENDER_REPLAY_SHOTS  "all" (the default), "last" (only the window's last frame) or "none"
  HALO_RENDER_REPLAY_FORCE  1: play a capture made against another gpu.h
  HALO_RENDER_REPLAY_FUZZ   SEED:EVERY: one record in EVERY is replaced by a call that is always wrong
                            (gpu_replay_fuzz.h), each written to fuzz.log before it is played; with
                            HALO_RENDER_VALIDATION=reject the validator must keep every one from the backend

The exit status: 0 when the file played to its end (and, fuzzing, every
mutation was refused), 1 when it was refused or malformed, 2 when a mutation
reached the backend.
*/

#include "ios_host.h"
#include "host.h"
#include "renderer_build.h"

#include <TargetConditionals.h>

#if HALO_RENDER_VALIDATION

#include "gpu_capture.h"
#include "gpu_replay_fuzz.h"
#include "gpu_validate.h"
#include "png_write.h"
#include "gpu_interface_hash.h"

#include <SDL3/SDL.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags);
uint32_t host_sdl_gl_create_context(uint32_t window);
int host_sdl_gl_make_current(uint32_t window, uint32_t context);

static struct
{
	char out[1024];
	int shots;                  /* 0 none, 1 last, 2 all */
	int metal;
	uint32_t last_frame;
	FILE *frames, *fuzz_log;
	struct gpu_replay_fuzz fuzz;
	int fuzzing;
	unsigned long failures_before, holes, refused_by_reader, mutated;
	unsigned char *pixels;
	size_t pixels_size;
} replay;

void host_render_replay(void) __attribute__((noreturn));

int host_render_replay_requested(void)
{
	const char *path = getenv("HALO_RENDER_REPLAY");

	return path && *path;
}

static void replay_log(const char *line)
{
	platform_log("%s", line);
}

static uint32_t replay_flags(void *context, uint32_t captured)
{
	uint32_t flags = captured & 0xffffu;

	(void)context;
	/* no worker compiles, no pacing, no frame counter, no frame rate cap;
	visibility answers wait for the GPU, as a deterministic run's do */
	flags &= ~(uint32_t)(GPU_INITIALIZE_ASYNC_SHADERS | GPU_INITIALIZE_FRAME_COUNTER | GPU_INITIALIZE_PACING_ANY_RATE |
		GPU_INITIALIZE_METAL);
	flags |= GPU_INITIALIZE_FIXED_TIMESTEP | GPU_INITIALIZE_PACING_OFF | (replay.metal ? GPU_INITIALIZE_METAL : 0);
	return flags;
}

static uint64_t hash_pixels(const unsigned char *pixels, size_t size)
{
	uint64_t hash = 14695981039346656037ull;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ pixels[index]) * 1099511628211ull;
	return hash;
}

static void replay_frame(void *context, uint32_t frame, const gpu_texture *pictures, const uint32_t (*sizes)[2], uint32_t count)
{
	uint32_t index;

	(void)context;
	SDL_PumpEvents();
	if (!replay.shots || replay.fuzzing)
		return;
	for (index = 0; index < count; index++)
	{
		size_t size = (size_t)sizes[index][0] * sizes[index][1] * 4;
		const char *suffix = count == 1 ? "" : index ? "-right" : "-left";

		if (!pictures[index] || !size || size > (1u << 28))
			continue;
		if (size > replay.pixels_size)
		{
			free(replay.pixels);
			replay.pixels = malloc(size);
			replay.pixels_size = replay.pixels ? size : 0;
		}
		if (!replay.pixels || !gpu_texture_read(pictures[index], replay.pixels, (uint32_t)size))
		{
			platform_log("render replay: frame %u's picture cannot be read", frame);
			continue;
		}
		if (replay.frames)
			fprintf(replay.frames, "frame%05u%s %ux%u %016llx\n", frame, suffix, sizes[index][0], sizes[index][1],
				(unsigned long long)hash_pixels(replay.pixels, size));
		if (replay.shots == 2 || frame == replay.last_frame)
		{
			char path[1200];

			/* as d3d8_device.c's screenshot_file writes a frame */
			snprintf(path, sizeof(path), "%s/frame%05u%s.png", replay.out, frame, suffix);
			png_write_bgra(path, replay.pixels, sizes[index][0], sizes[index][1], (unsigned long)sizes[index][0] * 4);
		}
	}
}

static int replay_mutate(void *context, uint32_t operation, uint32_t *payload, uint32_t size)
{
	int changed = gpu_replay_fuzz_mutate(context, operation, payload, size);

	if (changed)
	{
		replay.failures_before = gpu_validation_failures();
		/* on disk before the call is made: a crash leaves its cause as the last line */
		if (replay.fuzz_log)
		{
			fprintf(replay.fuzz_log, "mutation %u: operation %u: %s\n", replay.fuzz.mutations, operation, replay.fuzz.last);
			fflush(replay.fuzz_log);
			fsync(fileno(replay.fuzz_log));
		}
	}
	return changed;
}

static void replay_mutated(void *context, uint32_t operation, int refused)
{
	(void)context;
	replay.mutated++;
	if (refused)
		replay.refused_by_reader++;
	else if (gpu_validation_failures() == replay.failures_before)
	{
		replay.holes++;
		platform_log("render replay: fuzz: NOT REFUSED: %s (operation %u) reached the backend", replay.fuzz.last, operation);
		if (replay.fuzz_log)
			fprintf(replay.fuzz_log, "  NOT REFUSED\n");
	}
}

#define REPLAY_FUNCTION(type, name, parameters, arguments) .name = gpu_##name,
#define REPLAY_PROCEDURE(name, parameters, arguments) .name = gpu_##name,
/* gpu.h's entry points (host_gpu_dispatch.c), as the guest's imports reach them */
static const struct gpu_backend entry_points = { GPU_FUNCTIONS(REPLAY_FUNCTION, REPLAY_PROCEDURE) };

void host_render_replay(void)
{
	const char *path = getenv("HALO_RENDER_REPLAY"), *out = getenv("HALO_RENDER_REPLAY_OUT"), *shots = getenv("HALO_RENDER_REPLAY_SHOTS");
	const char *fuzz = getenv("HALO_RENDER_REPLAY_FUZZ"), *force = getenv("HALO_RENDER_REPLAY_FORCE");
	struct gpu_replay_host host = { replay_log, &replay.fuzz, replay_flags, replay_frame, NULL, NULL, GPU_INTERFACE_HASH,
		GPU_FRONT_END_HASH, force && *force == '1' };
	struct gpu_replay_result result;
	struct gpu_capture_header header;
	mach_timebase_info_data_t timebase;
	uint64_t started;
	char error[256], name[1200];
	uint32_t window;
	double seconds;
	int status;
	FILE *file;

	snprintf(replay.out, sizeof(replay.out), "%s", out && *out ? out : "render-replay");
	mkdir(replay.out, 0755);
	replay.shots = !shots || !strcmp(shots, "all") ? 2 : !strcmp(shots, "last") ? 1 : 0;
	memset(&result, 0, sizeof(result));
	status = gpu_replay_header(path, &header, error, sizeof(error));
	if (status)
	{
		snprintf(result.error, sizeof(result.error), "%s", error);
		goto report;
	}
	replay.metal = header.capabilities.shader_language == GPU_SHADER_LANGUAGE_MSL;
	replay.last_frame = header.first_frame + header.frame_count - 1;
	platform_log("render replay: %s (%s, frames %u to %u, made on %s) with %s", path, header.label, header.first_frame,
		replay.last_frame, header.guest_sha256, replay.metal ? "Metal" : "OpenGL ES");
	{
		const char *renderer_error = host_render_replay_rejection(replay.metal, TARGET_OS_VISION);
		if (renderer_error)
		{
			snprintf(result.error, sizeof(result.error), "%s", renderer_error);
			status = -1;
			goto report;
		}
	}
	/* the window, and for OpenGL ES the context, as sdl_platform.c makes them */
	if (replay.metal)
	{
		window = host_sdl_create_window("Halo", 640, 480, SDL_WINDOW_METAL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	}
	else
	{
		uint32_t context;

		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
		window = host_sdl_create_window("Halo", 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		context = window ? host_sdl_gl_create_context(window) : 0;
		if (!context || !host_sdl_gl_make_current(window, context))
		{
			snprintf(result.error, sizeof(result.error), "cannot create an OpenGL ES context: %s", SDL_GetError());
			status = -1;
			goto report;
		}
		SDL_GL_SetSwapInterval(0);
	}
	if (!host_sdl_window())
	{
		snprintf(result.error, sizeof(result.error), "cannot create the window: %s", SDL_GetError());
		status = -1;
		goto report;
	}
	snprintf(name, sizeof(name), "%s/frames.txt", replay.out);
	replay.frames = fopen(name, "w");
	if (fuzz && *fuzz)
	{
		unsigned long seed = 1, every = 8;

		sscanf(fuzz, "%lu:%lu", &seed, &every);
		replay.fuzzing = 1;
		replay.fuzz.state = (uint64_t)seed * 2654435761u + 1;
		replay.fuzz.every = (uint32_t)(every ? every : 1);
		host.mutate = replay_mutate;
		host.mutated = replay_mutated;
		snprintf(name, sizeof(name), "%s/fuzz.log", replay.out);
		replay.fuzz_log = fopen(name, "w");
		platform_log("render replay: fuzzing, seed %lu, one record in %lu", seed, every);
	}
	started = mach_absolute_time();
	status = gpu_replay_run(path, &entry_points, &host, &result);
	gpu_flush();
	mach_timebase_info(&timebase);
	seconds = (double)(mach_absolute_time() - started) * (double)timebase.numer / (double)timebase.denom / 1e9;
	platform_log("render replay: %s: %u frames, %u records, %u draws (%u skipped by the backend) in %.2f s%s%s", status ? "FAILED" :
		"done", result.frames, result.records, result.draws, result.draws_skipped, seconds, status ? ": " : "", result.error);
	if (replay.fuzzing)
		platform_log("render replay: fuzz: %lu mutations, %lu refused by the reader, %lu refused by the validator, %lu NOT refused",
			replay.mutated, replay.refused_by_reader, replay.mutated - replay.refused_by_reader - replay.holes, replay.holes);
	if (replay.frames)
		fclose(replay.frames);
	if (replay.fuzz_log)
	{
		fprintf(replay.fuzz_log, "finished: %lu mutations, %lu not refused\n", replay.mutated, replay.holes);
		fclose(replay.fuzz_log);
	}
report:
	if (status)
		platform_log("render replay: FAILED: %s", result.error);
	snprintf(name, sizeof(name), "%s/result.txt", replay.out);
	file = fopen(name, "w");
	if (file)
	{
		fprintf(file, "status %d\nerror %s\nframes %u\nrecords %u\ndraws %u\ndraws_skipped %u\nstale_front_end %d\n"
			"validation_failures %lu\nvalidation_rejected %lu\nfuzz_mutations %lu\nfuzz_not_refused %lu\n", status, result.error,
			result.frames, result.records, result.draws, result.draws_skipped, result.stale_front_end, gpu_validation_failures(),
			gpu_validation_rejected(), replay.mutated, replay.holes);
		fclose(file);
	}
	gpu_validation_summary();
	host_exit(status ? 1 : replay.holes ? 2 : 0);
}

#endif
