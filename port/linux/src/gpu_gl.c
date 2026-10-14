/*
GPU_GL.C

The OpenGL and OpenGL ES backend of gpu.h: every GL call the Direct3D front
end (d3d8_device.c, xbox_textures.c) needs, behind the handles and packets
gpu.h declares.
*/

#include "gpu.h"
#include "gl.h"
#include "halo_stereo.h"
#include "halo_display.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (xbox_kernel.c and sdl_platform.c on Linux and in the
guest; host_gpu.c in the iOS host); the format attribute matches the Linux
build's declaration, so the format strings here stay checked */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void platform_video_drawable_size(int *width, int *height);
void platform_video_swap(void);

#ifdef GPU_GL_ES
/* OpenGL ES 3 (port/ios/README.md): the desktop format, enumerants and entry
points used below that ES lacks */
#define GL_BGRA GL_RGBA
#define glDepthRange glDepthRangef
#define glClearDepth glClearDepthf
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

/* OpenGL ES features that are optional (gpu_initialize) */
struct xgpu_capabilities
{
	int copy_image;
	int border_clamp;
	int anisotropy;
	int s3tc;
	/* ES 3.2: glDrawElementsBaseVertex */
	int base_vertex;
	/* ES 3.1 with fragment atomic counters: exact visibility test counts */
	int atomic_counters;
};

/* what the context supports (gpu_initialize) */
static struct xgpu_capabilities xgpu_capabilities;

#ifdef GPU_GL_HOST
/* the iOS host (port/ios/host): SDL for the extension probe, and UIKit's
drawable framebuffer, which stands in for framebuffer 0 (ios_host.h) */
#include <SDL3/SDL.h>
uint32_t host_ios_default_framebuffer(void);
#endif
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST
/* SDL's extension check calls glGetString by name, which under Catalyst can
   resolve to desktop OpenGL's (gl_functions.c); use the loaded entry point. */
static bool catalyst_extension_supported(const char *name)
{
	const char *list = (const char *)glGetString(GL_EXTENSIONS);
	size_t length = strlen(name);
	for (const char *at = list; at && (at = strstr(at, name)); at += length)
		if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0))
			return true;
	return false;
}
#define gl_extension_supported catalyst_extension_supported
#else
#define gl_extension_supported SDL_GL_ExtensionSupported
#endif
#if !defined(GPU_GL_ES) && !defined(GPU_GL_HOST)
/* Intel's graphics with Mesa's driver can hang the GPU in a long run of
draws with no pipeline flush between them, which the game's effects make
(hundreds of small draws in a row): the command streamer stops at a draw,
and the reset that follows takes the desktop's other programs with it.
Intel's workaround for a hang of this kind on their DG2 graphics
(Wa_16014538804) is a flush at least every 3 draws, which Mesa does not
apply to the others. A memory barrier is one (and only that: nothing here
writes images). debug.gpu_flush_draws sets the interval; -1 picks 3 on Mesa
Intel and never elsewhere. */
#include "port_config.h"

static unsigned long flush_every, flush_draws;

static void draw_flush_initialize(void)
{
	long every = config_integer("debug.gpu_flush_draws");
	const char *renderer = (const char *)glGetString(GL_RENDERER);

	if (every < 0)
		every = renderer && strstr(renderer, "Mesa Intel") ? 3 : 0;
	if (every > 0)
	{
		flush_every = (unsigned long)every;
		platform_log("GPU: a pipeline flush every %ld draws", every);
	}
}

static void draw_flush(void)
{
	if (flush_every && ++flush_draws >= flush_every)
	{
		flush_draws = 0;
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}
}
#else
static void draw_flush_initialize(void)
{
}

static void draw_flush(void)
{
}
#endif

/* ---------- streams */

#ifdef GPU_GL_ES
/* Mobile drivers (Mali) keep every orphaned copy of a buffer until the GPU
is done with it, so a large buffer orphaned each frame costs its size per
frame in flight and more. Instead each frame streams into the next of a few
smaller buffers, reusing one only once the GPU has finished the frame that
last used it (gl_wait_frame). A busy frame streams about 5 MB of
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
#ifdef GPU_GL_ES
	GLuint stream_buffers[STREAM_BUFFER_RING];
	GLuint index_buffers[STREAM_BUFFER_RING];
	unsigned long buffer_ring;
#endif
	unsigned long stream_offset;
	GLuint index_buffer;
	unsigned long index_offset;
} streams;

#ifdef GPU_GL_ES
/* ---------- the GL that ran in the host before step 4 (host_gl.c)

These call the raw halo_gl* pointers, not the counted aliases: as the
host's services (host_gl.c) they ran outside the guest's GL call count, and
debug.gpu_stats' totals (gpu_call_count_take) stay comparable with the
records from before the backend moved into the host. */

/* writes a range no queued draw reads, without waiting for the GPU */
static void gl_buffer_write(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	void *mapped = halo_glMapBufferRange(target, offset, size,
		GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);

	if (mapped)
	{
		memcpy(mapped, data, (size_t)size);
		halo_glUnmapBuffer(target);
	}
	else
	{
		halo_glBufferSubData(target, offset, size, data);
	}
}

/* the fence of each ring slot's last frame */
static GLsync fences[STREAM_BUFFER_RING];

/* fences the GPU work queued so far as ring slot slot's */
static void gl_fence_frame(unsigned long slot)
{
	if (fences[slot])
		halo_glDeleteSync(fences[slot]);
	fences[slot] = halo_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

/* waits for the GPU to finish the work last fenced for slot */
static void gl_wait_frame(unsigned long slot)
{
	if (!fences[slot])
		return;
	halo_glClientWaitSync(fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	halo_glDeleteSync(fences[slot]);
	fences[slot] = NULL;
}

/* a 32-bit word of a buffer object, waiting for the GPU */
static GLuint gl_read_buffer_word(GLuint buffer, GLintptr offset)
{
	GLint previous = 0;
	GLuint value = 0;
	void *mapped;

	halo_glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &previous);
	halo_glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	mapped = halo_glMapBufferRange(GL_COPY_READ_BUFFER, offset, sizeof(value), GL_MAP_READ_BIT);
	if (mapped)
	{
		memcpy(&value, mapped, sizeof(value));
		halo_glUnmapBuffer(GL_COPY_READ_BUFFER);
	}
	halo_glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)previous);
	return value;
}
#endif

/* ---------- GL state cache

Consecutive draws share most of their state, but each sets all of it: the
setters here skip the call when GL already holds the value. Code that
changes GL state behind the cache's back (clears, presentation, texture
uploads, render target and framebuffer creation) calls
state_invalidate, after which every value is set again. Unknown
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
	GLenum blend_source, blend_destination, blend_alpha_source, blend_alpha_destination, blend_equation;
	float blend_color[4];
	unsigned char color_mask;
	GLenum front_face, cull_mode, polygon_mode;
	float polygon_offset[2];
	GLenum active_texture;
	/* per unit: the GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP and GL_TEXTURE_3D
	bindings */
	GLuint textures[GPU_STAGE_COUNT][3];
	GLuint samplers[GPU_STAGE_COUNT];
	GLuint array_buffer;
	GLuint element_array_buffer;
	unsigned char attribute_enabled[GPU_ATTRIBUTE_COUNT];
	struct attribute_pointer attribute_pointers[GPU_ATTRIBUTE_COUNT];
	/* a disabled attribute's value; kind 1 is the integer zero */
	unsigned char attribute_value_kind[GPU_ATTRIBUTE_COUNT];
	float attribute_values[GPU_ATTRIBUTE_COUNT][4];
} gl_state;

static void state_invalidate(void)
{
	memset(&gl_state, 0xff, sizeof(gl_state));
}

/* GL_DEPTH_CLAMP (GL_DEPTH_CLAMP_EXT on ES, 0x864f either way), which only
the first-person body's draws turn on (gpu_raster_state.depth_clamp). Its
state lives apart from gl_state: nothing else turns it on, so it stays
known across state_invalidate, and a frame without such a draw makes no GL
call for it (the GL records count every call). ES checks GL_EXT_depth_clamp
on the first draw that asks; without it the body is clipped at the near
plane as before */
static void depth_clamp_apply(int enabled)
{
	static int on, supported = -1;

	enabled = enabled != 0;
	if (enabled == on)
		return;
	if (supported < 0)
	{
#ifdef GPU_GL_ES
		supported = gl_extension_supported("GL_EXT_depth_clamp") ? 1 : 0;
#else
		supported = 1;
#endif
		platform_log("the first-person body's depth clamp: %s", supported ? "on" :
			"not supported (no GL_EXT_depth_clamp), so the body is clipped at the near plane");
	}
	if (!supported)
		return;
	on = enabled;
	if (on)
		glEnable(0x864f);
	else
		glDisable(0x864f);
}

static void state_enable(unsigned char *shadow, GLenum capability, int enabled)
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

static void state_framebuffer(GLuint framebuffer)
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
	int integer, GLsizei stride, unsigned long offset)
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
		GLenum alpha_source = blend->alpha_separate ? gl_blend_factor(blend->alpha_source) : source;
		GLenum alpha_destination = blend->alpha_separate ? gl_blend_factor(blend->alpha_destination) : destination;
		GLenum equation = gl_blend_equation(blend->operation);
		float blend_color[4];

		if (gl_state.blend_source != source || gl_state.blend_destination != destination ||
			gl_state.blend_alpha_source != alpha_source || gl_state.blend_alpha_destination != alpha_destination)
		{
			gl_state.blend_source = source;
			gl_state.blend_destination = destination;
			gl_state.blend_alpha_source = alpha_source;
			gl_state.blend_alpha_destination = alpha_destination;
			if (blend->alpha_separate)
				glBlendFuncSeparate(source, destination, alpha_source, alpha_destination);
			else
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
#ifndef GPU_GL_ES
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

	depth_clamp_apply(raster->depth_clamp);
	state_enable(&gl_state.offset_fill, GL_POLYGON_OFFSET_FILL, raster->depth_bias_enable);
#ifndef GPU_GL_ES
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
static GLuint samplers[GPU_STAGE_COUNT];
static struct gpu_sampler_state configured[GPU_STAGE_COUNT];
static int configured_valid[GPU_STAGE_COUNT];
/* gpu_capabilities.border_clamp: without it BORDER addressing clamps to the
edge */
static int border_clamp;
/* gpu_capabilities.base_vertex: indexed draws take a base vertex */
static int base_vertex;

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
	configured_valid[stage] = 1;

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
#ifdef GPU_GL_ES
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

/* ---------- visibility tests

A test runs on the scratch query (slot 0), which swaps into its slot when the
test ends. On ES with atomic counters the pixel shader counts instead, into a
ring of counters; on desktop GL the GPU writes each count into a mapped
buffer, so a result never waits. */

/* created by gpu_initialize, read by gpu_draw and the functions after gpu_clear */

#ifdef GPU_GL_ES
#define VISIBILITY_QUERY GL_ANY_SAMPLES_PASSED
#else
#define VISIBILITY_QUERY GL_SAMPLES_PASSED
#endif

static struct
{
	GLuint queries[GPU_VISIBILITY_SLOTS];
	int active;
#ifdef GPU_GL_ES
	int counters;                  /* GPU_OCCLUSION_SHADER_COUNTER */
	GLuint counter_buffer;
	unsigned long counter_next;
	unsigned long counter_active;
	unsigned long counter_of_slot[GPU_VISIBILITY_SLOTS];
#else
	GLuint results_buffer;
	volatile GLuint *results;
#endif
} visibility;

#ifndef GPU_GL_ES
static void GLAPIENTRY gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
	GLsizei length, const GLchar *message, const void *user)
{
	(void)source; (void)id; (void)length; (void)user;
	if (severity != GL_DEBUG_SEVERITY_NOTIFICATION)
		platform_log("GL %s: %s", type == GL_DEBUG_TYPE_ERROR ? "error" : "debug", message);
}
#endif

/* GPU_INITIALIZE_DEBUG: report GL errors (the debug callback on desktop GL,
check_errors on ES) */
static int gl_debug;

static void gpu_gl_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	GLint major = 0, minor = 0, maximum_texture_size = 0;

	gl_debug = (flags & GPU_INITIALIZE_DEBUG) != 0;
	memset(capabilities, 0, sizeof(*capabilities));
#ifdef GPU_GL_HOST
	/* the host resolves the entry points here (the guest did in
	platform_video_initialize, sdl_platform.c); Apple's ES 3.0 lacks the 3.2
	ones, whose callers the probes below never reach, and a missing ES 3.0
	one is fatal there (gl_functions.c) */
	gl_functions_load();
	/* counted, as the guest's two calls were */
	platform_log("OpenGL %s on %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
#endif
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
	capabilities->max_texture_size = (uint32_t)maximum_texture_size;
	capabilities->triangle_fans = 1;
	capabilities->line_loops = 1;
#ifdef GPU_GL_ES
	{
		int es31 = major > 3 || (major == 3 && minor >= 1);
		int es32 = major > 3 || (major == 3 && minor >= 2);

		/* clip control is emulated in the vertex shader (nv2a_vsh.c) */
		xgpu_capabilities.copy_image = es32 || gl_extension_supported("GL_EXT_copy_image") ||
			gl_extension_supported("GL_OES_copy_image");
		xgpu_capabilities.border_clamp = es32 || gl_extension_supported("GL_EXT_texture_border_clamp") ||
			gl_extension_supported("GL_OES_texture_border_clamp");
		xgpu_capabilities.anisotropy = gl_extension_supported("GL_EXT_texture_filter_anisotropic");
		xgpu_capabilities.base_vertex = es32;
		if (es31)
		{
			GLint counters = 0;

			glGetIntegerv(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, &counters);
			xgpu_capabilities.atomic_counters = counters > 0;
		}
		xgpu_capabilities.s3tc = gl_extension_supported("GL_EXT_texture_compression_s3tc") ||
			(gl_extension_supported("GL_EXT_texture_compression_dxt1") &&
			gl_extension_supported("GL_ANGLE_texture_compression_dxt3") &&
			gl_extension_supported("GL_ANGLE_texture_compression_dxt5"));
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
	if (gl_debug)
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
#ifdef GPU_GL_ES
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
#ifndef GPU_GL_ES
	glGenBuffers(1, &streams.stream_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, streams.stream_buffer);
	glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
	glGenBuffers(1, &streams.index_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, streams.index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
#endif
	border_clamp = capabilities->border_clamp ? 1 : 0;
	base_vertex = capabilities->base_vertex ? 1 : 0;
	glGenSamplers(GPU_STAGE_COUNT, samplers);
	glGenQueries(GPU_VISIBILITY_SLOTS, visibility.queries);
#ifndef GPU_GL_ES
	glGenBuffers(1, &visibility.results_buffer);
	glBindBuffer(GL_QUERY_BUFFER, visibility.results_buffer);
	glBufferStorage(GL_QUERY_BUFFER, GPU_VISIBILITY_SLOTS * sizeof(GLuint), NULL,
		GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
	visibility.results = glMapBufferRange(GL_QUERY_BUFFER, 0, GPU_VISIBILITY_SLOTS * sizeof(GLuint),
		GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
	if (!visibility.results)
		platform_log("cannot map the visibility test results; tests wait for the GPU");
#endif
#ifdef GPU_GL_ES
	visibility.counters = xgpu_capabilities.atomic_counters;
	if (visibility.counters)
	{
		glGenBuffers(1, &visibility.counter_buffer);
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, visibility.counter_buffer);
		glBufferData(GL_ATOMIC_COUNTER_BUFFER, GPU_VISIBILITY_SLOTS * sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
	}
#endif
	{
		/* every attribute starts as (0, 0, 0, 1), as the front end's
		device.attributes do */
		static const float initial[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		GLuint index;

		for (index = 0; index < GPU_ATTRIBUTE_COUNT; index++)
			glVertexAttrib4fv(index, initial);
	}
	state_invalidate();
	capabilities->upscale = 1;
	GPU_CAPABILITIES_LOG(platform_log, capabilities);
	draw_flush_initialize();
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

static gpu_texture gpu_gl_texture_create(const struct gpu_texture_description *description)
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
	state_invalidate();
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

static void gpu_gl_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	struct texture_record *record = texture_record(texture);
	const struct gpu_texture_description *description = &record->description;
	int compressed = description->format >= GPU_FORMAT_BC1 && description->format <= GPU_FORMAT_BC3;
	GLenum image_target = description->type == GPU_TEXTURE_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : record->target;
	GLsizei width = texture_level_dimension(description->width, level);
	GLsizei height = texture_level_dimension(description->height, level);
	GLsizei depth = texture_level_dimension(description->depth, level);

	/* once per refresh, before its first face and level */
	if (face == 0 && level == 0)
	{
		glBindTexture(record->target, texture);
		state_invalidate();
#ifdef GPU_GL_ES
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

static void gpu_gl_texture_destroy(gpu_texture texture)
{
	GLuint name = texture;

	glDeleteTextures(1, &name);
	/* deleting a bound texture unbinds it */
	state_invalidate();
	/* GL may hand the name out again */
	memset(texture_record(texture), 0, sizeof(struct texture_record));
}

/* the window's framebuffer: 0, or in the iOS host UIKit's drawable
framebuffer (which the GL bridge substituted for 0 before step 4) */
static GLuint default_framebuffer(void)
{
#ifdef GPU_GL_HOST
	return host_ios_default_framebuffer();
#else
	return 0;
#endif
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

static GLuint framebuffer_get(GLuint color, GLuint depth)
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
	state_invalidate();
	entry->next = framebuffers;
	framebuffers = entry;
	return entry->framebuffer;
}

#ifdef GPU_GL_ES
/* glCopyImageSubData for ES 3.0/3.1 contexts without the extension */
static void copy_level_by_blit(GLuint source, GLuint destination, GLint level, GLsizei width, GLsizei height)
{
	static GLuint draw_framebuffer;

	if (!draw_framebuffer)
		glGenFramebuffers(1, &draw_framebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(source, 0));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, level);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer());
	/* the blit bypasses the cached state, so the next draw must re-apply it */
	state_invalidate();
}
#endif

static void gpu_gl_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	const struct gpu_texture_description *description = &texture_record(destination)->description;
	GLsizei width = texture_level_dimension(description->width, level);
	GLsizei height = texture_level_dimension(description->height, level);

#ifdef GPU_GL_ES
	if (!xgpu_capabilities.copy_image)
	{
		copy_level_by_blit(source, destination, (GLint)level, width, height);
		return;
	}
#endif
	glCopyImageSubData(source, GL_TEXTURE_2D, 0, 0, 0, 0,
		destination, GL_TEXTURE_2D, (GLint)level, 0, 0, 0, width, height, 1);
}

static uint32_t gpu_gl_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	const struct gpu_texture_description *description = &texture_record(texture)->description;
	uint32_t width = description->width, height = description->height;

	if (description->type != GPU_TEXTURE_2D || size < width * height * 4)
		return 0;
	if (description->format == GPU_FORMAT_DEPTH_STENCIL)
	{
#ifdef GPU_GL_ES
		/* ES 3.0 reads back no depth */
		return 0;
#else
		/* the depth as floats, rows in the order the color reads give them */
		state_texture(0, GL_TEXTURE_2D, texture);
		if (gl_state.active_texture != GL_TEXTURE0)
		{
			gl_state.active_texture = GL_TEXTURE0;
			glActiveTexture(GL_TEXTURE0);
		}
		glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, pixels);
		return 1;
#endif
	}
	if (description->usage != GPU_USAGE_RENDER_TARGET)
	{
#ifdef GPU_GL_ES
		static GLuint read_framebuffer;

		/* ES cannot read a block-compressed texture back */
		if (description->format != GPU_FORMAT_BGRA8)
			return 0;
		/* through a framebuffer of its own, not framebuffer_get's cache: that
		is keyed by texture name, and GL reuses the names of destroyed upload
		textures. The read sees the stored bytes, not the sampling swizzle, and
		an upload's stored bytes are already BGRA */
		if (!read_framebuffer)
			glGenFramebuffers(1, &read_framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
#else
		/* bound on unit 0 through the cache, so the cache stays right; and
		unit 0 made active even when the cache already had the texture there,
		since glGetTexImage reads the active unit's binding */
		state_texture(0, GL_TEXTURE_2D, texture);
		if (gl_state.active_texture != GL_TEXTURE0)
		{
			gl_state.active_texture = GL_TEXTURE0;
			glActiveTexture(GL_TEXTURE0);
		}
		glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
#endif
		return 1;
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(texture, 0));
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
#ifdef GPU_GL_ES
	{
		/* a render target is RGBA8, and ES reads it as RGBA (GL_BGRA is an
		alias here) */
		unsigned char *bytes = pixels;
		uint32_t pixel;

		for (pixel = 0; pixel < width * height; pixel++)
		{
			unsigned char red = bytes[pixel * 4];

			bytes[pixel * 4] = bytes[pixel * 4 + 2];
			bytes[pixel * 4 + 2] = red;
		}
	}
#endif
	return 1;
}

static void gpu_gl_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, (GLint)base_level);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	state_invalidate();
}

/* ---------- buffers

A gpu_buffer is the GL buffer name. */

static gpu_buffer gpu_gl_buffer_create(uint32_t size)
{
	GLuint name = 0;

	glGenBuffers(1, &name);
	glBindBuffer(GL_COPY_WRITE_BUFFER, name);
	glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, NULL, GL_DYNAMIC_DRAW);
	return name;
}

static void gpu_gl_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
#ifdef GPU_GL_ES
	/* Mali copies the whole buffer for a glBufferSubData that queued draws
	might read (see STREAM_BUFFER_RING); unused ranges can be written without
	waiting for them */
	if (flags & GPU_WRITE_UNUSED)
	{
		gl_buffer_write(GL_COPY_WRITE_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
		return;
	}
#else
	(void)flags;
#endif
	glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
}

static void gpu_gl_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
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

static uint32_t gpu_gl_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
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
#ifdef GPU_GL_ES
		gl_buffer_write(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
#else
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
#endif
		streams.index_offset += size;
		*buffer = streams.index_buffer;
		return (uint32_t)offset;
	}
	gpu_gl_stream_reserve(size, 0);
	offset = streams.stream_offset;
	state_array_buffer(streams.stream_buffer);
#ifdef GPU_GL_ES
	gl_buffer_write(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
#else
	glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
#endif
	streams.stream_offset += size;
	*buffer = streams.stream_buffer;
	return (uint32_t)offset;
}

/* the frame is presented: on ES, fence this ring slot and wait for the next
one's frame to finish; on desktop GL, orphan both buffers at the next use */
static void stream_frame(void)
{
#ifdef GPU_GL_ES
	gl_fence_frame(streams.buffer_ring);
	streams.buffer_ring = (streams.buffer_ring + 1) % STREAM_BUFFER_RING;
	gl_wait_frame(streams.buffer_ring);
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

static gpu_shader gpu_gl_shader_create(uint32_t stage, const char *source)
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
	int constants_consecutive;
	/* gpu_constant_store.serial at the program's last constant upload */
	uint64_t constants_serial;
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
	entry->constant_count = GPU_CONSTANT_COUNT;
	if (entry->constants >= 0)
	{
		unsigned long index;

		/* c[i] is usually at c's location plus i, and the compiler may
		drop registers past the last one the program reads */
		entry->constants_consecutive = 1;
		for (index = 1; index < GPU_CONSTANT_COUNT; index++)
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
				entry->constants_consecutive = 0;
				entry->constant_count = GPU_CONSTANT_COUNT;
				break;
			}
		}
	}
#define GPU_GL_LOCATE(name, glsl_type, count, stage) \
	GPU_UNIFORM_IF_NOT_CONSTANTS_##stage(entry->name = glGetUniformLocation(entry->program, #name);)
	GPU_UNIFORMS(GPU_GL_LOCATE)
#undef GPU_GL_LOCATE
	for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
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

		if (store->serial - entry->constants_serial <= GPU_CONSTANT_COUNT)
		{
			uint64_t serial;

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
				glUniform4fv(entry->constants, GPU_CONSTANT_COUNT, &store->c[0][0]);
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

/* presents so far (gpu_present), for check_errors' log line */
static unsigned long frames;

#ifdef GPU_GL_ES
/* ES has no debug callback in 3.0; debug.gl_debug polls glGetError around
each draw instead, reporting each distinct error a few times */
static void check_errors(const char *where)
{
	static unsigned long reports;
	GLenum error;

	if (!gl_debug)
		return;
	while ((error = glGetError()) != GL_NO_ERROR)
	{
		if (reports++ < 200)
			platform_log("GL error %04x at %s (frame %lu)", (unsigned)error, where, frames);
	}
}
#else
#define check_errors(where) ((void)0)
#endif

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

static uint32_t gpu_gl_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	struct gpu_gl_program *program;
	GLenum mode = gl_primitive(draw->primitive);
	uint32_t index;
	int stage;

	draw_flush();
#ifdef GPU_GL_ES
	/* the counter the active visibility test adds to, bound before
	program_get, which may link a program */
	if (visibility.active && visibility.counters)
		glBindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, visibility.counter_buffer,
			(GLintptr)(visibility.counter_active * sizeof(GLuint)), sizeof(GLuint));
#endif
	program = program_get(draw->vertex_shader, draw->pixel_shader);
	if (!program)
	{
		check_errors("program");
		return 0;
	}
	state_framebuffer(framebuffer_get(draw->color_target, draw->depth_target));
	apply_raster_state(&draw->viewport, &draw->scissor, &draw->depth_stencil, &draw->blend, &draw->raster);
	for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
		apply_stage(stage, &draw->stages[stage]);
	program_use(program);
	program_constants(program, constants);
	program_uniforms(program, uniforms);
	for (index = 0; index < GPU_ATTRIBUTE_COUNT; index++)
	{
		const struct gpu_vertex_attribute *attribute = &draw->attributes[index];

		apply_attribute(index, attribute, attribute->stream < 16 ? &draw->streams[attribute->stream] : NULL,
			draw->constant_values[index]);
	}
	if (!draw->index_buffer)
	{
		glDrawArrays(mode, 0, (GLsizei)draw->count);
		check_errors("draw");
		return 1;
	}
	state_element_array_buffer(draw->index_buffer);
	if (base_vertex)
		glDrawElementsBaseVertex(mode, (GLsizei)draw->count, GL_UNSIGNED_SHORT,
			(const void *)(unsigned long)draw->index_offset, draw->base_vertex);
	else
		glDrawElements(mode, (GLsizei)draw->count, GL_UNSIGNED_SHORT, (const void *)(unsigned long)draw->index_offset);
	check_errors("draw");
	return 1;
}

static void gpu_gl_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
	float rgba[4];
	GLbitfield mask = 0;
	uint32_t index;

	state_framebuffer(framebuffer_get(clear->color_target, clear->depth_target));
	if (clear->flags & GPU_CLEAR_COLOR)
	{
		color_to_vec4(clear->color, rgba);
		glColorMask((clear->channel_mask & GPU_CHANNEL_RED) != 0, (clear->channel_mask & GPU_CHANNEL_GREEN) != 0,
			(clear->channel_mask & GPU_CHANNEL_BLUE) != 0, (clear->channel_mask & GPU_CHANNEL_ALPHA) != 0);
		glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
		mask |= GL_COLOR_BUFFER_BIT;
	}
	if (clear->flags & GPU_CLEAR_DEPTH)
	{
		glDepthMask(GL_TRUE);
		glClearDepth(clear->depth);
		mask |= GL_DEPTH_BUFFER_BIT;
	}
	if (clear->flags & GPU_CLEAR_STENCIL)
	{
		glStencilMask(0xff);
		glClearStencil((GLint)clear->stencil);
		mask |= GL_STENCIL_BUFFER_BIT;
	}
	if (!mask)
		return;
	glEnable(GL_SCISSOR_TEST);
	for (index = 0; index < count; index++)
	{
		glScissor(rectangles[index].x, rectangles[index].y, rectangles[index].width, rectangles[index].height);
		glClear(mask);
	}
	glDisable(GL_SCISSOR_TEST);
	/* the masks and the scissor bypassed the cached state */
	state_invalidate();
}

/* ---------- visibility tests (the state is declared before gpu_initialize) */

static void gpu_gl_visibility_begin(void)
{
	visibility.active = 1;
#ifdef GPU_GL_ES
	if (visibility.counters)
	{
		const GLuint zero = 0;

		visibility.counter_next = (visibility.counter_next + 1) % GPU_VISIBILITY_SLOTS;
		visibility.counter_active = visibility.counter_next;
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, visibility.counter_buffer);
		gl_buffer_write(GL_ATOMIC_COUNTER_BUFFER, (GLintptr)(visibility.counter_active * sizeof(GLuint)),
			sizeof(zero), &zero);
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
		return;
	}
#endif
	glBeginQuery(VISIBILITY_QUERY, visibility.queries[0]);
}

static void gpu_gl_visibility_end(uint32_t slot)
{
	GLuint scratch;

	visibility.active = 0;
#ifdef GPU_GL_ES
	if (visibility.counters)
	{
		visibility.counter_of_slot[slot] = visibility.counter_active;
		return;
	}
#endif
	glEndQuery(VISIBILITY_QUERY);
	scratch = visibility.queries[0];
	visibility.queries[0] = visibility.queries[slot];
	visibility.queries[slot] = scratch;
#ifndef GPU_GL_ES
	if (visibility.results)
	{
		/* the GPU writes the count into the slot once it is known */
		glBindBuffer(GL_QUERY_BUFFER, visibility.results_buffer);
		glGetQueryObjectuiv(visibility.queries[slot], GL_QUERY_RESULT, (GLuint *)(slot * sizeof(GLuint)));
		glBindBuffer(GL_QUERY_BUFFER, 0);
	}
#endif
}

static uint32_t gpu_gl_visibility_result(uint32_t slot, uint32_t *samples)
{
	GLuint available = 0, count = 0;

#ifdef GPU_GL_ES
	if (visibility.counters)
	{
		/* reading the buffer waits for the draws that counted */
		*samples = gl_read_buffer_word(visibility.counter_buffer,
			(GLintptr)(visibility.counter_of_slot[slot] * sizeof(GLuint)));
		return 1;
	}
#else
	if (visibility.results)
	{
		/* the latest count the GPU has written: from this test, or while
		the GPU is still behind, from the slot's earlier ones */
		*samples = visibility.results[slot];
		return 1;
	}
#endif
	glGetQueryObjectuiv(visibility.queries[slot], GL_QUERY_RESULT_AVAILABLE, &available);
	if (!available)
	{
		/* the game spins on an incomplete test (lens flares), so how often
		this runs depends on the GPU's timing: keep debug.gpu_stats' call
		count comparable between runs */
		halo_gl_call_count--;
		return 0;
	}
	glGetQueryObjectuiv(visibility.queries[slot], GL_QUERY_RESULT, &count);
	*samples = count;
	return 1;
}

/* ---------- anti-aliasing passes (gpu_anti_alias)

display.anti_aliasing's FXAA and SMAA, as upstream's xgpu_post.c (94882796),
run on a render target in place, over the rectangle of a window's 3D view.
Each pass draws one triangle covering the rectangle (its corners from
gl_VertexID, with no vertex data) and finds its texels from gl_FragCoord: a
target's rows are the picture's from the top, as the passes' own textures'
are, so no coordinate is turned over. The target's pixels are copied first,
so that no pass reads what it writes, and only color is written back: the
game keeps values of its own in destination alpha. The programs are built
when the setting is chosen (gpu_anti_alias_prepare), not in the middle of a
frame.

FXAA is upstream's, after Timothy Lottes' description: the direction of an
edge from the luma around a pixel, the edge's ends found by stepping along
it, and the pixel blended across it by where it lies between them, or by how
much it stands out from its neighbors. SMAA is port/third_party/smaa's,
compiled as GLSL at its HIGH preset (SMAA_GLSL_3 on OpenGL ES 3): edges from
luma, blending weights from its lookup textures, then the blend. The Metal
backend's passes (gpu_metal.m) are the same shaders in MSL. */

enum
{
	_post_program_fxaa,
	_post_program_smaa_edges,
	_post_program_smaa_weights,
	_post_program_smaa_blend,
	NUMBER_OF_POST_PROGRAMS
};

static struct
{
	GLuint vertex_shader;
	GLuint programs[NUMBER_OF_POST_PROGRAMS];
	/* each program's metrics uniform, and FXAA's bounds */
	GLint metrics[NUMBER_OF_POST_PROGRAMS];
	GLint bounds;
	/* FXAA's, and SMAA's, programs or textures could not be made */
	int failed[2];
	/* the size of the textures below */
	unsigned long width, height;
	/* the target's pixels before the pass, and SMAA's edges and blending
	weights */
	GLuint color, edges, weights;
	GLuint color_framebuffer, edges_framebuffer, weights_framebuffer;
	/* SMAA's lookup textures */
	GLuint area, search;
	/* linear and clamped, as FXAA and SMAA sample */
	GLuint sampler;
	/* no attributes: the triangle comes from gl_VertexID */
	GLuint vertex_array;
} post;

/* a growing string (the passes' shader text) */
struct post_text
{
	char *buffer;
	size_t length, capacity;
};

static void post_append(struct post_text *text, const char *string)
{
	size_t length = strlen(string);

	if (text->length + length + 1 > text->capacity)
	{
		text->capacity = (text->length + length + 1) * 2;
		text->buffer = realloc(text->buffer, text->capacity);
	}
	memcpy(text->buffer + text->length, string, length + 1);
	text->length += length;
}

static void post_header(struct post_text *text)
{
#ifdef GPU_GL_ES
	post_append(text,
		"#version 300 es\n"
		"precision highp float;\n"
		"precision highp int;\n"
		"precision highp sampler2D;\n");
#else
	post_append(text, "#version 450 core\n");
#endif
}

/* one triangle over the whole viewport */
static const char post_vertex_source[] =
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char post_fxaa_source[] =
	"uniform sampler2D color_texture;\n"
	"/* 1 / width, 1 / height, width, height */\n"
	"uniform vec4 metrics;\n"
	"/* the rectangle's outermost texel centers, as texture coordinates */\n"
	"uniform vec4 bounds;\n"
	"out vec4 result;\n"
	"\n"
	"const float EDGE_THRESHOLD = 0.125;\n"
	"const float EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
	"const float SUBPIXEL_QUALITY = 0.75;\n"
	"const int STEPS = 12;\n"
	"const float STEP_LENGTHS[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
	"\n"
	"vec3 color_at(vec2 position)\n"
	"{\n"
	"\treturn textureLod(color_texture, clamp(position, bounds.xy, bounds.zw), 0.0).rgb;\n"
	"}\n"
	"\n"
	"float luma_at(vec2 position)\n"
	"{\n"
	"\treturn dot(color_at(position), vec3(0.299, 0.587, 0.114));\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 position = gl_FragCoord.xy * metrics.xy;\n"
	"\tfloat luma = luma_at(position);\n"
	"\tfloat up = luma_at(position + vec2(0.0, -metrics.y));\n"
	"\tfloat down = luma_at(position + vec2(0.0, metrics.y));\n"
	"\tfloat left = luma_at(position + vec2(-metrics.x, 0.0));\n"
	"\tfloat right = luma_at(position + vec2(metrics.x, 0.0));\n"
	"\tfloat highest = max(luma, max(max(up, down), max(left, right)));\n"
	"\tfloat range = highest - min(luma, min(min(up, down), min(left, right)));\n"
	"\n"
	"\t/* no edge: the target has the pixel already */\n"
	"\tif (range < max(EDGE_THRESHOLD_MINIMUM, highest * EDGE_THRESHOLD))\n"
	"\t\tdiscard;\n"
	"\tfloat up_left = luma_at(position + vec2(-metrics.x, -metrics.y));\n"
	"\tfloat up_right = luma_at(position + vec2(metrics.x, -metrics.y));\n"
	"\tfloat down_left = luma_at(position + vec2(-metrics.x, metrics.y));\n"
	"\tfloat down_right = luma_at(position + vec2(metrics.x, metrics.y));\n"
	"\n"
	"\t/* how much the pixel stands out from its neighborhood (a feature\n"
	"\tthinner than a pixel) */\n"
	"\tfloat average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
	"\tfloat subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
	"\tsubpixel = subpixel * subpixel * SUBPIXEL_QUALITY;\n"
	"\n"
	"\t/* an edge along the rows where the luma changes more from row to row\n"
	"\tthan from column to column; across it, the side it changes more to */\n"
	"\tbool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
	"\t\tabs(up_right + down_right - 2.0 * right) >=\n"
	"\t\tabs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
	"\t\tabs(down_left + down_right - 2.0 * down);\n"
	"\tfloat before = horizontal ? up : left;\n"
	"\tfloat after = horizontal ? down : right;\n"
	"\tbool before_steeper = abs(before - luma) >= abs(after - luma);\n"
	"\tfloat gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
	"\tfloat edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
	"\tvec2 across = (horizontal ? vec2(0.0, metrics.y) : vec2(metrics.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
	"\tvec2 along = horizontal ? vec2(metrics.x, 0.0) : vec2(0.0, metrics.y);\n"
	"\n"
	"\t/* along the edge, half a pixel across, to where its luma ends either way */\n"
	"\tvec2 end_before = position + 0.5 * across - along;\n"
	"\tvec2 end_after = position + 0.5 * across + along;\n"
	"\tfloat luma_before = luma_at(end_before) - edge_luma;\n"
	"\tfloat luma_after = luma_at(end_after) - edge_luma;\n"
	"\tbool reached_before = abs(luma_before) >= gradient;\n"
	"\tbool reached_after = abs(luma_after) >= gradient;\n"
	"\tfor (int index = 1; index < STEPS && !(reached_before && reached_after); index++)\n"
	"\t{\n"
	"\t\tif (!reached_before)\n"
	"\t\t{\n"
	"\t\t\tend_before -= along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_before = luma_at(end_before) - edge_luma;\n"
	"\t\t\treached_before = abs(luma_before) >= gradient;\n"
	"\t\t}\n"
	"\t\tif (!reached_after)\n"
	"\t\t{\n"
	"\t\t\tend_after += along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_after = luma_at(end_after) - edge_luma;\n"
	"\t\t\treached_after = abs(luma_after) >= gradient;\n"
	"\t\t}\n"
	"\t}\n"
	"\n"
	"\t/* blended across the edge the more, the nearer the pixel is to an end;\n"
	"\tonly where the luma at that end changes the way the pixel's does */\n"
	"\tfloat distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
	"\tfloat distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
	"\tfloat pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
	"\tbool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
	"\tresult = vec4(color_at(position + max(varies ? pixel_offset : 0.0, subpixel) * across), 1.0);\n"
	"}\n";

/* the main functions of SMAA's passes, after SMAA.hlsl; its vertex
functions' offsets are found for each pixel */
static const char *const post_smaa_mains[] =
{
	/* edges */
	"uniform sampler2D color_texture;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
	"\tvec4 offset[3];\n"
	"\tSMAAEdgeDetectionVS(texcoord, offset);\n"
	"\tresult = vec4(SMAALumaEdgeDetectionPS(texcoord, offset, color_texture), 0.0, 0.0);\n"
	"}\n",
	/* weights */
	"uniform sampler2D edges_texture;\n"
	"uniform sampler2D area_texture;\n"
	"uniform sampler2D search_texture;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
	"\tvec2 pixcoord;\n"
	"\tvec4 offset[3];\n"
	"\tSMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
	"\tresult = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_texture, area_texture,\n"
	"\t\tsearch_texture, vec4(0.0));\n"
	"}\n",
	/* blend */
	"uniform sampler2D color_texture;\n"
	"uniform sampler2D weights_texture;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
	"\tvec4 offset;\n"
	"\tSMAANeighborhoodBlendingVS(texcoord, offset);\n"
	"\tresult = SMAANeighborhoodBlendingPS(texcoord, offset, color_texture, weights_texture);\n"
	"}\n",
};

static GLuint post_link(GLuint vertex_shader, GLuint fragment_shader, const char *what)
{
	GLuint program = glCreateProgram();
	GLint status = 0;

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[4096];

		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		platform_log("cannot link the %s program: %s", what, log);
		return 0;
	}
	return program;
}

/* a program, its samplers given their texture units; 0 if it does not
build */
static GLuint post_program_build(int which, const char *smaa_shader)
{
	static const char *const names[NUMBER_OF_POST_PROGRAMS] =
	{
		"FXAA", "SMAA edge detection", "SMAA blending weight", "SMAA neighborhood blending"
	};
	static const struct
	{
		const char *name;
		GLint unit;
	} units[] =
	{
		{ "color_texture", 0 }, { "edges_texture", 0 }, { "area_texture", 1 },
		{ "search_texture", 2 }, { "weights_texture", 1 },
	};
	struct post_text text = { 0 };
	GLuint fragment_shader, program;
	unsigned long index;

	if (!post.vertex_shader)
	{
		post_header(&text);
		post_append(&text, post_vertex_source);
		post.vertex_shader = compile_shader(GL_VERTEX_SHADER, text.buffer, "anti-aliasing vertex");
		text.length = 0;
	}
	post_header(&text);
	if (which == _post_program_fxaa)
	{
		post_append(&text, post_fxaa_source);
	}
	else
	{
		post_append(&text,
#ifdef GPU_GL_ES
			"#define SMAA_GLSL_3 1\n"
#else
			"#define SMAA_GLSL_4 1\n"
#endif
			"uniform vec4 metrics;\n"
			"#define SMAA_RT_METRICS metrics\n"
			"#define SMAA_PRESET_HIGH 1\n");
		post_append(&text, smaa_shader);
		post_append(&text, "\n");
		post_append(&text, post_smaa_mains[which - _post_program_smaa_edges]);
	}
	fragment_shader = post.vertex_shader ? compile_shader(GL_FRAGMENT_SHADER, text.buffer, names[which]) : 0;
	free(text.buffer);
	if (!fragment_shader)
		return 0;
	program = post_link(post.vertex_shader, fragment_shader, names[which]);
	glDeleteShader(fragment_shader);
	if (!program)
		return 0;
	glUseProgram(program);
	/* (a name the program does not have is location -1, which is ignored) */
	for (index = 0; index < sizeof(units) / sizeof(units[0]); index++)
		glUniform1i(glGetUniformLocation(program, units[index].name), units[index].unit);
	post.metrics[which] = glGetUniformLocation(program, "metrics");
	if (which == _post_program_fxaa)
		post.bounds = glGetUniformLocation(program, "bounds");
	return program;
}

static GLuint post_texture_new(GLenum format, unsigned long width, unsigned long height, GLenum data_format,
	const void *data)
{
	GLuint texture;

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, (GLsizei)width, (GLsizei)height, 0, data_format,
		GL_UNSIGNED_BYTE, data);
	return texture;
}

static GLuint post_framebuffer_new(GLuint texture)
{
	GLuint framebuffer;
	GLenum draw_buffer = GL_COLOR_ATTACHMENT0;

	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glDrawBuffers(1, &draw_buffer);
	return framebuffer;
}

/* the passes' textures, of the target's size (SMAA's only for SMAA); color
is the target's format, so that the copy is exact */
static void post_textures_fit(int smaa, unsigned long width, unsigned long height)
{
	if (post.width != width || post.height != height)
	{
		GLuint textures[3] = { post.color, post.edges, post.weights };
		GLuint framebuffers[3] = { post.color_framebuffer, post.edges_framebuffer, post.weights_framebuffer };

		glDeleteFramebuffers(3, framebuffers);
		glDeleteTextures(3, textures);
		post.color = post.edges = post.weights = 0;
		post.color_framebuffer = post.edges_framebuffer = post.weights_framebuffer = 0;
		post.width = width;
		post.height = height;
		post.color = post_texture_new(GL_RGBA8, width, height, GL_RGBA, NULL);
		post.color_framebuffer = post_framebuffer_new(post.color);
	}
	if (smaa && !post.edges)
	{
		post.edges = post_texture_new(GL_RG8, width, height, GL_RG, NULL);
		post.weights = post_texture_new(GL_RGBA8, width, height, GL_RGBA, NULL);
		post.edges_framebuffer = post_framebuffer_new(post.edges);
		post.weights_framebuffer = post_framebuffer_new(post.weights);
	}
}

static void post_texture_bind(GLuint unit, GLuint texture)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBindSampler(unit, post.sampler);
}

/* a program's triangle drawn into a framebuffer over the rectangle;
color_only keeps the framebuffer's alpha */
static void post_pass_draw(int which, GLuint framebuffer, const GLint corners[4], int color_only)
{
	float metrics[4];

	metrics[0] = 1.0f / (float)post.width;
	metrics[1] = 1.0f / (float)post.height;
	metrics[2] = (float)post.width;
	metrics[3] = (float)post.height;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glViewport(corners[0], corners[1], corners[2] - corners[0], corners[3] - corners[1]);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, color_only ? GL_FALSE : GL_TRUE);
	glUseProgram(post.programs[which]);
	glUniform4fv(post.metrics[which], 1, metrics);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* an intermediate emptied, all of it, so that no edge of another window's
pass is found: SMAA's passes write only where there is an edge */
static void post_intermediate_clear(GLuint framebuffer)
{
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

static uint32_t gpu_gl_anti_alias_prepare(uint32_t method, const char *smaa_shader, const void *area_texels,
	const void *search_texels)
{
	int smaa = method == GPU_ANTI_ALIAS_SMAA;
	int first = smaa ? _post_program_smaa_edges : _post_program_fxaa;
	int last = smaa ? _post_program_smaa_blend : _post_program_fxaa;
	int which;

	if (post.failed[smaa])
		return 0;
	if (smaa && !smaa_shader)
	{
		post.failed[smaa] = 1;
		return 0;
	}
	for (which = first; which <= last; which++)
	{
		if (!post.programs[which] && !(post.programs[which] = post_program_build(which, smaa_shader)))
		{
			post.failed[smaa] = 1;
			state_invalidate();
			return 0;
		}
	}
	if (smaa && !post.area)
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		post.area = post_texture_new(GL_RG8, 160, 560, GL_RG, area_texels);
		post.search = post_texture_new(GL_R8, 64, 16, GL_RED, search_texels);
	}
	if (!post.sampler)
	{
		glGenSamplers(1, &post.sampler);
		glSamplerParameteri(post.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(post.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(post.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(post.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenVertexArrays(1, &post.vertex_array);
	}
	glBindVertexArray(streams.vertex_array);
	state_invalidate();
	return 1;
}

static void gpu_gl_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect)
{
	const struct gpu_texture_description *description = &texture_record(target)->description;
	int smaa = method == GPU_ANTI_ALIAS_SMAA;
	GLuint framebuffer;
	GLint corners[4];

	if (!post.programs[smaa ? _post_program_smaa_blend : _post_program_fxaa])
		return;
	corners[0] = rect->x < 0 ? 0 : rect->x;
	corners[1] = rect->y < 0 ? 0 : rect->y;
	corners[2] = rect->x + rect->width > (int32_t)description->width ? (GLint)description->width : rect->x + rect->width;
	corners[3] = rect->y + rect->height > (int32_t)description->height ? (GLint)description->height : rect->y + rect->height;
	if (corners[2] <= corners[0] || corners[3] <= corners[1])
		return;
	framebuffer = framebuffer_get(target, 0);
	post_textures_fit(smaa, description->width, description->height);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
#ifndef GPU_GL_ES
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
	glBindVertexArray(post.vertex_array);

	/* the target's pixels, read while the target is drawn into */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, post.color_framebuffer);
	glBlitFramebuffer(corners[0], corners[1], corners[2], corners[3], corners[0], corners[1], corners[2], corners[3],
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	if (!smaa)
	{
		float bounds[4];

		/* (split screen: no texel of the next window is read) */
		bounds[0] = ((float)corners[0] + 0.5f) / (float)description->width;
		bounds[1] = ((float)corners[1] + 0.5f) / (float)description->height;
		bounds[2] = ((float)corners[2] - 0.5f) / (float)description->width;
		bounds[3] = ((float)corners[3] - 0.5f) / (float)description->height;
		post_texture_bind(0, post.color);
		glUseProgram(post.programs[_post_program_fxaa]);
		glUniform4fv(post.bounds, 1, bounds);
		post_pass_draw(_post_program_fxaa, framebuffer, corners, 1);
	}
	else
	{
		post_intermediate_clear(post.edges_framebuffer);
		post_texture_bind(0, post.color);
		post_pass_draw(_post_program_smaa_edges, post.edges_framebuffer, corners, 0);

		post_intermediate_clear(post.weights_framebuffer);
		post_texture_bind(0, post.edges);
		post_texture_bind(1, post.area);
		post_texture_bind(2, post.search);
		post_pass_draw(_post_program_smaa_weights, post.weights_framebuffer, corners, 0);

		post_texture_bind(0, post.color);
		post_texture_bind(1, post.weights);
		post_pass_draw(_post_program_smaa_blend, framebuffer, corners, 1);
	}
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindVertexArray(streams.vertex_array);
	/* (the passes changed GL state behind the cache) */
	state_invalidate();
}

/* ---------- frames */

/* (no display clock: d3d8_device.c's vertical blank keeps its timer) */
static uint32_t gpu_gl_vertical_blank_wait(void)
{
	return 0;
}

static void gpu_gl_flush(void)
{
	glFlush();
}

/* display.native_hud (gpu.h): the scene's picture stretched over the
display-sized back buffer with the filter gpu_gl_present's blit uses, inside
area */
static void gpu_gl_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect);

static void gpu_gl_upscale(gpu_texture source, gpu_texture destination, const struct gpu_rect *area, uint32_t anti_alias)
{
	const struct gpu_texture_description *from = &texture_record(source)->description;
	const struct gpu_texture_description *to = &texture_record(destination)->description;

	/* (FXAA first, as its own pass, over the matching rectangle of the
	scene: a blit can't run it) */
	if (anti_alias == GPU_ANTI_ALIAS_FXAA && to->width && to->height)
	{
		struct gpu_rect rect;

		rect.x = (int32_t)((long)area->x * from->width / to->width);
		rect.y = (int32_t)((long)area->y * from->height / to->height);
		rect.width = (int32_t)(((long)area->x + area->width) * from->width / to->width) - rect.x;
		rect.height = (int32_t)(((long)area->y + area->height) * from->height / to->height) - rect.y;
		gpu_gl_anti_alias(GPU_ANTI_ALIAS_FXAA, source, &rect);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(source, 0));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_get(destination, 0));
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_SCISSOR_TEST);
	glScissor(area->x, area->y, area->width, area->height);
	glBlitFramebuffer(0, 0, (GLint)from->width, (GLint)from->height, 0, 0, (GLint)to->width, (GLint)to->height,
		GL_COLOR_BUFFER_BIT, GL_LINEAR);
	glDisable(GL_SCISSOR_TEST);
	/* the blit bypassed the cached state */
	state_invalidate();
}

static uint32_t gpu_gl_present(gpu_texture back_buffer)
{
	const struct gpu_texture_description *description = &texture_record(back_buffer)->description;
	int window_width, window_height, width, height, x, y;

	platform_video_drawable_size(&window_width, &window_height);
	/* letterbox to the back buffer's aspect ratio (a picture within a pixel
	of its place at its own size: halo_display.h) */
	{
		struct halo_display_size place = halo_display_present_size(window_width, window_height,
			(long)description->width, (long)description->height);

		width = (int)place.width;
		height = (int)place.height;
	}
	x = (window_width - width) / 2;
	y = (window_height - height) / 2;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, default_framebuffer());
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(back_buffer, 0));
	/* row 0 of the render target is the top of the picture */
	glBlitFramebuffer(0, 0, (GLint)description->width, (GLint)description->height,
		x, y + height, x + width, y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	platform_video_swap();
	/* the blit bypassed the cached state */
	state_invalidate();
	stream_frame();
	frames++;
	/* vsync paces GL, and when a frame shows isn't known */
	return 0;
}

/* gpu_present_stereo's HUD: a render target's picture over the viewport,
blended by its alpha (a blit can't blend). Row 0 of the texture is the top. */
static struct
{
	GLuint program, sampler, vertex_array;
	/* the program's source rectangle (u0, v0, u1, v1, v down) */
	GLint source;
} overlay;

static int overlay_prepare(void)
{
	static const char *const vertex_source =
#ifdef GPU_GL_ES
		"#version 300 es\n"
#else
		"#version 450 core\n"
#endif
		"uniform vec4 source;\n"
		"out vec2 uv;\n"
		"void main()\n"
		"{\n"
		"	vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
		"	uv = mix(source.xy, source.zw, vec2(corner.x, 1.0 - corner.y));\n"
		"	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char *const fragment_source =
#ifdef GPU_GL_ES
		"#version 300 es\nprecision highp float;\n"
#else
		"#version 450 core\n"
#endif
		"uniform sampler2D picture;\n"
		"in vec2 uv;\n"
		"out vec4 color;\n"
		"void main()\n"
		"{\n"
		"	color = texture(picture, uv);\n"
		"}\n";

	if (!overlay.program)
	{
		GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source, "HUD overlay vertex");
		GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source, "HUD overlay pixel");
		GLint status = 0;

		if (!vertex || !fragment)
			return 0;
		overlay.program = glCreateProgram();
		glAttachShader(overlay.program, vertex);
		glAttachShader(overlay.program, fragment);
		glLinkProgram(overlay.program);
		glGetProgramiv(overlay.program, GL_LINK_STATUS, &status);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		if (!status)
		{
			platform_log("cannot link the HUD overlay program");
			glDeleteProgram(overlay.program);
			overlay.program = 0;
			return 0;
		}
		glGenSamplers(1, &overlay.sampler);
		glSamplerParameteri(overlay.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(overlay.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(overlay.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(overlay.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenVertexArrays(1, &overlay.vertex_array);
		overlay.source = glGetUniformLocation(overlay.program, "source");
	}
	return 1;
}

/* one picture (a render target) letterboxed into the half of the window at
half_x, as gpu_gl_present does the whole; returns its rectangle */
static void present_half(gpu_texture picture, int half_x, int half_width, int window_height, int box[4])
{
	const struct gpu_texture_description *description = &texture_record(picture)->description;
	int width = half_width, height = (int)((long)half_width * description->height / description->width);

	if (height > window_height)
	{
		height = window_height;
		width = (int)((long)window_height * description->width / description->height);
	}
	box[0] = half_x + (half_width - width) / 2;
	box[1] = (window_height - height) / 2;
	box[2] = width;
	box[3] = height;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(picture, 0));
	glBlitFramebuffer(0, 0, (GLint)description->width, (GLint)description->height,
		box[0], box[1] + height, box[0] + width, box[1], GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

/* the side-by-side debug view: eye 0 in the left half of the window, eye 1 in
the right (or, zoomed, the zoomed picture in both), the HUD over each */
static uint32_t gpu_gl_present_stereo(const struct gpu_stereo_present *present)
{
	int window_width, window_height, half_width, eye, boxes[2][4];

	platform_video_drawable_size(&window_width, &window_height);
	half_width = window_width / 2;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, default_framebuffer());
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	for (eye = 0; eye < 2; eye++)
		present_half(present->eye_color[eye], eye * half_width, half_width, window_height, boxes[eye]);
	/* zoomed (halo_stereo.h): the zoomed picture in place of the eyes'
	(their pictures are last frame's), as HEAD mode's presenter's quad on
	the HUD's plane shows it to each eye's fixed frustum, with the parallax
	of its distance */
	if (present->zoom && present->zoom_tangents[0] > 0.0f && present->zoom_tangents[1] > 0.0f && overlay_prepare())
	{
		float across = present->zoom_tangents[0], up = present->zoom_tangents[1];

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, default_framebuffer());
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
		glUseProgram(overlay.program);
		glBindVertexArray(overlay.vertex_array);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, present->zoom);
		glBindSampler(0, overlay.sampler);
		for (eye = 0; eye < 2; eye++)
		{
			/* the eye sits its offset (meters) to the side: its view's edge
			at tangent t meets the quad at (offset / distance + t) of the
			quad's half tangent */
			float offset = (eye == 0 ? -HALO_STEREO_SIDE_BY_SIDE_OFFSET : HALO_STEREO_SIDE_BY_SIDE_OFFSET) * 3.048f /
				HALO_STEREO_ZOOM_DISTANCE_METERS;

			glUniform4f(overlay.source, 0.5f + (offset - HALO_STEREO_SIDE_BY_SIDE_TANGENT) / (2.0f * across),
				0.5f - HALO_STEREO_SIDE_BY_SIDE_TANGENT / (2.0f * up),
				0.5f + (offset + HALO_STEREO_SIDE_BY_SIDE_TANGENT) / (2.0f * across),
				0.5f + HALO_STEREO_SIDE_BY_SIDE_TANGENT / (2.0f * up));
			glViewport(boxes[eye][0], boxes[eye][1], boxes[eye][2], boxes[eye][3]);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glBindSampler(0, 0);
		glBindVertexArray(streams.vertex_array);
	}
	/* the widgets' dim (halo_stereo_ui_dim_active), as HEAD mode's presenter
	darkens the eyes by it: each eye's picture times 1 - ui_dim (the quad's
	own color counts for nothing) */
	if (present->ui_dim > 0.0f && overlay_prepare())
	{
		float keep = present->ui_dim < 1.0f ? 1.0f - present->ui_dim : 0.0f;

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, default_framebuffer());
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glEnable(GL_BLEND);
		glBlendColor(keep, keep, keep, 1.0f);
		glBlendFuncSeparate(GL_ZERO, GL_CONSTANT_COLOR, GL_ZERO, GL_ONE);
		glUseProgram(overlay.program);
		glUniform4f(overlay.source, 0.0f, 0.0f, 1.0f, 1.0f);
		glBindVertexArray(overlay.vertex_array);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, present->eye_color[0]);
		glBindSampler(0, overlay.sampler);
		for (eye = 0; eye < 2; eye++)
		{
			glViewport(boxes[eye][0], boxes[eye][1], boxes[eye][2], boxes[eye][3]);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glBindSampler(0, 0);
		glBindVertexArray(streams.vertex_array);
		glDisable(GL_BLEND);
	}
	/* the HUD over each half: the crosshairs' layer, each HUD group's
	target and the HUD layer itself (the catch-all), each laid out as the
	whole HUD, so the debug view shows the HUD as mono lays it out; a layer
	nothing drew this frame is 0 */
	{
		gpu_texture layers[HALO_HUD_GROUP_COUNT + 3];
		int layer_count = 0, layer;

		if (present->reticle_layer)
			layers[layer_count++] = present->reticle_layer;
		for (layer = 0; layer < HALO_HUD_GROUP_COUNT; layer++)
			if (present->hud_group[layer])
				layers[layer_count++] = present->hud_group[layer];
		if (present->hud)
			layers[layer_count++] = present->hud;
		/* the UI layer over everything, as the presenter's UI quad is */
		if (present->ui)
			layers[layer_count++] = present->ui;
		if (layer_count > 0 && overlay_prepare())
		{
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, default_framebuffer());
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_CULL_FACE);
			glDisable(GL_STENCIL_TEST);
			glEnable(GL_BLEND);
			/* premultiplied color over the picture by the layer's alpha, the
			picture's transmittance (d3d8_device.c, hud_layer_blend) */
			glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
			glUseProgram(overlay.program);
			glUniform4f(overlay.source, 0.0f, 0.0f, 1.0f, 1.0f);
			glBindVertexArray(overlay.vertex_array);
			glActiveTexture(GL_TEXTURE0);
			glBindSampler(0, overlay.sampler);
			for (layer = 0; layer < layer_count; layer++)
			{
				glBindTexture(GL_TEXTURE_2D, layers[layer]);
				for (eye = 0; eye < 2; eye++)
				{
					glViewport(boxes[eye][0], boxes[eye][1], boxes[eye][2], boxes[eye][3]);
					glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				}
			}
			glBindSampler(0, 0);
			glBindVertexArray(streams.vertex_array);
			glDisable(GL_BLEND);
		}
	}
	platform_video_swap();
	/* the blits and the overlay bypassed the cached state */
	state_invalidate();
	stream_frame();
	frames++;
	return 0;
}

static uint32_t gpu_gl_call_count_take(void)
{
	uint32_t count = (uint32_t)halo_gl_call_count;

	halo_gl_call_count = 0;
	return count;
}

/* ---------- compiling at map load: GL links its programs as it draws, and
carries no lists (gpu.h) */

static uint32_t gpu_gl_warm_list_read(const char *name, char *text, uint32_t size)
{
	(void)name;
	(void)text;
	(void)size;
	return 0;
}

static void gpu_gl_warm_begin(void)
{
}

static void gpu_gl_pipeline_warm(const struct gpu_pipeline_description *description)
{
	(void)description;
}

static void gpu_gl_warm_end(void)
{
}

static uint32_t gpu_gl_pipeline_built_take(struct gpu_pipeline_description *description)
{
	(void)description;
	return 0;
}

/* (frames here are paced by the swap interval, and nothing logs them) */
static void gpu_gl_frame_context(uint32_t scene_class, uint32_t frame_rate, const char *level)
{
	(void)scene_class;
	(void)frame_rate;
	(void)level;
}

/* ---------- the backend */

#define GPU_GL_FUNCTION(type, name, parameters, arguments) .name = gpu_gl_##name,
#define GPU_GL_PROCEDURE(name, parameters, arguments) .name = gpu_gl_##name,
const struct gpu_backend gpu_backend_gl = { GPU_FUNCTIONS(GPU_GL_FUNCTION, GPU_GL_PROCEDURE) };
#undef GPU_GL_FUNCTION
#undef GPU_GL_PROCEDURE

#ifndef GPU_GL_HOST
/* gpu.h's entry points, where GL is the only backend (the iOS host
dispatches between backends: port/ios/host/host_gpu_dispatch.c) */
#define GPU_GL_FUNCTION(type, name, parameters, arguments) type gpu_##name parameters { return gpu_gl_##name arguments; }
#define GPU_GL_PROCEDURE(name, parameters, arguments) void gpu_##name parameters { gpu_gl_##name arguments; }
GPU_FUNCTIONS(GPU_GL_FUNCTION, GPU_GL_PROCEDURE)
#undef GPU_GL_FUNCTION
#undef GPU_GL_PROCEDURE
#endif
