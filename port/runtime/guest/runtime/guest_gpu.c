/*
GUEST_GPU.C

The GPU backend (gpu.h) runs in the host on iOS: each gpu_* function the
Direct3D front end calls is the host_gpu_* import of the same name and
signature (guest_host.h, tools/ios_bridges.py). Including both headers makes
the compiler check each pair of prototypes against each other.
*/

#include "gpu.h"
#include "guest_host.h"

gpu_texture gpu_texture_create(const struct gpu_texture_description *description)
{
	return host_gpu_texture_create(description);
}

void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	host_gpu_texture_upload(texture, face, level, data, size);
}

void gpu_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	host_gpu_texture_copy_level(source, destination, level);
}

void gpu_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	host_gpu_texture_generate_mipmaps(texture, base_level);
}

void gpu_texture_destroy(gpu_texture texture)
{
	host_gpu_texture_destroy(texture);
}

uint32_t gpu_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	return host_gpu_texture_read(texture, pixels, size);
}

gpu_buffer gpu_buffer_create(uint32_t size)
{
	return host_gpu_buffer_create(size);
}

void gpu_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	host_gpu_buffer_write(buffer, offset, size, data, flags);
}

void gpu_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	host_gpu_stream_reserve(vertex_bytes, index_bytes);
}

uint32_t gpu_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	return host_gpu_stream(kind, data, size, buffer);
}

gpu_shader gpu_shader_create(uint32_t stage, const char *source)
{
	return host_gpu_shader_create(stage, source);
}

void gpu_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
	host_gpu_clear(clear, rectangles, count);
}

uint32_t gpu_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants, const struct gpu_uniforms *uniforms)
{
	return host_gpu_draw(draw, constants, uniforms);
}

void gpu_visibility_begin(void)
{
	host_gpu_visibility_begin();
}

void gpu_visibility_end(uint32_t slot)
{
	host_gpu_visibility_end(slot);
}

uint32_t gpu_visibility_result(uint32_t slot, uint32_t *samples)
{
	return host_gpu_visibility_result(slot, samples);
}

void gpu_flush(void)
{
	host_gpu_flush();
}

void gpu_upscale(gpu_texture source, gpu_texture destination, const struct gpu_rect *area, uint32_t anti_alias)
{
	host_gpu_upscale(source, destination, area, anti_alias);
}

uint32_t gpu_anti_alias_prepare(uint32_t method, const char *smaa_shader, const void *area_texels,
	const void *search_texels)
{
	return host_gpu_anti_alias_prepare(method, smaa_shader, area_texels, search_texels);
}

void gpu_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect)
{
	host_gpu_anti_alias(method, target, rect);
}

uint32_t gpu_vertical_blank_wait(void)
{
	return host_gpu_vertical_blank_wait();
}

void gpu_frame_start(void)
{
	host_gpu_frame_start();
}

uint32_t gpu_present(gpu_texture back_buffer)
{
	return host_gpu_present(back_buffer);
}

uint32_t gpu_present_stereo(const struct gpu_stereo_present *present)
{
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
	host_gpu_warm_begin();
}

void gpu_pipeline_warm(const struct gpu_pipeline_description *description)
{
	host_gpu_pipeline_warm(description);
}

void gpu_warm_end(void)
{
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
