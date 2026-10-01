/*
GPU.H

The interface between the Direct3D front end (d3d8_gl.c, xbox_textures.c)
and a GPU backend (gpu_gl.c; Metal in Phase 1). It is filled in one
sub-step at a time (renderer split design, step 3).

It compiles on the 64-bit iOS host as well as in the 32-bit guest, so it
includes only <stdint.h> and its structs use fixed-width fields alone: the
two ABIs lay them out the same way.
*/

#ifndef __HALO_GPU_H
#define __HALO_GPU_H

#include <stdint.h>

/* 0 is none */
typedef uint32_t gpu_texture, gpu_buffer, gpu_shader;

/* how visibility (occlusion) tests count samples */
enum
{
	/* the number of samples that passed */
	GPU_OCCLUSION_EXACT,
	/* only whether any passed (OpenGL ES occlusion queries) */
	GPU_OCCLUSION_ANY_SAMPLE,
	/* the pixel shader counts them (count_samples in the pixel shader key) */
	GPU_OCCLUSION_SHADER_COUNTER,
};

struct gpu_capabilities
{
	/* D3DCOLOR vertex attributes can be read as BGRA; otherwise the front
	end swizzles them while streaming */
	uint8_t vertex_bgra;
	/* indexed draws take a base vertex; otherwise indices are rebased */
	uint8_t base_vertex;
	uint8_t triangle_fans;
	uint8_t line_loops;
	/* samplers apply a LOD bias; otherwise the shader does */
	uint8_t sampler_lod_bias;
	/* GPU_OCCLUSION_* */
	uint8_t occlusion_mode;
	/* BC1-3 textures; otherwise the front end decodes DXT */
	uint8_t s3tc;
	/* BORDER addressing; otherwise it becomes CLAMP_TO_EDGE */
	uint8_t border_clamp;
	/* the shading language version: 450, 300 or 310 */
	uint16_t shader_language;
	/* OpenGL ES shading language */
	uint8_t shader_es;
	/* clip space is emulated in the vertex shader: rows from the top, and
	depth from 0..1 to -1..1 */
	uint8_t clip_y_flip;
	uint8_t clip_z_remap;
	uint8_t pad[3];
	uint32_t max_texture_size;
};

/* ---------- textures, render targets included */

/* gpu_texture_description.type */
enum { GPU_TEXTURE_2D = 1, GPU_TEXTURE_3D, GPU_TEXTURE_CUBE };

/* gpu_texture_description.format: BGRA8 texels are 32-bit ARGB words in
memory; BC1-3 are DXT1, 3 and 5 */
enum
{
	GPU_FORMAT_BGRA8 = 1,
	GPU_FORMAT_BC1,
	GPU_FORMAT_BC2,
	GPU_FORMAT_BC3,
	GPU_FORMAT_DEPTH_STENCIL,
};

/* gpu_texture_description.usage */
enum
{
	/* filled by gpu_texture_upload */
	GPU_USAGE_UPLOAD = 1,
	/* drawn into, or filled from render targets (mip composites) */
	GPU_USAGE_RENDER_TARGET,
};

struct gpu_texture_description
{
	uint8_t type;
	uint8_t format;
	uint8_t usage;
	uint8_t pad;
	uint32_t width, height, depth, levels;
};

gpu_texture gpu_texture_create(const struct gpu_texture_description *description);
/* one face (0 unless a cube) and level; a refresh uploads every face and
level in order, starting from face 0, level 0 */
void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size);
/* level 0 of source into level of destination (mip composites) */
void gpu_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level);
/* the levels after base_level from base_level */
void gpu_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level);
void gpu_texture_destroy(gpu_texture texture);

/* probe the context, which must be current, and set it up */
void gpu_initialize(struct gpu_capabilities *capabilities);

#endif
