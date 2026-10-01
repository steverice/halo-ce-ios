/*
GPU_GL.C

The OpenGL and OpenGL ES backend of gpu.h. Step 3 of the renderer split moves
the GL calls of d3d8_gl.c and xbox_textures.c here one group at a time;
for now it probes the context.
*/

#include "xgpu.h"
#include "port_config.h"

#include <stdlib.h>
#include <string.h>

#ifdef HALO_ILP32
/* OpenGL ES 3 has no BGRA upload format; d3d8_gl.c defines the same alias */
#define GL_BGRA GL_RGBA
#endif

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
