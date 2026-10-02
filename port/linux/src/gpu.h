/*
GPU.H

The interface between the Direct3D front end (d3d8_device.c, xbox_textures.c)
and a GPU backend (gpu_gl.c; Metal in Phase 1). It is filled in one
sub-step at a time (renderer split design, step 3).

It compiles on the 64-bit iOS host as well as in the 32-bit guest, so it
includes only <stdint.h> and gpu_uniforms.h (which includes nothing), and its
structs use fixed-width fields alone: the two ABIs lay them out the same way.
*/

#ifndef __HALO_GPU_H
#define __HALO_GPU_H

#include <stdint.h>
#include "gpu_uniforms.h"

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
/* level 0 of a 2D color texture as BGRA8 rows from the top; returns 0 and
writes nothing if size is short of width * height * 4 or the backend cannot
read the texture (ES: block-compressed textures) */
uint32_t gpu_texture_read(gpu_texture texture, void *pixels, uint32_t size);

/* ---------- buffers: the vertex mirror's segments */

/* gpu_buffer_write flags: no queued draw reads the range (pages uploaded for
the first time), so the backend needn't wait for the GPU */
enum { GPU_WRITE_UNUSED = 1 };

gpu_buffer gpu_buffer_create(uint32_t size);
void gpu_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags);

/* ---------- per-frame transient data */

/* gpu_stream kinds */
enum { GPU_STREAM_KIND_VERTEX = 1, GPU_STREAM_KIND_INDEX };

/* room for what one draw streams, before its first gpu_stream: starting a
new buffer between two of a draw's streams would leave the earlier ones
pointing at discarded storage */
void gpu_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes);
/* copies data into the frame's stream or index buffer; returns its offset
and sets *buffer */
uint32_t gpu_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer);

/* ---------- shaders */

enum { GPU_SHADER_VERTEX = 1, GPU_SHADER_PIXEL };

/* source is in the dialect gpu_capabilities.shader_language names; 0 if it
doesn't compile */
gpu_shader gpu_shader_create(uint32_t stage, const char *source);

/* ---------- vertex constants

The front end's store of the 192 vertex constant registers. Each register's
serial is the value serial took when it last changed, and log holds the
index of the register that changed at each serial, modulo its size: a
backend that saw serial s can upload just what changed since. */

enum { GPU_CONSTANT_COUNT = 192, GPU_CONSTANT_LOG_SIZE = 1024 };

struct gpu_constant_store
{
	float c[GPU_CONSTANT_COUNT][4];
	uint32_t serials[GPU_CONSTANT_COUNT];
	uint32_t serial;
	uint8_t log[GPU_CONSTANT_LOG_SIZE];
};

/* ---------- uniforms (gpu_uniforms.h): what the shaders read besides the
vertex constants, and serial, which changes whenever any of them does */

struct gpu_uniforms
{
	GPU_UNIFORMS(GPU_UNIFORM_FIELD)
	uint32_t serial;
	uint32_t pad[3];
};

/* ---------- draw state (the spec's draw packet, filled in sub-step e) */

/* compare functions: depth and stencil tests */
enum
{
	GPU_COMPARE_NEVER, GPU_COMPARE_LESS, GPU_COMPARE_EQUAL, GPU_COMPARE_LESS_EQUAL,
	GPU_COMPARE_GREATER, GPU_COMPARE_NOT_EQUAL, GPU_COMPARE_GREATER_EQUAL, GPU_COMPARE_ALWAYS,
};

enum
{
	GPU_STENCIL_KEEP, GPU_STENCIL_ZERO, GPU_STENCIL_REPLACE, GPU_STENCIL_INCREMENT_CLAMP,
	GPU_STENCIL_DECREMENT_CLAMP, GPU_STENCIL_INVERT, GPU_STENCIL_INCREMENT_WRAP, GPU_STENCIL_DECREMENT_WRAP,
};

enum
{
	GPU_BLEND_ZERO, GPU_BLEND_ONE,
	GPU_BLEND_SOURCE_COLOR, GPU_BLEND_ONE_MINUS_SOURCE_COLOR,
	GPU_BLEND_SOURCE_ALPHA, GPU_BLEND_ONE_MINUS_SOURCE_ALPHA,
	GPU_BLEND_DESTINATION_ALPHA, GPU_BLEND_ONE_MINUS_DESTINATION_ALPHA,
	GPU_BLEND_DESTINATION_COLOR, GPU_BLEND_ONE_MINUS_DESTINATION_COLOR,
	GPU_BLEND_SOURCE_ALPHA_SATURATE,
	GPU_BLEND_CONSTANT_COLOR, GPU_BLEND_ONE_MINUS_CONSTANT_COLOR,
	GPU_BLEND_CONSTANT_ALPHA, GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA,
};

enum { GPU_BLEND_OP_ADD, GPU_BLEND_OP_SUBTRACT, GPU_BLEND_OP_REVERSE_SUBTRACT, GPU_BLEND_OP_MIN, GPU_BLEND_OP_MAX };

/* which faces to discard */
enum { GPU_CULL_NONE, GPU_CULL_FRONT, GPU_CULL_BACK };
enum { GPU_FRONT_CLOCKWISE, GPU_FRONT_COUNTER_CLOCKWISE };
enum { GPU_FILL_SOLID, GPU_FILL_LINE, GPU_FILL_POINT };

/* in target pixels */
struct gpu_viewport
{
	int32_t x, y, width, height;
	float min_z, max_z;
};

/* in target pixels; width or height 0: the scissor test is off */
struct gpu_rect
{
	int32_t x, y, width, height;
};

struct gpu_depth_stencil_state
{
	uint8_t depth_test;
	uint8_t depth_write;
	uint8_t depth_function;
	uint8_t stencil_test;
	uint8_t stencil_function;
	uint8_t stencil_fail, stencil_depth_fail, stencil_pass;
	uint32_t stencil_reference, stencil_read_mask, stencil_write_mask;
};

struct gpu_blend_state
{
	uint8_t enable;
	uint8_t source, destination, operation;
	/* ARGB, as D3DCOLOR */
	uint32_t color;
	/* bit 0 red, 1 green, 2 blue, 3 alpha */
	uint8_t color_write_mask;
	uint8_t pad[3];
};

struct gpu_raster_state
{
	uint8_t cull_mode;
	uint8_t front_face;
	uint8_t fill_mode;
	uint8_t depth_bias_enable;
	float depth_bias_slope;
	float depth_bias_constant;
};

/* texture filters (D3DTSS_MINFILTER, MAGFILTER, MIPFILTER); the GL backend
treats every one but POINT as linear, and ANISOTROPIC also enables
anisotropy. They keep D3D's distinctions so that a change between two of
them is still a change of state (the backend's sampler cache) */
enum
{
	GPU_FILTER_NONE, GPU_FILTER_POINT, GPU_FILTER_LINEAR, GPU_FILTER_ANISOTROPIC,
	GPU_FILTER_QUINCUNX, GPU_FILTER_GAUSSIAN_CUBIC,
};

/* texture addressing (D3DTSS_ADDRESSU, V, W); CLAMP and CLAMP_TO_EDGE sample
alike but stay distinct, as the filters do */
enum { GPU_ADDRESS_WRAP, GPU_ADDRESS_MIRROR, GPU_ADDRESS_CLAMP, GPU_ADDRESS_BORDER, GPU_ADDRESS_CLAMP_TO_EDGE };

struct gpu_sampler_state
{
	uint8_t min_filter, mag_filter, mip_filter;
	uint8_t address_u, address_v, address_w;
	uint8_t pad[2];
	/* the D3D values: the first level sampled, and the anisotropy an
	ANISOTROPIC min filter uses */
	uint32_t max_mip_level;
	uint32_t max_anisotropy;
	/* the D3D bias; the shader applies it without sampler_lod_bias */
	float lod_bias;
	/* ARGB, as D3DCOLOR; BORDER addressing without border_clamp becomes
	CLAMP_TO_EDGE in the backend */
	uint32_t border_color;
};

struct gpu_stage
{
	gpu_texture texture;
	/* 0: the stage is off; else GPU_TEXTURE_* */
	uint8_t type;
	uint8_t pad[3];
	struct gpu_sampler_state sampler;
};

/* vertex attribute formats: the GL backend reads UBYTE and NORMSHORT
normalized, SHORT as plain integers converted to float, and NORMPACKED3 as
one unsigned integer the shader unpacks; BGRA8 only with vertex_bgra */
enum
{
	GPU_ATTRIBUTE_FLOAT1 = 1, GPU_ATTRIBUTE_FLOAT2, GPU_ATTRIBUTE_FLOAT3, GPU_ATTRIBUTE_FLOAT4,
	GPU_ATTRIBUTE_BGRA8, GPU_ATTRIBUTE_RGBA8,
	GPU_ATTRIBUTE_SHORT1, GPU_ATTRIBUTE_SHORT2, GPU_ATTRIBUTE_SHORT3, GPU_ATTRIBUTE_SHORT4,
	GPU_ATTRIBUTE_NORMSHORT1, GPU_ATTRIBUTE_NORMSHORT2, GPU_ATTRIBUTE_NORMSHORT3, GPU_ATTRIBUTE_NORMSHORT4,
	GPU_ATTRIBUTE_UBYTE1, GPU_ATTRIBUTE_UBYTE2, GPU_ATTRIBUTE_UBYTE3, GPU_ATTRIBUTE_UBYTE4,
	GPU_ATTRIBUTE_NORMPACKED3,
};

/* an attribute's source besides streams 0-15: a constant value, or nothing */
enum { GPU_STREAM_CONSTANT = 16, GPU_STREAM_NONE = 255 };

struct gpu_vertex_stream
{
	gpu_buffer buffer;
	uint32_t offset;
	uint32_t stride;
};

struct gpu_vertex_attribute
{
	uint8_t format;
	uint8_t stream;
	/* bytes from the start of the stream's vertex */
	uint16_t offset;
};

/* primitives; TRIANGLE_FAN and LINE_LOOP only with triangle_fans and line_loops */
enum
{
	GPU_PRIMITIVE_POINTS, GPU_PRIMITIVE_LINES, GPU_PRIMITIVE_LINE_LOOP, GPU_PRIMITIVE_LINE_STRIP,
	GPU_PRIMITIVE_TRIANGLES, GPU_PRIMITIVE_TRIANGLE_STRIP, GPU_PRIMITIVE_TRIANGLE_FAN,
};

/* one draw, complete: the backend applies the whole packet without reference
to any draw before it */
struct gpu_draw
{
	gpu_texture color_target;     /* 0: depth-only draw */
	gpu_texture depth_target;     /* 0: none */
	gpu_shader vertex_shader;
	gpu_shader pixel_shader;
	struct gpu_viewport viewport;
	struct gpu_rect scissor;
	struct gpu_depth_stencil_state depth_stencil;
	struct gpu_blend_state blend;
	struct gpu_raster_state raster;
	struct gpu_stage stages[4];
	struct gpu_vertex_stream streams[16];
	struct gpu_vertex_attribute attributes[16];
	float constant_values[16][4]; /* for GPU_STREAM_CONSTANT attributes */
	gpu_buffer index_buffer;      /* 0: not indexed; 16-bit indices */
	uint32_t index_offset;
	uint32_t primitive;
	uint32_t count;
	int32_t base_vertex;
};

/* ---------- drawing */

/* gpu_clear.flags: the buffers cleared */
enum { GPU_CLEAR_COLOR = 1, GPU_CLEAR_DEPTH = 2, GPU_CLEAR_STENCIL = 4 };
/* gpu_clear.channel_mask: the color channels cleared */
enum { GPU_CHANNEL_RED = 1, GPU_CHANNEL_GREEN = 2, GPU_CHANNEL_BLUE = 4, GPU_CHANNEL_ALPHA = 8 };

struct gpu_clear
{
	gpu_texture color_target;   /* 0: none */
	gpu_texture depth_target;   /* 0: none; then no depth or stencil flags */
	uint32_t flags;
	uint32_t channel_mask;
	uint32_t color;             /* ARGB */
	float depth;
	uint32_t stencil;
};

/* clears each rectangle (in target pixels, as gpu_rect is everywhere) of the
requested buffers; depth and stencil are written whatever the draw-time write
masks are */
void gpu_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count);

/* draws; returns 0 if the draw was skipped because its program failed to link */
uint32_t gpu_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms);

/* ---------- visibility (occlusion) tests */

/* slots a test's result is kept in; slot 0 is the backend's */
enum { GPU_VISIBILITY_SLOTS = 4096 };

/* the draws until gpu_visibility_end count their samples */
void gpu_visibility_begin(void);
/* the count goes to slot (1 to GPU_VISIBILITY_SLOTS - 1) */
void gpu_visibility_end(uint32_t slot);
/* 1 and the raw count (as occlusion_mode counts) once it is known, else 0 */
uint32_t gpu_visibility_result(uint32_t slot, uint32_t *samples);

/* ---------- frames */

/* submits the work queued so far */
void gpu_flush(void);
/* letterboxes the back buffer into the drawable, swaps, and starts the next
frame's transient buffers */
void gpu_present(gpu_texture back_buffer);
/* the GL calls (a backend's commands) issued since the last call, for
debug.gpu_stats */
uint32_t gpu_call_count_take(void);

/* probe the context, which must be current, and set it up */
void gpu_initialize(struct gpu_capabilities *capabilities);

#endif
