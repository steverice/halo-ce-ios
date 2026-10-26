/*
GUEST_GPU.C

The GPU backend (gpu.h) runs in the host on iOS: each gpu_* function the
Direct3D front end calls is the host_gpu_* import of the same name and
signature (guest_host.h, tools/ios_bridges.py). Including both headers makes
the compiler check each pair of prototypes against each other.
*/

#include "gpu.h"
#include "guest_host.h"

/* port: d3d8_device.c may hold an immediate-mode draw (debug.immediate_merge); every call below first submits it, so
each finds the GPU as the game left it. (The ones that do not order GPU work are left out, some of which
another thread calls: the vertical blank thread's gpu_vertical_blank_wait.) */
#if HALO_IMMEDIATE_BATCH && !HALO_RENDER_VALIDATION
extern int halo_immediate_pending;
void halo_immediate_flush(void);
#define IMMEDIATE_FLUSH() do { if (halo_immediate_pending) halo_immediate_flush(); } while (0)
#else
#define IMMEDIATE_FLUSH() ((void)0)
#endif

/* port: the profiling build (tools/ios_build.py --profile, docs/profiling.md): each call into the host's GPU backend
is a scope of the recording, from the call to the return of the host's work. The ones made many times a frame (a draw,
an upload) are summed a frame rather than recorded each (profile_trace_name_aggregate). Call sites have no trailing
semicolon, so a normal build has none of it. */
#ifdef HALO_PROFILE
#include "../../../linux/include/profile_trace.h"
#include <pthread.h>

enum
{
	_gpu_profile_texture,
	_gpu_profile_buffer,
	_gpu_profile_shader_create,
	_gpu_profile_clear,
	_gpu_profile_draw,
	_gpu_profile_visibility,
	_gpu_profile_visibility_result,
	_gpu_profile_pipeline_warm,
	/* (the rest are scopes of their own) */
	_gpu_profile_flush,
	_gpu_profile_upscale,
	_gpu_profile_anti_alias,
	_gpu_profile_frame_start,
	_gpu_profile_present,
	_gpu_profile_warm,
	_gpu_profile_initialize,
	NUMBER_OF_GPU_PROFILE_NAMES,
	NUMBER_OF_GPU_PROFILE_AGGREGATES = _gpu_profile_flush,
};

static const char *const gpu_profile_texts[NUMBER_OF_GPU_PROFILE_NAMES] =
{
	"gpu_texture", "gpu_buffer", "gpu_shader_create", "gpu_clear", "gpu_draw", "gpu_visibility",
	"gpu_visibility_result", "gpu_pipeline_warm",
	"gpu_flush", "gpu_upscale", "gpu_anti_alias", "gpu_frame_start", "gpu_present", "gpu_warm", "gpu_initialize",
};
static int gpu_profile_ids[NUMBER_OF_GPU_PROFILE_NAMES];
/* (registered once, by whichever thread calls first: the guest's C library has no pthread_once) */
static int gpu_profile_registered;
static pthread_mutex_t gpu_profile_lock = PTHREAD_MUTEX_INITIALIZER;

static void gpu_profile_register(void)
{
	int index;

	for (index = 0; index < NUMBER_OF_GPU_PROFILE_NAMES; index++)
	{
		if (index < NUMBER_OF_GPU_PROFILE_AGGREGATES)
			profile_trace_name_aggregate(gpu_profile_texts[index]);
		gpu_profile_ids[index] = profile_trace_name(gpu_profile_texts[index]);
	}
}

static inline int gpu_profile_begin(int index)
{
	if (!__atomic_load_n(&gpu_profile_registered, __ATOMIC_ACQUIRE))
	{
		pthread_mutex_lock(&gpu_profile_lock);
		if (!gpu_profile_registered)
		{
			gpu_profile_register();
			__atomic_store_n(&gpu_profile_registered, 1, __ATOMIC_RELEASE);
		}
		pthread_mutex_unlock(&gpu_profile_lock);
	}
	profile_trace_begin(gpu_profile_ids[index]);
	return gpu_profile_ids[index];
}

static inline void gpu_profile_end(const int *id)
{
	profile_trace_end(*id);
}

#define GPU_PROFILED(name) \
	int gpu_profiled __attribute__((cleanup(gpu_profile_end), unused)) = gpu_profile_begin(_gpu_profile_##name);
#else
#define GPU_PROFILED(name)
#endif

gpu_texture gpu_texture_create(const struct gpu_texture_description *description)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(texture)
	return host_gpu_texture_create(description);
}

void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(texture)
	host_gpu_texture_upload(texture, face, level, data, size);
}

void gpu_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(texture)
	host_gpu_texture_copy_level(source, destination, level);
}

void gpu_texture_copy_rects(gpu_texture source, const struct gpu_texture_copy *copies, uint32_t count)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(texture)
	host_gpu_texture_copy_rects(source, copies, count);
}

void gpu_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(texture)
	host_gpu_texture_generate_mipmaps(texture, base_level);
}

void gpu_texture_destroy(gpu_texture texture)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(texture)
	host_gpu_texture_destroy(texture);
}

uint32_t gpu_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(texture)
	return host_gpu_texture_read(texture, pixels, size);
}

void gpu_texture_set_content(gpu_texture texture, uint32_t width, uint32_t height)
{
	IMMEDIATE_FLUSH();
	host_gpu_texture_set_content(texture, width, height);
}

gpu_buffer gpu_buffer_create(uint32_t size)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(buffer)
	return host_gpu_buffer_create(size);
}

void gpu_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(buffer)
	host_gpu_buffer_write(buffer, offset, size, data, flags);
}

void gpu_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(buffer)
	host_gpu_stream_reserve(vertex_bytes, index_bytes);
}

uint32_t gpu_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(buffer)
	return host_gpu_stream(kind, data, size, buffer);
}

gpu_shader gpu_shader_create(uint32_t stage, const char *source)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(shader_create)
	return host_gpu_shader_create(stage, source);
}

void gpu_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(clear)
	host_gpu_clear(clear, rectangles, count);
}

uint32_t gpu_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants, const struct gpu_uniforms *uniforms)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(draw)
	return host_gpu_draw(draw, constants, uniforms);
}

void gpu_visibility_begin(void)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(visibility)
	host_gpu_visibility_begin();
}

void gpu_visibility_end(uint32_t slot)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(visibility)
	host_gpu_visibility_end(slot);
}

uint32_t gpu_visibility_result(uint32_t slot, uint32_t *samples)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(visibility_result)
	return host_gpu_visibility_result(slot, samples);
}

void gpu_flush(void)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(flush)
	host_gpu_flush();
}

void gpu_upscale(gpu_texture source, gpu_texture destination, const struct gpu_rect *area, uint32_t anti_alias)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(upscale)
	host_gpu_upscale(source, destination, area, anti_alias);
}

uint32_t gpu_anti_alias_prepare(uint32_t method, const char *smaa_shader, const void *area_texels,
	const void *search_texels)
{
	GPU_PROFILED(anti_alias)
	return host_gpu_anti_alias_prepare(method, smaa_shader, area_texels, search_texels);
}

void gpu_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(anti_alias)
	host_gpu_anti_alias(method, target, rect);
}

uint32_t gpu_vertical_blank_wait(void)
{
	return host_gpu_vertical_blank_wait();
}

uint32_t gpu_companion_size(uint32_t *width, uint32_t *height)
{
	return host_gpu_companion_size(width, height);
}

void gpu_companion_set(gpu_texture target)
{
	IMMEDIATE_FLUSH();
	host_gpu_companion_set(target);
}

void gpu_frame_start(void)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(frame_start)
	host_gpu_frame_start();
}

uint32_t gpu_present(gpu_texture back_buffer)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(present)
	return host_gpu_present(back_buffer);
}

uint32_t gpu_present_stereo(const struct gpu_stereo_present *present)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(present)
	return host_gpu_present_stereo(present);
}

uint32_t gpu_call_count_take(void)
{
	return host_gpu_call_count_take();
}

uint32_t gpu_warm_list_read(const char *name, char *text, uint32_t size)
{
	return host_gpu_warm_list_read(name, text, size);
}

void gpu_warm_begin(void)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(warm)
	host_gpu_warm_begin();
}

void gpu_pipeline_warm(const struct gpu_pipeline_description *description)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(pipeline_warm)
	host_gpu_pipeline_warm(description);
}

void gpu_warm_end(void)
{
	IMMEDIATE_FLUSH();
	GPU_PROFILED(warm)
	host_gpu_warm_end();
}

uint32_t gpu_pipeline_built_take(struct gpu_pipeline_description *description)
{
	return host_gpu_pipeline_built_take(description);
}

void gpu_frame_context(uint32_t scene_class, uint32_t frame_rate, const char *level)
{
	host_gpu_frame_context(scene_class, frame_rate, level);
}

uint32_t gpu_compiles_outstanding(void)
{
	return host_gpu_compiles_outstanding();
}

void gpu_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	GPU_PROFILED(initialize)
	host_gpu_initialize(flags, capabilities);
}

#if HALO_RENDER_VALIDATION
/* render validation (gpu_validate.h): the front end's two calls */
#include "guest_host_validation.h"

void gpu_validation_identity(const struct gpu_validation_identity *identity)
{
	host_gpu_validation_identity(identity);
}

void gpu_validation_note(uint32_t reason, uint32_t detail, uint32_t value)
{
	host_gpu_validation_note(reason, detail, value);
}
#endif

void gpu_compile_progress(uint32_t action, struct gpu_compile_progress *progress)
{
	host_gpu_compile_progress(action, progress);
}
