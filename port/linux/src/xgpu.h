/*
XGPU.H

Internals shared by the OpenGL implementation of the Xbox Direct3D API:
the NV2A shader translators (nv2a_vsh.c, nv2a_psh.c), texture decoding
(xbox_textures.c), guest memory write tracking (memory_watch.c) and the
device itself (d3d8_gl.c).
*/

#ifndef __HALO_LINUX_XGPU_H
#define __HALO_LINUX_XGPU_H

#include "platform.h"
#include "gl.h"
#include "gpu.h"

#ifdef HALO_ILP32
/* OpenGL ES features that are optional (d3d8_gl.c gl_initialize) */
struct xgpu_capabilities
{
	BOOL copy_image;
	BOOL border_clamp;
	BOOL anisotropy;
	BOOL s3tc;
	/* ES 3.2: glDrawElementsBaseVertex */
	BOOL base_vertex;
	/* ES 3.1 with fragment atomic counters: exact visibility test counts */
	BOOL atomic_counters;
	/* "300 es" or "310 es" */
	const char *shading_language;
};

extern struct xgpu_capabilities xgpu_capabilities;

/* port/runtime/guest/runtime/guest_host.h */
int host_gl_has_extension(const char *name);
unsigned int host_gl_read_buffer_word(unsigned int buffer, unsigned int offset);
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data);
void host_gl_fence_frame(unsigned int slot);
void host_gl_wait_frame(unsigned int slot);
#endif

/* what the GPU backend can do (gpu_initialize, d3d8_gl.c) */
extern struct gpu_capabilities device_capabilities;

/* GL backend internals the front end still uses until step 3e moves the draw
state (gpu_gl.c) */
GLenum gpu_gl_texture_target(uint32_t type);
GLuint gpu_gl_framebuffer_get(GLuint color, GLuint depth);
/* until sub-step e's draw packet: the front end binds through the GL state
cache (gpu_gl.c) */
void gpu_gl_state_framebuffer(GLuint framebuffer);
void gpu_gl_state_element_array_buffer(GLuint buffer);
void gpu_gl_state_attribute_pointer(GLuint index, GLuint buffer, GLint size, GLenum type, GLboolean normalized,
	BOOL integer, GLsizei stride, unsigned long offset);
void gpu_gl_state_attribute_value(GLuint index, const float *value);
/* until sub-step e-4's gpu_draw: the front end applies the packet's raster
state and texture stages (gpu_gl.c) */
void gpu_gl_apply_raster_state(const struct gpu_viewport *viewport, const struct gpu_rect *scissor,
	const struct gpu_depth_stencil_state *depth_stencil, const struct gpu_blend_state *blend,
	const struct gpu_raster_state *raster);
void gpu_gl_apply_stage(int stage, const struct gpu_stage *packet_stage);
/* until step 3e's gpu_draw: the front end drives a program (gpu_gl.c) */
struct gpu_gl_program;
struct gpu_gl_program *gpu_gl_program_get(gpu_shader vertex, gpu_shader pixel);
void gpu_gl_program_use(struct gpu_gl_program *program);
void gpu_gl_program_constants(struct gpu_gl_program *program, const struct gpu_constant_store *constants);
void gpu_gl_program_uniforms(struct gpu_gl_program *program, const struct gpu_uniforms *uniforms);
/* until step 3f's gpu_present: advances the stream buffers after a frame (gpu_gl.c) */
void gpu_gl_stream_frame(void);

/* ---------- GL state

The backend caches the GL state it sets for draws (gpu_gl.c); code that
changes GL state behind it (binding a texture to upload it, deleting one)
must call this afterwards. */

void xgpu_gl_state_invalidate(void);

/* ---------- generated source text */

struct xgpu_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

void xgpu_text_append(struct xgpu_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* ---------- shader dialects

What the translators (nv2a_vsh.c, nv2a_psh.c) emit, filled from the
context's capabilities (d3d8_gl.c). GLSL only; Metal Shading Language joins
in Phase 1. */

struct nv2a_dialect
{
	/* OpenGL ES: precision statements and an "es" #version */
	unsigned char es;
	/* the #version number: 450, 300 or 310 */
	unsigned short version;
	/* emulate glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE): rows from the top
	(which also flips the winding, d3d8_gl.c), depth from 0..1 to -1..1 */
	unsigned char clip_y_flip;
	unsigned char clip_z_remap;
	/* keep the clip-space position the screen-space conversion divides, and
	undo the conversion without dividing (the mobile precision workaround,
	nv2a_vsh.c) */
	unsigned char clip_capture;
	/* samplers have no LOD bias of their own: pass texture_lod_bias to each
	lookup */
	unsigned char shader_lod_bias;
	/* debug.gpu_debug_expression, _texture0 and _flat (port_config.c) */
	const char *debug_expression;
	unsigned char debug_texture0;
	unsigned char debug_flat;
};

/* the uniform declarations of one stage (GPU_UNIFORM_VERTEX or
GPU_UNIFORM_PIXEL, gpu_uniforms.h) for a dialect */
void nv2a_uniform_declarations(struct xgpu_text *text, const struct nv2a_dialect *dialect, int stage);

/* ---------- vertex shaders */

#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
/* D3D constant register -96 is hardware register 0 */
#define XGPU_VERTEX_CONSTANT_BIAS 96

/* GLSL for an NV2A vertex program (the instruction words after the program
header). Attributes whose bit is set in packed_attribute_mask are fed as
NORMPACKED3 32-bit integers and unpacked in the shader. Returns a malloc'd
string. */
char *nv2a_vertex_shader_translate(const struct nv2a_dialect *dialect, const DWORD *instructions,
	unsigned long instruction_count, unsigned long packed_attribute_mask);

/* ---------- pixel shaders */

enum
{
	_xgpu_sampler_none = 0,
	_xgpu_sampler_2d,
	_xgpu_sampler_3d,
	_xgpu_sampler_cube,
};

/* everything a translated pixel shader depends on; the GLSL program cache
is keyed by these bytes */
struct nv2a_pixel_shader_key
{
	DWORD combiner_state[D3DRS_PS_MAX];
	/* D3DRS_PSTEXTUREMODES lies past D3DRS_PS_MAX */
	DWORD texture_modes;
	unsigned char sampler_type[4];
	unsigned char alpha_kill[4];
	/* D3DTSS_COLORSIGN: channels (bit 0 alpha ... bit 3 blue, as
	D3DTSIGN_*) that hold signed data in an unsigned texture format */
	unsigned char color_sign[4];
	/* D3DCMP_* function for the alpha test, or 0 when disabled */
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
	/* inside a visibility test: count the samples that pass (iOS) */
	unsigned char count_samples;
	unsigned char pad;
};

char *nv2a_pixel_shader_translate(const struct nv2a_dialect *dialect, const struct nv2a_pixel_shader_key *key);

/* ---------- textures */

struct xgpu_texture_description
{
	DWORD format;       /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;        /* not swizzled; addressed with texel coordinates */
	BOOL compressed;
	unsigned long pitch; /* linear textures */
};

void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
/* bytes of one face, mip levels included (cube faces are padded) */
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);

/* the texture for an Xbox texture header, uploading or refreshing it from
guest memory as needed; *type receives a GPU_TEXTURE_* */
gpu_texture xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, uint32_t *type,
	struct xgpu_texture_description *description);
void xgpu_texture_cache_begin_frame(void);

/* ---------- render targets */

struct xgpu_render_target
{
	unsigned long data;  /* physical address */
	unsigned long width, height;
	BOOL depth;
	gpu_texture texture;
	/* pixels per unit of width and height: more than 1 for the screen's
	targets when the game draws at the display's resolution (d3d8_gl.c) */
	float scale[2];
	unsigned long gl_width, gl_height;
};

/* the GL texture holding a render target with this physical address, or 0 */
struct xgpu_render_target *xgpu_render_target_find(unsigned long data);

#endif
