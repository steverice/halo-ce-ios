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

GLenum gpu_gl_texture_target(uint32_t type)
{
	return type == GPU_TEXTURE_CUBE ? GL_TEXTURE_CUBE_MAP : type == GPU_TEXTURE_3D ? GL_TEXTURE_3D : GL_TEXTURE_2D;
}

gpu_texture gpu_texture_create(const struct gpu_texture_description *description)
{
	struct texture_record *record;
	GLuint name = 0;
	uint32_t level;

	glGenTextures(1, &name);
	record = texture_record(name);
	record->description = *description;
	record->target = gpu_gl_texture_target(description->type);
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
		xgpu_gl_bind_array_buffer(streams.stream_buffer);
		glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		streams.stream_offset = 0;
	}
	/* step 3's front end passes 0: today the index buffer makes room as each
	range is uploaded (gpu_stream), which keeps the GL call order; a backend
	that needs a draw's index bytes up front gets them from the draw packet
	(sub-step e) */
	if (index_bytes && streams.index_offset + index_bytes > INDEX_BUFFER_SIZE)
	{
		xgpu_gl_bind_element_array_buffer(streams.index_buffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		streams.index_offset = 0;
	}
}

uint32_t gpu_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	unsigned long offset;

	size = (size + 15) & ~15U;
	if (kind == GPU_STREAM_INDEX)
	{
		/* the index buffer makes room as each range comes */
		xgpu_gl_bind_element_array_buffer(streams.index_buffer);
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
	xgpu_gl_bind_array_buffer(streams.stream_buffer);
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

struct gpu_gl_program *gpu_gl_program_get(gpu_shader vertex_shader, gpu_shader fragment_shader)
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
	xgpu_gl_use_program(entry->program);
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

void gpu_gl_program_use(struct gpu_gl_program *program)
{
	xgpu_gl_use_program(program->program);
}

void gpu_gl_program_constants(struct gpu_gl_program *entry, const struct gpu_constant_store *store)
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

void gpu_gl_program_uniforms(struct gpu_gl_program *entry, const struct gpu_uniforms *uniforms)
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
