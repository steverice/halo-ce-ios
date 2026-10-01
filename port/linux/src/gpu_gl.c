/*
GPU_GL.C

The OpenGL and OpenGL ES backend of gpu.h. Step 3 of the renderer split moves
the GL calls of d3d8_gl.c and xbox_textures.c here one group at a time;
for now it probes the context.
*/

#include "xgpu.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HALO_ILP32
/* OpenGL ES 3 has no BGRA upload format; d3d8_gl.c defines the same alias */
#define GL_BGRA GL_RGBA
#define glDepthRange glDepthRangef
/* the sampler extensions' enumerants, for ES headers without them */
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84fe
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812d
#endif
#endif

/* ---------- streams */

#ifdef HALO_ILP32
/* Mobile drivers (Mali) keep every orphaned copy of a buffer until the GPU
is done with it, so a large buffer orphaned each frame costs its size per
frame in flight and more. Instead each frame streams into the next of a few
smaller buffers, reusing one only once the GPU has finished the frame that
last used it (host_gl_wait_frame). A busy frame streams about 5 MB of
vertices. */
#define STREAM_BUFFER_SIZE (16 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (2 * 1024 * 1024)
#define STREAM_BUFFER_RING 3
#else
#define STREAM_BUFFER_SIZE (32 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (8 * 1024 * 1024)
#endif

/* the vertex array and the per-frame stream and index buffers (on ES a ring
of them, one per frame in flight; desktop GL orphans one) */
static struct
{
	GLuint vertex_array;
	GLuint stream_buffer;
#ifdef HALO_ILP32
	GLuint stream_buffers[STREAM_BUFFER_RING];
	GLuint index_buffers[STREAM_BUFFER_RING];
	unsigned long buffer_ring;
#endif
	unsigned long stream_offset;
	GLuint index_buffer;
	unsigned long index_offset;
} streams;

/* ---------- GL state cache

Consecutive draws share most of their state, but each sets all of it: the
setters here skip the call when GL already holds the value. Code that
changes GL state behind the cache's back (clears, presentation, texture
uploads, render target and framebuffer creation) calls
xgpu_gl_state_invalidate, after which every value is set again. Unknown
values are all ones, which no real value matches (floats become NaN, which
compares unequal to everything). */

struct attribute_pointer
{
	GLuint buffer;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLboolean integer;
	GLsizei stride;
	unsigned long offset;
};

static struct gpu_gl_state
{
	GLuint program;
	GLuint framebuffer;
	GLint viewport[4];
	GLint scissor[4];
	float depth_range[2];
	unsigned char depth_test, stencil_test, blend, cull_face, offset_fill, offset_line;
	unsigned char scissor_test;
	GLenum depth_function;
	unsigned char depth_mask;
	GLenum stencil_function;
	GLint stencil_reference;
	GLuint stencil_value_mask;
	GLenum stencil_operations[3];
	GLuint stencil_write_mask;
	GLenum blend_source, blend_destination, blend_equation;
	float blend_color[4];
	unsigned char color_mask;
	GLenum front_face, cull_mode, polygon_mode;
	float polygon_offset[2];
	GLenum active_texture;
	/* per unit: the GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP and GL_TEXTURE_3D
	bindings */
	GLuint textures[D3DTSS_MAXSTAGES][3];
	GLuint samplers[D3DTSS_MAXSTAGES];
	GLuint array_buffer;
	GLuint element_array_buffer;
	unsigned char attribute_enabled[XGPU_VERTEX_ATTRIBUTE_COUNT];
	struct attribute_pointer attribute_pointers[XGPU_VERTEX_ATTRIBUTE_COUNT];
	/* a disabled attribute's value; kind 1 is the integer zero */
	unsigned char attribute_value_kind[XGPU_VERTEX_ATTRIBUTE_COUNT];
	float attribute_values[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
} gl_state;

void xgpu_gl_state_invalidate(void)
{
	memset(&gl_state, 0xff, sizeof(gl_state));
}

static void state_enable(unsigned char *shadow, GLenum capability, BOOL enabled)
{
	unsigned char value = enabled ? 1 : 0;

	if (*shadow == value)
		return;
	*shadow = value;
	if (value)
		glEnable(capability);
	else
		glDisable(capability);
}

static void state_program(GLuint program)
{
	if (gl_state.program != program)
	{
		gl_state.program = program;
		glUseProgram(program);
	}
}

void gpu_gl_state_framebuffer(GLuint framebuffer)
{
	if (gl_state.framebuffer != framebuffer)
	{
		gl_state.framebuffer = framebuffer;
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	}
}

static void state_texture(int unit, GLenum target, GLuint texture)
{
	int slot = target == GL_TEXTURE_CUBE_MAP ? 1 : target == GL_TEXTURE_3D ? 2 : 0;

	if (gl_state.textures[unit][slot] == texture)
		return;
	if (gl_state.active_texture != GL_TEXTURE0 + (GLenum)unit)
	{
		gl_state.active_texture = GL_TEXTURE0 + (GLenum)unit;
		glActiveTexture(gl_state.active_texture);
	}
	gl_state.textures[unit][slot] = texture;
	glBindTexture(target, texture);
}

static void state_sampler(int unit, GLuint sampler)
{
	if (gl_state.samplers[unit] != sampler)
	{
		gl_state.samplers[unit] = sampler;
		glBindSampler((GLuint)unit, sampler);
	}
}

static void state_array_buffer(GLuint buffer)
{
	if (gl_state.array_buffer != buffer)
	{
		gl_state.array_buffer = buffer;
		glBindBuffer(GL_ARRAY_BUFFER, buffer);
	}
}

static void state_element_array_buffer(GLuint buffer)
{
	if (gl_state.element_array_buffer != buffer)
	{
		gl_state.element_array_buffer = buffer;
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
	}
}

static void state_attribute_pointer(GLuint index, GLuint buffer, GLint size, GLenum type, GLboolean normalized,
	BOOL integer, GLsizei stride, unsigned long offset)
{
	struct attribute_pointer *pointer = &gl_state.attribute_pointers[index];

	if (gl_state.attribute_enabled[index] != 1)
	{
		gl_state.attribute_enabled[index] = 1;
		glEnableVertexAttribArray(index);
	}
	if (pointer->buffer == buffer && pointer->size == size && pointer->type == type &&
		pointer->normalized == normalized && pointer->integer == (integer ? GL_TRUE : GL_FALSE) &&
		pointer->stride == stride && pointer->offset == offset)
	{
		return;
	}
	state_array_buffer(buffer);
	if (integer)
		glVertexAttribIPointer(index, size, type, stride, (const void *)offset);
	else
		glVertexAttribPointer(index, size, type, normalized, stride, (const void *)offset);
	pointer->buffer = buffer;
	pointer->size = size;
	pointer->type = type;
	pointer->normalized = normalized;
	pointer->integer = integer ? GL_TRUE : GL_FALSE;
	pointer->stride = stride;
	pointer->offset = offset;
}

/* disables the attribute, which then reads value, or the integer zero */
static void state_attribute_value(GLuint index, const float *value)
{
	unsigned char kind = value ? 0 : 1;

	if (gl_state.attribute_enabled[index] != 0)
	{
		gl_state.attribute_enabled[index] = 0;
		glDisableVertexAttribArray(index);
	}
	if (gl_state.attribute_value_kind[index] == kind &&
		(!value || !memcmp(gl_state.attribute_values[index], value, sizeof(gl_state.attribute_values[index]))))
	{
		return;
	}
	gl_state.attribute_value_kind[index] = kind;
	if (value)
	{
		memcpy(gl_state.attribute_values[index], value, sizeof(gl_state.attribute_values[index]));
		glVertexAttrib4fv(index, value);
	}
	else
	{
		glVertexAttribI4ui(index, 0, 0, 0, 0);
	}
}

/* the GL size, type and normalization of an attribute format */
static void gl_attribute_format(uint32_t format, GLint *size, GLenum *type, GLboolean *normalized)
{
	*normalized = GL_FALSE;
	switch (format)
	{
	case GPU_ATTRIBUTE_FLOAT1: *size = 1; *type = GL_FLOAT; break;
	case GPU_ATTRIBUTE_FLOAT2: *size = 2; *type = GL_FLOAT; break;
	case GPU_ATTRIBUTE_FLOAT3: *size = 3; *type = GL_FLOAT; break;
	case GPU_ATTRIBUTE_BGRA8: *size = GL_BGRA; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_RGBA8: *size = 4; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_SHORT1: *size = 1; *type = GL_SHORT; break;
	case GPU_ATTRIBUTE_SHORT2: *size = 2; *type = GL_SHORT; break;
	case GPU_ATTRIBUTE_SHORT3: *size = 3; *type = GL_SHORT; break;
	case GPU_ATTRIBUTE_SHORT4: *size = 4; *type = GL_SHORT; break;
	case GPU_ATTRIBUTE_NORMSHORT1: *size = 1; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_NORMSHORT2: *size = 2; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_NORMSHORT3: *size = 3; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_NORMSHORT4: *size = 4; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_UBYTE1: *size = 1; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_UBYTE2: *size = 2; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_UBYTE3: *size = 3; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_UBYTE4: *size = 4; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case GPU_ATTRIBUTE_NORMPACKED3: *size = 1; *type = GL_UNSIGNED_INT; break;
	case GPU_ATTRIBUTE_FLOAT4: default: *size = 4; *type = GL_FLOAT; break;
	}
}

/* stream is read when attribute->stream is 0-15, value when it is
GPU_STREAM_CONSTANT (and not for NORMPACKED3, which reads the integer zero) */
static void apply_attribute(uint32_t index, const struct gpu_vertex_attribute *attribute,
	const struct gpu_vertex_stream *stream, const float *value)
{
	GLint size;
	GLenum type;
	GLboolean normalized;

	if (attribute->stream == GPU_STREAM_CONSTANT)
	{
		/* a packed attribute reads the integer zero */
		state_attribute_value(index, attribute->format == GPU_ATTRIBUTE_NORMPACKED3 ? NULL : value);
		return;
	}
	gl_attribute_format(attribute->format, &size, &type, &normalized);
	state_attribute_pointer(index, stream->buffer, size, type, normalized,
		attribute->format == GPU_ATTRIBUTE_NORMPACKED3, (GLsizei)stream->stride,
		(unsigned long)stream->offset + attribute->offset);
}

/* ---------- raster state: the packet's viewport, scissor, depth and
stencil, blend and raster state, applied through the cache */

/* a D3DCOLOR's channels as floats, the arithmetic the front end used, so
the cache's floats compare equal */
static void color_to_vec4(uint32_t color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

static GLenum gl_compare(uint32_t function)
{
	switch (function)
	{
	case GPU_COMPARE_LESS: return GL_LESS;
	case GPU_COMPARE_EQUAL: return GL_EQUAL;
	case GPU_COMPARE_LESS_EQUAL: return GL_LEQUAL;
	case GPU_COMPARE_GREATER: return GL_GREATER;
	case GPU_COMPARE_NOT_EQUAL: return GL_NOTEQUAL;
	case GPU_COMPARE_GREATER_EQUAL: return GL_GEQUAL;
	case GPU_COMPARE_ALWAYS: return GL_ALWAYS;
	default: return GL_NEVER;
	}
}

static GLenum gl_stencil_operation(uint32_t operation)
{
	switch (operation)
	{
	case GPU_STENCIL_ZERO: return GL_ZERO;
	case GPU_STENCIL_REPLACE: return GL_REPLACE;
	case GPU_STENCIL_INCREMENT_CLAMP: return GL_INCR;
	case GPU_STENCIL_DECREMENT_CLAMP: return GL_DECR;
	case GPU_STENCIL_INVERT: return GL_INVERT;
	case GPU_STENCIL_INCREMENT_WRAP: return GL_INCR_WRAP;
	case GPU_STENCIL_DECREMENT_WRAP: return GL_DECR_WRAP;
	default: return GL_KEEP;
	}
}

static GLenum gl_blend_factor(uint32_t factor)
{
	switch (factor)
	{
	case GPU_BLEND_ONE: return GL_ONE;
	case GPU_BLEND_SOURCE_COLOR: return GL_SRC_COLOR;
	case GPU_BLEND_ONE_MINUS_SOURCE_COLOR: return GL_ONE_MINUS_SRC_COLOR;
	case GPU_BLEND_SOURCE_ALPHA: return GL_SRC_ALPHA;
	case GPU_BLEND_ONE_MINUS_SOURCE_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
	case GPU_BLEND_DESTINATION_ALPHA: return GL_DST_ALPHA;
	case GPU_BLEND_ONE_MINUS_DESTINATION_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
	case GPU_BLEND_DESTINATION_COLOR: return GL_DST_COLOR;
	case GPU_BLEND_ONE_MINUS_DESTINATION_COLOR: return GL_ONE_MINUS_DST_COLOR;
	case GPU_BLEND_SOURCE_ALPHA_SATURATE: return GL_SRC_ALPHA_SATURATE;
	case GPU_BLEND_CONSTANT_COLOR: return GL_CONSTANT_COLOR;
	case GPU_BLEND_ONE_MINUS_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
	case GPU_BLEND_CONSTANT_ALPHA: return GL_CONSTANT_ALPHA;
	case GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA: return GL_ONE_MINUS_CONSTANT_ALPHA;
	default: return GL_ZERO;
	}
}

static GLenum gl_blend_equation(uint32_t operation)
{
	switch (operation)
	{
	case GPU_BLEND_OP_SUBTRACT: return GL_FUNC_SUBTRACT;
	case GPU_BLEND_OP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
	case GPU_BLEND_OP_MIN: return GL_MIN;
	case GPU_BLEND_OP_MAX: return GL_MAX;
	default: return GL_FUNC_ADD;
	}
}

static void apply_raster_state(const struct gpu_viewport *viewport, const struct gpu_rect *scissor,
	const struct gpu_depth_stencil_state *depth_stencil, const struct gpu_blend_state *blend,
	const struct gpu_raster_state *raster)
{
	GLint viewport_box[4];
	GLint scissor_box[4];
	float depth_range[2];

	viewport_box[0] = viewport->x;
	viewport_box[1] = viewport->y;
	viewport_box[2] = viewport->width;
	viewport_box[3] = viewport->height;
	if (memcmp(gl_state.viewport, viewport_box, sizeof(viewport_box)))
	{
		memcpy(gl_state.viewport, viewport_box, sizeof(viewport_box));
		glViewport(viewport_box[0], viewport_box[1], viewport_box[2], viewport_box[3]);
	}
	/* (glScissor takes the corner and the size, as glViewport does) */
	scissor_box[0] = scissor->x;
	scissor_box[1] = scissor->y;
	scissor_box[2] = scissor->width;
	scissor_box[3] = scissor->height;
	if (memcmp(gl_state.scissor, scissor_box, sizeof(scissor_box)))
	{
		memcpy(gl_state.scissor, scissor_box, sizeof(scissor_box));
		glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
	}
	state_enable(&gl_state.scissor_test, GL_SCISSOR_TEST, scissor_box[2] > 0 && scissor_box[3] > 0);
	depth_range[0] = viewport->min_z;
	depth_range[1] = viewport->max_z;
	if (memcmp(gl_state.depth_range, depth_range, sizeof(depth_range)))
	{
		memcpy(gl_state.depth_range, depth_range, sizeof(depth_range));
		glDepthRange(depth_range[0], depth_range[1]);
	}

	state_enable(&gl_state.depth_test, GL_DEPTH_TEST, depth_stencil->depth_test);
	if (depth_stencil->depth_test)
	{
		GLenum function = gl_compare(depth_stencil->depth_function);

		if (gl_state.depth_function != function)
		{
			gl_state.depth_function = function;
			glDepthFunc(function);
		}
	}
	{
		unsigned char mask = depth_stencil->depth_write ? 1 : 0;

		if (gl_state.depth_mask != mask)
		{
			gl_state.depth_mask = mask;
			glDepthMask(mask ? GL_TRUE : GL_FALSE);
		}
	}

	state_enable(&gl_state.stencil_test, GL_STENCIL_TEST, depth_stencil->stencil_test);
	if (depth_stencil->stencil_test)
	{
		GLenum function = gl_compare(depth_stencil->stencil_function);
		GLint reference = (GLint)depth_stencil->stencil_reference;
		GLenum operations[3];

		if (gl_state.stencil_function != function || gl_state.stencil_reference != reference ||
			gl_state.stencil_value_mask != depth_stencil->stencil_read_mask)
		{
			gl_state.stencil_function = function;
			gl_state.stencil_reference = reference;
			gl_state.stencil_value_mask = depth_stencil->stencil_read_mask;
			glStencilFunc(function, reference, depth_stencil->stencil_read_mask);
		}
		operations[0] = gl_stencil_operation(depth_stencil->stencil_fail);
		operations[1] = gl_stencil_operation(depth_stencil->stencil_depth_fail);
		operations[2] = gl_stencil_operation(depth_stencil->stencil_pass);
		if (memcmp(gl_state.stencil_operations, operations, sizeof(operations)))
		{
			memcpy(gl_state.stencil_operations, operations, sizeof(operations));
			glStencilOp(operations[0], operations[1], operations[2]);
		}
		if (gl_state.stencil_write_mask != depth_stencil->stencil_write_mask)
		{
			gl_state.stencil_write_mask = depth_stencil->stencil_write_mask;
			glStencilMask(depth_stencil->stencil_write_mask);
		}
	}

	state_enable(&gl_state.blend, GL_BLEND, blend->enable);
	if (blend->enable)
	{
		GLenum source = gl_blend_factor(blend->source);
		GLenum destination = gl_blend_factor(blend->destination);
		GLenum equation = gl_blend_equation(blend->operation);
		float blend_color[4];

		if (gl_state.blend_source != source || gl_state.blend_destination != destination)
		{
			gl_state.blend_source = source;
			gl_state.blend_destination = destination;
			glBlendFunc(source, destination);
		}
		if (gl_state.blend_equation != equation)
		{
			gl_state.blend_equation = equation;
			glBlendEquation(equation);
		}
		color_to_vec4(blend->color, blend_color);
		if (memcmp(gl_state.blend_color, blend_color, sizeof(blend_color)))
		{
			memcpy(gl_state.blend_color, blend_color, sizeof(blend_color));
			glBlendColor(blend_color[0], blend_color[1], blend_color[2], blend_color[3]);
		}
	}
	if (gl_state.color_mask != blend->color_write_mask)
	{
		gl_state.color_mask = blend->color_write_mask;
		glColorMask((blend->color_write_mask & 1) != 0, (blend->color_write_mask & 2) != 0,
			(blend->color_write_mask & 4) != 0, (blend->color_write_mask & 8) != 0);
	}

	state_enable(&gl_state.cull_face, GL_CULL_FACE, raster->cull_mode != GPU_CULL_NONE);
	if (raster->cull_mode != GPU_CULL_NONE)
	{
		GLenum front_face = raster->front_face == GPU_FRONT_COUNTER_CLOCKWISE ? GL_CCW : GL_CW;
		GLenum cull_mode = raster->cull_mode == GPU_CULL_FRONT ? GL_FRONT : GL_BACK;

		if (gl_state.front_face != front_face)
		{
			gl_state.front_face = front_face;
			glFrontFace(front_face);
		}
		if (gl_state.cull_mode != cull_mode)
		{
			gl_state.cull_mode = cull_mode;
			glCullFace(cull_mode);
		}
	}
#ifndef HALO_ILP32
	/* ES draws filled polygons only (wireframe is a debug mode) */
	{
		GLenum polygon_mode = raster->fill_mode == GPU_FILL_LINE ? GL_LINE :
			raster->fill_mode == GPU_FILL_POINT ? GL_POINT : GL_FILL;

		if (gl_state.polygon_mode != polygon_mode)
		{
			gl_state.polygon_mode = polygon_mode;
			glPolygonMode(GL_FRONT_AND_BACK, polygon_mode);
		}
	}
#endif

	state_enable(&gl_state.offset_fill, GL_POLYGON_OFFSET_FILL, raster->depth_bias_enable);
#ifndef HALO_ILP32
	state_enable(&gl_state.offset_line, GL_POLYGON_OFFSET_LINE, raster->depth_bias_enable);
#endif
	if (raster->depth_bias_enable)
	{
		float offset[2];

		offset[0] = raster->depth_bias_slope;
		offset[1] = raster->depth_bias_constant;
		if (memcmp(gl_state.polygon_offset, offset, sizeof(offset)))
		{
			memcpy(gl_state.polygon_offset, offset, sizeof(offset));
			glPolygonOffset(offset[0], offset[1]);
		}
	}
}

/* ---------- texture stages: a packet stage's texture and sampler object are
bound, and the sampler configured when the stage's sampler state changed */

/* one sampler object per texture stage, and the state each was last
configured with (not part of gl_state: a sampler object keeps its
parameters whatever GL state changes behind the cache) */
static GLuint samplers[D3DTSS_MAXSTAGES];
static struct gpu_sampler_state configured[D3DTSS_MAXSTAGES];
static BOOL configured_valid[D3DTSS_MAXSTAGES];
/* gpu_capabilities.border_clamp: without it BORDER addressing clamps to the
edge */
static BOOL border_clamp;
/* gpu_capabilities.base_vertex: indexed draws take a base vertex */
static BOOL base_vertex;

/* the GL target of a GPU_TEXTURE_* type */
static GLenum texture_target(uint32_t type)
{
	return type == GPU_TEXTURE_CUBE ? GL_TEXTURE_CUBE_MAP : type == GPU_TEXTURE_3D ? GL_TEXTURE_3D : GL_TEXTURE_2D;
}

static GLenum gl_address(uint32_t mode)
{
	switch (mode)
	{
	case GPU_ADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
	case GPU_ADDRESS_CLAMP:
	case GPU_ADDRESS_CLAMP_TO_EDGE: return GL_CLAMP_TO_EDGE;
	case GPU_ADDRESS_BORDER: return border_clamp ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
	default: return GL_REPEAT;
	}
}

static void apply_stage(int stage, const struct gpu_stage *packet_stage)
{
	const struct gpu_sampler_state *state = &packet_stage->sampler;
	GLuint sampler = samplers[stage];
	GLenum minification;
	float border[4];

	if (!packet_stage->type)
	{
		state_texture(stage, GL_TEXTURE_2D, 0);
		return;
	}
	state_texture(stage, texture_target(packet_stage->type), packet_stage->texture);
	state_sampler(stage, sampler);
	if (configured_valid[stage] && !memcmp(&configured[stage], state, sizeof(*state)))
		return;
	configured[stage] = *state;
	configured_valid[stage] = TRUE;

	if (state->min_filter == GPU_FILTER_POINT)
		minification = state->mip_filter == GPU_FILTER_NONE ? GL_NEAREST :
			state->mip_filter == GPU_FILTER_POINT ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
	else
		minification = state->mip_filter == GPU_FILTER_NONE ? GL_LINEAR :
			state->mip_filter == GPU_FILTER_POINT ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
	glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, (GLint)minification);
	glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, state->mag_filter == GPU_FILTER_POINT ? GL_NEAREST : GL_LINEAR);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, (GLint)gl_address(state->address_u));
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, (GLint)gl_address(state->address_v));
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, (GLint)gl_address(state->address_w));
#ifdef HALO_ILP32
	/* ES has no sampler LOD bias; the pixel shader applies it
	(texture_lod_bias) */
	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)state->max_mip_level);
	if (xgpu_capabilities.anisotropy)
		glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT,
			(state->min_filter == GPU_FILTER_ANISOTROPIC && state->max_anisotropy > 1) ? (float)state->max_anisotropy : 1.0f);
	if (border_clamp)
	{
		color_to_vec4(state->border_color, border);
		glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
	}
#else
	glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, state->lod_bias);
	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)state->max_mip_level);
	glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY,
		(state->min_filter == GPU_FILTER_ANISOTROPIC && state->max_anisotropy > 1) ? (float)state->max_anisotropy : 1.0f);
	color_to_vec4(state->border_color, border);
	glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
#endif
}

#ifndef HALO_ILP32
static void GLAPIENTRY gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
	GLsizei length, const GLchar *message, const void *user)
{
	(void)source; (void)id; (void)length; (void)user;
	if (severity != GL_DEBUG_SEVERITY_NOTIFICATION)
		platform_log("GL %s: %s", type == GL_DEBUG_TYPE_ERROR ? "error" : "debug", message);
}
#endif

void gpu_initialize(struct gpu_capabilities *capabilities)
{
	GLint major = 0, minor = 0, maximum_texture_size = 0;

	memset(capabilities, 0, sizeof(*capabilities));
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
	capabilities->max_texture_size = (uint32_t)maximum_texture_size;
	capabilities->triangle_fans = 1;
	capabilities->line_loops = 1;
#ifdef HALO_ILP32
	{
		BOOL es31 = major > 3 || (major == 3 && minor >= 1);
		BOOL es32 = major > 3 || (major == 3 && minor >= 2);

		/* clip control is emulated in the vertex shader (nv2a_vsh.c) */
		xgpu_capabilities.copy_image = es32 || host_gl_has_extension("GL_EXT_copy_image") ||
			host_gl_has_extension("GL_OES_copy_image");
		xgpu_capabilities.border_clamp = es32 || host_gl_has_extension("GL_EXT_texture_border_clamp") ||
			host_gl_has_extension("GL_OES_texture_border_clamp");
		xgpu_capabilities.anisotropy = host_gl_has_extension("GL_EXT_texture_filter_anisotropic");
		xgpu_capabilities.base_vertex = es32;
		xgpu_capabilities.shading_language = es31 ? "310 es" : "300 es";
		if (es31)
		{
			GLint counters = 0;

			glGetIntegerv(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, &counters);
			xgpu_capabilities.atomic_counters = counters > 0;
		}
		xgpu_capabilities.s3tc = host_gl_has_extension("GL_EXT_texture_compression_s3tc") ||
			(host_gl_has_extension("GL_EXT_texture_compression_dxt1") &&
			host_gl_has_extension("GL_ANGLE_texture_compression_dxt3") &&
			host_gl_has_extension("GL_ANGLE_texture_compression_dxt5"));
		platform_log("OpenGL ES %d.%d: copy image %d, border clamp %d, anisotropy %d, S3TC %d, sample counting %d",
			(int)major, (int)minor, xgpu_capabilities.copy_image, xgpu_capabilities.border_clamp,
			xgpu_capabilities.anisotropy, xgpu_capabilities.s3tc, xgpu_capabilities.atomic_counters);

		capabilities->vertex_bgra = 0;
		capabilities->base_vertex = xgpu_capabilities.base_vertex ? 1 : 0;
		capabilities->sampler_lod_bias = 0;
		capabilities->occlusion_mode = xgpu_capabilities.atomic_counters ?
			GPU_OCCLUSION_SHADER_COUNTER : GPU_OCCLUSION_ANY_SAMPLE;
		capabilities->s3tc = xgpu_capabilities.s3tc ? 1 : 0;
		capabilities->border_clamp = xgpu_capabilities.border_clamp ? 1 : 0;
		capabilities->shader_language = es31 ? 310 : 300;
		capabilities->shader_es = 1;
		capabilities->clip_y_flip = 1;
		capabilities->clip_z_remap = 1;
	}
#else
	if (config_boolean("debug.gl_debug"))
	{
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(gl_debug_callback, NULL);
	}
	glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
	glEnable(GL_PROGRAM_POINT_SIZE);
	capabilities->vertex_bgra = 1;
	capabilities->base_vertex = 1;
	capabilities->sampler_lod_bias = 1;
	capabilities->occlusion_mode = GPU_OCCLUSION_EXACT;
	capabilities->s3tc = 1;
	capabilities->border_clamp = 1;
	capabilities->shader_language = 450;
#endif
	glGenVertexArrays(1, &streams.vertex_array);
	glBindVertexArray(streams.vertex_array);
#ifdef HALO_ILP32
	{
		int ring;

		glGenBuffers(STREAM_BUFFER_RING, streams.stream_buffers);
		glGenBuffers(STREAM_BUFFER_RING, streams.index_buffers);
		for (ring = 0; ring < STREAM_BUFFER_RING; ring++)
		{
			glBindBuffer(GL_ARRAY_BUFFER, streams.stream_buffers[ring]);
			glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, streams.index_buffers[ring]);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		}
		streams.stream_buffer = streams.stream_buffers[0];
		streams.index_buffer = streams.index_buffers[0];
	}
#endif
#ifndef HALO_ILP32
	glGenBuffers(1, &streams.stream_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, streams.stream_buffer);
	glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
	glGenBuffers(1, &streams.index_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, streams.index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
#endif
	border_clamp = capabilities->border_clamp ? TRUE : FALSE;
	base_vertex = capabilities->base_vertex ? TRUE : FALSE;
	glGenSamplers(D3DTSS_MAXSTAGES, samplers);
}

/* ---------- textures

In this backend a gpu_texture is the GL texture name, so the framebuffer
cache and the traces keep their keys. Each name has a record of what it was
created as. */

struct texture_record
{
	struct gpu_texture_description description;
	GLenum target;
};

static struct texture_record *texture_records;
static unsigned long texture_record_count;

static struct texture_record *texture_record(gpu_texture texture)
{
	if (texture >= texture_record_count)
	{
		unsigned long count = texture_record_count ? texture_record_count : 256;

		while (count <= texture)
			count *= 2;
		/* (allocation failure crashes, like the calloc calls elsewhere) */
		texture_records = realloc(texture_records, count * sizeof(*texture_records));
		memset(texture_records + texture_record_count, 0, (count - texture_record_count) * sizeof(*texture_records));
		texture_record_count = count;
	}
	return &texture_records[texture];
}

static GLsizei texture_level_dimension(uint32_t size, uint32_t level)
{
	return (GLsizei)(size >> level ? size >> level : 1);
}

gpu_texture gpu_texture_create(const struct gpu_texture_description *description)
{
	struct texture_record *record;
	GLuint name = 0;
	uint32_t level;

	glGenTextures(1, &name);
	record = texture_record(name);
	record->description = *description;
	record->target = texture_target(description->type);
	/* upload textures get their storage from each refresh's uploads */
	if (description->usage != GPU_USAGE_RENDER_TARGET)
		return name;
	glBindTexture(GL_TEXTURE_2D, name);
	if (description->levels > 1)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)description->levels - 1);
	for (level = 0; level < description->levels; level++)
	{
		GLsizei width = texture_level_dimension(description->width, level);
		GLsizei height = texture_level_dimension(description->height, level);

		if (description->format == GPU_FORMAT_DEPTH_STENCIL)
			glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL,
				GL_UNSIGNED_INT_24_8, NULL);
		else
			glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
	/* (mip composites didn't reset here before; mip_composite_get always
	resets before the draw goes on, so nothing changes) */
	xgpu_gl_state_invalidate();
	return name;
}

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83f1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83f2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83f3
#endif

static GLenum compressed_format(uint32_t format)
{
	switch (format)
	{
	case GPU_FORMAT_BC1: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case GPU_FORMAT_BC2: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	default: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	}
}

void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	struct texture_record *record = texture_record(texture);
	const struct gpu_texture_description *description = &record->description;
	BOOL compressed = description->format >= GPU_FORMAT_BC1 && description->format <= GPU_FORMAT_BC3;
	GLenum image_target = description->type == GPU_TEXTURE_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : record->target;
	GLsizei width = texture_level_dimension(description->width, level);
	GLsizei height = texture_level_dimension(description->height, level);
	GLsizei depth = texture_level_dimension(description->depth, level);

	/* once per refresh, before its first face and level */
	if (face == 0 && level == 0)
	{
		glBindTexture(record->target, texture);
		xgpu_gl_state_invalidate();
#ifdef HALO_ILP32
		/* BGRA8 texels are 32-bit ARGB words in memory; ES takes RGBA */
		glTexParameteri(record->target, GL_TEXTURE_SWIZZLE_R, compressed ? GL_RED : GL_BLUE);
		glTexParameteri(record->target, GL_TEXTURE_SWIZZLE_B, compressed ? GL_BLUE : GL_RED);
#endif
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexParameteri(record->target, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(record->target, GL_TEXTURE_MAX_LEVEL, (GLint)description->levels - 1);
	}
	if (compressed)
	{
		if (record->target == GL_TEXTURE_3D)
			glCompressedTexImage3D(image_target, (GLint)level, compressed_format(description->format), width, height, depth, 0,
				(GLsizei)size, data);
		else
			glCompressedTexImage2D(image_target, (GLint)level, compressed_format(description->format), width, height, 0,
				(GLsizei)size, data);
	}
	else if (record->target == GL_TEXTURE_3D)
	{
		glTexImage3D(image_target, (GLint)level, GL_RGBA8, width, height, depth, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
	}
	else
	{
		glTexImage2D(image_target, (GLint)level, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
	}
}

void gpu_texture_destroy(gpu_texture texture)
{
	GLuint name = texture;

	glDeleteTextures(1, &name);
	/* deleting a bound texture unbinds it */
	xgpu_gl_state_invalidate();
	/* GL may hand the name out again */
	memset(texture_record(texture), 0, sizeof(struct texture_record));
}

/* ---------- framebuffers, cached by attachment */

struct framebuffer_entry
{
	struct framebuffer_entry *next;
	GLuint color;
	GLuint depth;
	GLuint framebuffer;
};

static struct framebuffer_entry *framebuffers;

GLuint gpu_gl_framebuffer_get(GLuint color, GLuint depth)
{
	struct framebuffer_entry *entry;
	GLenum draw_buffer = color ? GL_COLOR_ATTACHMENT0 : GL_NONE;

	for (entry = framebuffers; entry; entry = entry->next)
	{
		if (entry->color == color && entry->depth == depth)
			return entry->framebuffer;
	}
	entry = calloc(1, sizeof(*entry));
	entry->color = color;
	entry->depth = depth;
	glGenFramebuffers(1, &entry->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, entry->framebuffer);
	if (color)
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	if (depth)
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
	glDrawBuffers(1, &draw_buffer);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		platform_log("framebuffer %u/%u is incomplete", color, depth);
	xgpu_gl_state_invalidate();
	entry->next = framebuffers;
	framebuffers = entry;
	return entry->framebuffer;
}

#ifdef HALO_ILP32
/* glCopyImageSubData for ES 3.0/3.1 contexts without the extension */
static void copy_level_by_blit(GLuint source, GLuint destination, GLint level, GLsizei width, GLsizei height)
{
	static GLuint draw_framebuffer;

	if (!draw_framebuffer)
		glGenFramebuffers(1, &draw_framebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu_gl_framebuffer_get(source, 0));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, level);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	/* the blit bypasses the cached state, so the next draw must re-apply it */
	xgpu_gl_state_invalidate();
}
#endif

void gpu_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	const struct gpu_texture_description *description = &texture_record(destination)->description;
	GLsizei width = texture_level_dimension(description->width, level);
	GLsizei height = texture_level_dimension(description->height, level);

#ifdef HALO_ILP32
	if (!xgpu_capabilities.copy_image)
	{
		copy_level_by_blit(source, destination, (GLint)level, width, height);
		return;
	}
#endif
	glCopyImageSubData(source, GL_TEXTURE_2D, 0, 0, 0, 0,
		destination, GL_TEXTURE_2D, (GLint)level, 0, 0, 0, width, height, 1);
}

void gpu_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, (GLint)base_level);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	xgpu_gl_state_invalidate();
}

/* ---------- buffers

A gpu_buffer is the GL buffer name. */

gpu_buffer gpu_buffer_create(uint32_t size)
{
	GLuint name = 0;

	glGenBuffers(1, &name);
	glBindBuffer(GL_COPY_WRITE_BUFFER, name);
	glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, NULL, GL_DYNAMIC_DRAW);
	return name;
}

void gpu_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
#ifdef HALO_ILP32
	/* Mali copies the whole buffer for a glBufferSubData that queued draws
	might read (see STREAM_BUFFER_RING); unused ranges can be written without
	waiting for them */
	if (flags & GPU_WRITE_UNUSED)
	{
		host_gl_buffer_write(GL_COPY_WRITE_BUFFER, offset, size, data);
		return;
	}
#else
	(void)flags;
#endif
	glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
}

void gpu_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	if (streams.stream_offset + vertex_bytes > STREAM_BUFFER_SIZE)
	{
		/* orphan the buffer and start again */
		state_array_buffer(streams.stream_buffer);
		glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		streams.stream_offset = 0;
	}
	/* step 3's front end passes 0: today the index buffer makes room as each
	range is uploaded (gpu_stream), which keeps the GL call order; a backend
	that needs a draw's index bytes up front gets them from the draw packet
	(sub-step e) */
	if (index_bytes && streams.index_offset + index_bytes > INDEX_BUFFER_SIZE)
	{
		state_element_array_buffer(streams.index_buffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		streams.index_offset = 0;
	}
}

uint32_t gpu_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	unsigned long offset;

	size = (size + 15) & ~15U;
	if (kind == GPU_STREAM_KIND_INDEX)
	{
		/* the index buffer makes room as each range comes */
		state_element_array_buffer(streams.index_buffer);
		if (streams.index_offset + size > INDEX_BUFFER_SIZE)
		{
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
			streams.index_offset = 0;
		}
		offset = streams.index_offset;
#ifdef HALO_ILP32
		host_gl_buffer_write(GL_ELEMENT_ARRAY_BUFFER, (unsigned int)offset, (unsigned int)size, data);
#else
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
#endif
		streams.index_offset += size;
		*buffer = streams.index_buffer;
		return (uint32_t)offset;
	}
	gpu_stream_reserve(size, 0);
	offset = streams.stream_offset;
	state_array_buffer(streams.stream_buffer);
#ifdef HALO_ILP32
	host_gl_buffer_write(GL_ARRAY_BUFFER, (unsigned int)offset, (unsigned int)size, data);
#else
	glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
#endif
	streams.stream_offset += size;
	*buffer = streams.stream_buffer;
	return (uint32_t)offset;
}

/* the frame is presented: on ES, fence this ring slot and wait for the next
one's frame to finish; on desktop GL, orphan both buffers at the next use */
void gpu_gl_stream_frame(void)
{
#ifdef HALO_ILP32
	host_gl_fence_frame((unsigned int)streams.buffer_ring);
	streams.buffer_ring = (streams.buffer_ring + 1) % STREAM_BUFFER_RING;
	host_gl_wait_frame((unsigned int)streams.buffer_ring);
	streams.stream_buffer = streams.stream_buffers[streams.buffer_ring];
	streams.index_buffer = streams.index_buffers[streams.buffer_ring];
	streams.stream_offset = 0;
	streams.index_offset = 0;
#else
	streams.stream_offset = STREAM_BUFFER_SIZE;
	streams.index_offset = INDEX_BUFFER_SIZE;
#endif
}

/* ---------- shaders */

static GLuint compile_shader(GLenum type, const char *source, const char *what)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[4096];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("cannot compile the %s shader:\n%s\n%s", what, log, source);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

gpu_shader gpu_shader_create(uint32_t stage, const char *source)
{
	return stage == GPU_SHADER_VERTEX ? compile_shader(GL_VERTEX_SHADER, source, "vertex") :
		compile_shader(GL_FRAGMENT_SHADER, source, "pixel");
}

/* ---------- programs */

struct gpu_gl_program
{
	struct gpu_gl_program *next;
	GLuint vertex_shader;
	GLuint fragment_shader;
	GLuint program;
	GLint constants;
#define GPU_GL_LOCATION(name, glsl_type, count, stage) GPU_UNIFORM_IF_NOT_CONSTANTS_##stage(GLint name;)
	GPU_UNIFORMS(GPU_GL_LOCATION)
#undef GPU_GL_LOCATION
	/* the vertex constants c[0..constant_count) the program uses; with
	consecutive locations, a changed range is uploaded by itself */
	unsigned long constant_count;
	BOOL constants_consecutive;
	/* gpu_constant_store.serial at the program's last constant upload */
	uint32_t constants_serial;
	/* gpu_uniforms.serial when the uniforms below were brought up to date */
	uint32_t uniforms_serial;
	/* what the program's other uniforms hold (all ones: unknown) */
	struct gpu_uniforms uniforms;
};

#define PROGRAM_BUCKETS 1024

static struct gpu_gl_program *program_buckets[PROGRAM_BUCKETS];

static struct gpu_gl_program *program_get(gpu_shader vertex_shader, gpu_shader fragment_shader)
{
	static struct gpu_gl_program *last;
	unsigned long hash = (vertex_shader * 2654435761UL) ^ fragment_shader;
	struct gpu_gl_program **bucket = &program_buckets[hash % PROGRAM_BUCKETS];
	struct gpu_gl_program *entry;
	GLint status = 0;
	int stage;

	if (last && last->vertex_shader == vertex_shader && last->fragment_shader == fragment_shader)
		return last;
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->vertex_shader == vertex_shader && entry->fragment_shader == fragment_shader)
		{
			if (!entry->program)
				return NULL;
			last = entry;
			return entry;
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->vertex_shader = vertex_shader;
	entry->fragment_shader = fragment_shader;
	memset(&entry->uniforms, 0xff, sizeof(entry->uniforms));
	entry->next = *bucket;
	*bucket = entry;
	if (!vertex_shader || !fragment_shader)
		return NULL;

	entry->program = glCreateProgram();
	glAttachShader(entry->program, vertex_shader);
	glAttachShader(entry->program, fragment_shader);
	glLinkProgram(entry->program);
	glGetProgramiv(entry->program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[4096];

		glGetProgramInfoLog(entry->program, sizeof(log), NULL, log);
		platform_log("cannot link a shader program: %s", log);
		entry->program = 0;
		return NULL;
	}
	state_program(entry->program);
	entry->constants = glGetUniformLocation(entry->program, "c");
	entry->constant_count = XGPU_VERTEX_CONSTANT_COUNT;
	if (entry->constants >= 0)
	{
		unsigned long index;

		/* c[i] is usually at c's location plus i, and the compiler may
		drop registers past the last one the program reads */
		entry->constants_consecutive = TRUE;
		for (index = 1; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
		{
			char name[16];
			GLint location;

			snprintf(name, sizeof(name), "c[%lu]", index);
			location = glGetUniformLocation(entry->program, name);
			if (location < 0)
			{
				entry->constant_count = index;
				break;
			}
			if (location != entry->constants + (GLint)index)
			{
				entry->constants_consecutive = FALSE;
				entry->constant_count = XGPU_VERTEX_CONSTANT_COUNT;
				break;
			}
		}
	}
#define GPU_GL_LOCATE(name, glsl_type, count, stage) \
	GPU_UNIFORM_IF_NOT_CONSTANTS_##stage(entry->name = glGetUniformLocation(entry->program, #name);)
	GPU_UNIFORMS(GPU_GL_LOCATE)
#undef GPU_GL_LOCATE
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		char name[8];

		snprintf(name, sizeof(name), "tex%d", stage);
		glUniform1i(glGetUniformLocation(entry->program, name), stage);
	}
	last = entry;
	return entry;
}

/* sets a program's uniform unless it already holds value */
static void uniform_vec4(GLint location, float *shadow, const float *value, int count)
{
	if (location < 0 || !memcmp(shadow, value, (size_t)count * 4 * sizeof(float)))
		return;
	memcpy(shadow, value, (size_t)count * 4 * sizeof(float));
	glUniform4fv(location, count, value);
}

static void uniform_float(GLint location, float *shadow, float value)
{
	if (location < 0 || !memcmp(shadow, &value, sizeof(value)))
		return;
	*shadow = value;
	glUniform1f(location, value);
}

static void program_use(struct gpu_gl_program *program)
{
	state_program(program->program);
}

static void program_constants(struct gpu_gl_program *entry, const struct gpu_constant_store *store)
{
	if (entry->constants >= 0 && entry->constants_serial != store->serial)
	{
		unsigned long first = entry->constant_count, last = 0, index;

		if (store->serial - entry->constants_serial <= XGPU_VERTEX_CONSTANT_COUNT)
		{
			uint32_t serial;

			for (serial = entry->constants_serial + 1; serial <= store->serial; serial++)
			{
				index = store->log[serial % GPU_CONSTANT_LOG_SIZE];
				if (index >= entry->constant_count)
					continue;
				if (first > index)
					first = index;
				if (last < index)
					last = index;
			}
		}
		else
		{
			for (index = 0; index < entry->constant_count; index++)
			{
				if (store->serials[index] > entry->constants_serial)
				{
					if (first > index)
						first = index;
					last = index;
				}
			}
		}
		if (first < entry->constant_count)
		{
			if (entry->constants_consecutive)
				glUniform4fv(entry->constants + (GLint)first, (GLsizei)(last - first + 1), store->c[first]);
			else
				glUniform4fv(entry->constants, XGPU_VERTEX_CONSTANT_COUNT, &store->c[0][0]);
		}
		entry->constants_serial = store->serial;
	}
}

static void program_uniforms(struct gpu_gl_program *entry, const struct gpu_uniforms *uniforms)
{
	/* a program that has had these uniforms since needs none of them */
	if (entry->uniforms_serial == uniforms->serial)
		return;
	entry->uniforms_serial = uniforms->serial;
#define GPU_GL_UPLOAD_vec4(name, count) \
	uniform_vec4(entry->name, entry->uniforms.name[0], uniforms->name[0], count);
#define GPU_GL_UPLOAD_float(name, count) \
	uniform_float(entry->name, &entry->uniforms.name[0][0], uniforms->name[0][0]);
#define GPU_GL_UPLOAD(name, glsl_type, count, stage) \
	GPU_UNIFORM_IF_NOT_CONSTANTS_##stage(GPU_GL_UPLOAD_##glsl_type(name, count))
	GPU_UNIFORMS(GPU_GL_UPLOAD)
#undef GPU_GL_UPLOAD
#undef GPU_GL_UPLOAD_float
#undef GPU_GL_UPLOAD_vec4
}

/* ---------- drawing */

static GLenum gl_primitive(uint32_t primitive)
{
	switch (primitive)
	{
	case GPU_PRIMITIVE_POINTS: return GL_POINTS;
	case GPU_PRIMITIVE_LINES: return GL_LINES;
	case GPU_PRIMITIVE_LINE_LOOP: return GL_LINE_LOOP;
	case GPU_PRIMITIVE_LINE_STRIP: return GL_LINE_STRIP;
	case GPU_PRIMITIVE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
	case GPU_PRIMITIVE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
	default: return GL_TRIANGLES;
	}
}

uint32_t gpu_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	struct gpu_gl_program *program = program_get(draw->vertex_shader, draw->pixel_shader);
	GLenum mode = gl_primitive(draw->primitive);
	uint32_t index;
	int stage;

	if (!program)
		return 0;
	gpu_gl_state_framebuffer(gpu_gl_framebuffer_get(draw->color_target, draw->depth_target));
	apply_raster_state(&draw->viewport, &draw->scissor, &draw->depth_stencil, &draw->blend, &draw->raster);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		apply_stage(stage, &draw->stages[stage]);
	program_use(program);
	program_constants(program, constants);
	program_uniforms(program, uniforms);
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		const struct gpu_vertex_attribute *attribute = &draw->attributes[index];

		apply_attribute(index, attribute, attribute->stream < 16 ? &draw->streams[attribute->stream] : NULL,
			draw->constant_values[index]);
	}
	if (!draw->index_buffer)
	{
		glDrawArrays(mode, 0, (GLsizei)draw->count);
		return 1;
	}
	state_element_array_buffer(draw->index_buffer);
	if (base_vertex)
		glDrawElementsBaseVertex(mode, (GLsizei)draw->count, GL_UNSIGNED_SHORT,
			(const void *)(unsigned long)draw->index_offset, draw->base_vertex);
	else
		glDrawElements(mode, (GLsizei)draw->count, GL_UNSIGNED_SHORT, (const void *)(unsigned long)draw->index_offset);
	return 1;
}
