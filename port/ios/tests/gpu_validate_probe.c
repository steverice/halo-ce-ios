/*
GPU_VALIDATE_PROBE.C

Render validation (port/ios/host/gpu_validate.c) fed deliberately bad calls
through a backend that draws nothing: each must produce its named reason,
and a good draw none. Built with HALO_RENDER_VALIDATION=1 (tools/ios_test.py).
*/

#include "gpu_validate.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- a backend that hands out handles and draws nothing */

static uint32_t next_handle, draws, draw_result = 1, visibility_ends, report_in_draw = GPU_VALIDATION_REASON_COUNT;
static unsigned char stream_memory[1 << 16];
static uint32_t stream_offset;
static uint8_t mock_astc = 1;

static gpu_texture mock_texture_create(const struct gpu_texture_description *description) { (void)description; return ++next_handle; }
static void mock_texture_upload(gpu_texture t, uint32_t f, uint32_t l, const void *d, uint32_t s) { (void)t; (void)f; (void)l; (void)d; (void)s; }
static void mock_texture_copy_level(gpu_texture s, gpu_texture d, uint32_t l) { (void)s; (void)d; (void)l; }
static void mock_texture_copy_rects(gpu_texture s, const struct gpu_texture_copy *c, uint32_t n) { (void)s; (void)c; (void)n; }
static void mock_texture_generate_mipmaps(gpu_texture t, uint32_t b) { (void)t; (void)b; }
static void mock_texture_destroy(gpu_texture t) { (void)t; }
static uint32_t mock_texture_read(gpu_texture t, void *p, uint32_t s) { (void)t; (void)p; (void)s; return 0; }
static void mock_texture_set_content(gpu_texture t, uint32_t w, uint32_t h) { (void)t; (void)w; (void)h; }
static gpu_buffer mock_buffer_create(uint32_t size) { (void)size; return ++next_handle; }
static void mock_buffer_write(gpu_buffer b, uint32_t o, uint32_t s, const void *d, uint32_t f) { (void)b; (void)o; (void)s; (void)d; (void)f; }
static void mock_stream_reserve(uint32_t v, uint32_t i) { (void)v; (void)i; }
static uint32_t mock_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	uint32_t offset = stream_offset;

	(void)kind;
	memcpy(stream_memory + offset, data, size);
	stream_offset += (size + 15) & ~15u;
	*buffer = 9000;
	return offset;
}
static gpu_shader mock_shader_create(uint32_t stage, const char *source) { (void)stage; return strstr(source, "broken") ? 0 : ++next_handle; }
static void mock_clear(const struct gpu_clear *c, const struct gpu_rect *r, uint32_t n) { (void)c; (void)r; (void)n; }
static uint32_t mock_draw(const struct gpu_draw *d, const struct gpu_constant_store *c, const struct gpu_uniforms *u)
{
	(void)d; (void)c; (void)u;
	draws++;
	if (report_in_draw != GPU_VALIDATION_REASON_COUNT)
		GPU_VALIDATION_REPORT(report_in_draw, 0, "the backend's reason");
	return draw_result;
}
static void mock_visibility_begin(void) {}
static void mock_visibility_end(uint32_t slot) { (void)slot; visibility_ends++; }
static uint32_t mock_visibility_result(uint32_t slot, uint32_t *samples) { (void)slot; *samples = 1; return 1; }
static void mock_flush(void) {}
static uint32_t mock_vertical_blank_wait(void) { return 0; }
static void mock_frame_start(void) {}
static uint32_t mock_present(gpu_texture b) { (void)b; stream_offset = 0; return 0; }
static uint32_t mock_present_stereo(const struct gpu_stereo_present *p) { (void)p; stream_offset = 0; return 0; }
static uint32_t mock_call_count_take(void) { return 0; }
static uint32_t mock_warm_list_read(const char *n, char *t, uint32_t s) { (void)n; (void)t; (void)s; return 0; }
static void mock_warm_begin(void) {}
static void mock_pipeline_warm(const struct gpu_pipeline_description *d) { (void)d; }
static void mock_warm_end(void) {}
static uint32_t mock_pipeline_built_take(struct gpu_pipeline_description *d) { (void)d; return 0; }
static void mock_upscale(gpu_texture s, gpu_texture d, const struct gpu_rect *a, uint32_t aa) { (void)s; (void)d; (void)a; (void)aa; }
static uint32_t mock_anti_alias_prepare(uint32_t m, const char *s, const void *a, const void *t) { (void)m; (void)s; (void)a; (void)t; return 0; }
static void mock_anti_alias(uint32_t m, gpu_texture t, const struct gpu_rect *r) { (void)m; (void)t; (void)r; }
static void mock_frame_context(uint32_t c, uint32_t r, const char *l) { (void)c; (void)r; (void)l; }
static uint32_t mock_companion_size(uint32_t *width, uint32_t *height) { *width = *height = 0; return 0; }
static void mock_companion_set(gpu_texture target) { (void)target; }
static void mock_compile_progress(uint32_t action, struct gpu_compile_progress *progress)
{
	(void)action;
	memset(progress, 0, sizeof(*progress));
	progress->sealed = 1;
}
static uint32_t mock_compiles_outstanding(void) { return 0; }
static void mock_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	(void)flags;
	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->vertex_bgra = capabilities->base_vertex = capabilities->border_clamp = capabilities->s3tc = 1;
	capabilities->astc = mock_astc;
	capabilities->max_texture_size = 4096;
}

#define MOCK_FUNCTION(type, name, parameters, arguments) .name = mock_##name,
#define MOCK_PROCEDURE(name, parameters, arguments) .name = mock_##name,
static const struct gpu_backend mock = { GPU_FUNCTIONS(MOCK_FUNCTION, MOCK_PROCEDURE) };

/* ---------- the probe */

static struct gpu_backend gpu;
static char last_line[1024], all_lines[1 << 16];
static jmp_buf stopped;
static int stops;

static void log_line(const char *line)
{
	snprintf(last_line, sizeof(last_line), "%s", line);
	if (strlen(all_lines) + strlen(line) + 2 < sizeof(all_lines))
	{
		strcat(all_lines, line);
		strcat(all_lines, "\n");
	}
}

static void stop_line(const char *line)
{
	log_line(line);
	stops++;
	longjmp(stopped, 1);
}

static void install(int mode, const char *allow)
{
	struct gpu_validation_host host = { log_line, stop_line, mode, allow };
	struct gpu_capabilities capabilities;

	gpu = mock;
	next_handle = draws = stream_offset = visibility_ends = 0;
	draw_result = 1;
	report_in_draw = GPU_VALIDATION_REASON_COUNT;
	all_lines[0] = 0;
	gpu_validation_install(&gpu, &host);
	gpu.initialize(0, &capabilities);
}

static int failures;

static unsigned long findings(void)
{
	unsigned long total = 0;
	uint32_t reason;

	for (reason = 0; reason < GPU_VALIDATION_REASON_COUNT; reason++)
		total += gpu_validation_count(reason);
	return total;
}

#define EXPECT(condition, what) \
	do { if (!(condition)) { failures++; printf("FAILED %s:%d: %s\n  last line: %s\n", __FILE__, __LINE__, what, last_line); } } while (0)
/* exactly one finding, of this reason */
#define EXPECT_ONLY(reason, what) \
	EXPECT(gpu_validation_count(GPU_VALIDATION_##reason) == 1 && findings() == 1 && \
		strstr(last_line, gpu_validation_reason_name(GPU_VALIDATION_##reason)), what)

static const char vertex_source[] =
	"uniform vec4 c[192];\nlayout(location = 0) in vec4 v0_in;\nlayout(location = 2) in vec4 v2_in;\n"
	"void main()\n{\n\tvec4 v0 = v0_in;\n\tvec4 v2 = v2_in;\n\tvec4 v9 = v9_in;\n\tR0 = v0.xyzw * c[4] + v2.xyzw * c[7];\n}\n";
static const char pixel_source[] =
	"uniform sampler2D tex0;\nuniform samplerCube tex1;\nuniform sampler2D tex2;\nuniform sampler2D tex3;\n"
	"void main()\n{\n\tt0 = texture(tex0, (xT0).xy * texture_scale[0].xy, texture_lod_bias[0]);\n"
	"\tt1 = texture(tex1, (xT1).xyz, texture_lod_bias[1]);\n}\n";

/* a scene's worth of good state */
static struct
{
	gpu_texture color, depth, small_depth, texture, cube, mipped, volume;
	gpu_shader vertex, pixel;
	gpu_buffer mirror;
	struct gpu_constant_store constants;
	struct gpu_uniforms uniforms;
	/* the "game's memory" a mirror segment uploads from: two pages */
	float memory[2048];
	uint16_t indices[6];
	float vertices[4][8];
} scene;

static gpu_texture texture_make(uint8_t type, uint8_t format, uint8_t usage, uint32_t width, uint32_t height, uint32_t levels,
	uint32_t uploaded_levels)
{
	struct gpu_texture_description description = { type, format, usage, 0, width, height, type == GPU_TEXTURE_3D ? 4 : 1, levels };
	static unsigned char texels[64 * 64 * 4 * 4];
	gpu_texture handle = gpu.texture_create(&description);
	uint32_t face, level;

	for (face = 0; face < (type == GPU_TEXTURE_CUBE ? 6u : 1u) && usage == GPU_USAGE_UPLOAD; face++)
		for (level = 0; level < uploaded_levels; level++)
			gpu.texture_upload(handle, face, level, texels, sizeof(texels));
	return handle;
}

static void scene_make(int mode, const char *allow)
{
	int index;

	install(mode, allow);
	memset(&scene, 0, sizeof(scene));
	scene.color = texture_make(GPU_TEXTURE_2D, GPU_FORMAT_BGRA8, GPU_USAGE_RENDER_TARGET, 640, 480, 1, 0);
	scene.depth = texture_make(GPU_TEXTURE_2D, GPU_FORMAT_DEPTH_STENCIL, GPU_USAGE_RENDER_TARGET, 640, 480, 1, 0);
	scene.small_depth = texture_make(GPU_TEXTURE_2D, GPU_FORMAT_DEPTH_STENCIL, GPU_USAGE_RENDER_TARGET, 320, 240, 1, 0);
	scene.texture = texture_make(GPU_TEXTURE_2D, GPU_FORMAT_BGRA8, GPU_USAGE_UPLOAD, 64, 64, 1, 1);
	scene.cube = texture_make(GPU_TEXTURE_CUBE, GPU_FORMAT_BGRA8, GPU_USAGE_UPLOAD, 32, 32, 1, 1);
	scene.mipped = texture_make(GPU_TEXTURE_2D, GPU_FORMAT_BGRA8, GPU_USAGE_UPLOAD, 64, 64, 7, 3);
	scene.volume = texture_make(GPU_TEXTURE_3D, GPU_FORMAT_BGRA8, GPU_USAGE_UPLOAD, 16, 16, 1, 1);
	scene.vertex = gpu.shader_create(GPU_SHADER_VERTEX, vertex_source);
	scene.pixel = gpu.shader_create(GPU_SHADER_PIXEL, pixel_source);
	scene.mirror = gpu.buffer_create(0x400000);
	for (index = 0; index < 2048; index++)
		scene.memory[index] = (float)index;
	/* the segment's first page alone is uploaded */
	gpu.buffer_write(scene.mirror, 0, 4096, scene.memory, GPU_WRITE_UNUSED);
	scene.indices[0] = 0; scene.indices[1] = 1; scene.indices[2] = 2;
	scene.indices[3] = 2; scene.indices[4] = 1; scene.indices[5] = 3;
	for (index = 0; index < 4 * 8; index++)
		scene.vertices[index / 8][index % 8] = (float)index * 0.25f;
	scene.constants.serial = 1;
	scene.uniforms.serial = 1;
	scene.uniforms.point_size[0][0] = 1.0f;
	EXPECT(findings() == 0, "the scene's resources are good");
}

/* an indexed draw of the streamed quad: stream 0 holds attributes 0 and 2 */
static void draw_make(struct gpu_draw *draw)
{
	int index;

	memset(draw, 0, sizeof(*draw));
	draw->color_target = scene.color;
	draw->depth_target = scene.depth;
	draw->vertex_shader = scene.vertex;
	draw->pixel_shader = scene.pixel;
	draw->viewport = (struct gpu_viewport){ 0, 0, 640, 480, 0.0f, 1.0f };
	draw->scissor = (struct gpu_rect){ 0, 0, 640, 480 };
	draw->blend.color_write_mask = 15;
	draw->stages[0].texture = scene.texture;
	draw->stages[0].type = GPU_TEXTURE_2D;
	draw->stages[0].sampler.min_filter = draw->stages[0].sampler.mag_filter = GPU_FILTER_LINEAR;
	draw->stages[1].texture = scene.cube;
	draw->stages[1].type = GPU_TEXTURE_CUBE;
	draw->stages[1].sampler.min_filter = draw->stages[1].sampler.mag_filter = GPU_FILTER_LINEAR;
	for (index = 0; index < GPU_ATTRIBUTE_COUNT; index++)
	{
		draw->attributes[index].format = GPU_ATTRIBUTE_FLOAT4;
		draw->attributes[index].stream = GPU_STREAM_CONSTANT;
	}
	draw->streams[0].offset = gpu.stream(GPU_STREAM_KIND_VERTEX, scene.vertices, sizeof(scene.vertices), &draw->streams[0].buffer);
	draw->streams[0].stride = 32;
	draw->attributes[0] = (struct gpu_vertex_attribute){ GPU_ATTRIBUTE_FLOAT4, 0, 0 };
	draw->attributes[2] = (struct gpu_vertex_attribute){ GPU_ATTRIBUTE_FLOAT4, 0, 16 };
	draw->index_offset = gpu.stream(GPU_STREAM_KIND_INDEX, scene.indices, sizeof(scene.indices), &draw->index_buffer);
	draw->primitive = GPU_PRIMITIVE_TRIANGLES;
	draw->count = 6;
}

static void submit(struct gpu_draw *draw)
{
	gpu.draw(draw, &scene.constants, &scene.uniforms);
}

int main(void)
{
	struct gpu_draw draw;
	float nan_value = nanf(""), infinity = INFINITY;

	/* a good draw: nothing, and the backend drew it */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	submit(&draw);
	EXPECT(findings() == 0 && draws == 1, "a good draw has no findings and reaches the backend");

	/* an index past the end of what was streamed */
	scene_make(GPU_VALIDATION_LOG, NULL);
	scene.indices[4] = 4;
	draw_make(&draw);
	submit(&draw);
	EXPECT_ONLY(INDEX_OUT_OF_RANGE, "an index past the streamed vertices");
	EXPECT(strstr(last_line, "frame 0 pass 1 draw 1") != NULL, "the finding names its frame, pass and draw");
	EXPECT(draws == 1, "log mode still draws");
	/* the same again is counted, not logged again */
	last_line[0] = 0;
	draw_make(&draw);
	submit(&draw);
	EXPECT(gpu_validation_count(GPU_VALIDATION_INDEX_OUT_OF_RANGE) == 2 && !last_line[0], "a repeated cause is counted, not logged");

	/* a base vertex that takes the smallest index below vertex 0 */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.base_vertex = -1;
	submit(&draw);
	EXPECT_ONLY(INDEX_BELOW_ZERO, "a base vertex below the smallest index");

	/* more indices than were streamed */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.count = 9;
	submit(&draw);
	EXPECT_ONLY(INDEX_RANGE_OUT_OF_RANGE, "an index range past its streamed range");

	/* a vertex range the mirror never uploaded: the second page */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.index_buffer = 0;
	draw.streams[0].buffer = scene.mirror;
	draw.streams[0].offset = 4096 - 64;
	draw.count = 3;
	submit(&draw);
	EXPECT_ONLY(VERTEX_RANGE_OUT_OF_RANGE, "vertices on a page the mirror never uploaded");
	/* the same range within the uploaded page is good, and read in place */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.index_buffer = 0;
	draw.streams[0].buffer = scene.mirror;
	draw.streams[0].offset = 4096 - 96;
	draw.count = 3;
	submit(&draw);
	EXPECT(findings() == 0, "the mirror's uploaded page is good");
	/* a NaN the game writes there afterwards is seen once the page is uploaded again, not before: the
	clean scan is kept until then */
	scene.memory[1000] = nan_value;
	submit(&draw);
	EXPECT(findings() == 0, "a clean mirrored range is not scanned again until it is uploaded again");
	gpu.buffer_write(scene.mirror, 0, 4096, scene.memory, 0);
	submit(&draw);
	EXPECT_ONLY(VERTEX_NONFINITE, "a NaN in a mirrored vertex, read from the game's memory");

	/* a stream from an earlier frame */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	gpu.present(scene.color);
	submit(&draw);
	EXPECT(gpu_validation_count(GPU_VALIDATION_STREAM_STALE) >= 1 && findings() == gpu_validation_count(GPU_VALIDATION_STREAM_STALE),
		"a draw that reads the last frame's streams");

	/* numbers: a NaN and an infinity in a streamed vertex, a huge value, a denormal */
	scene_make(GPU_VALIDATION_LOG, NULL);
	scene.vertices[1][2] = nan_value;
	draw_make(&draw);
	submit(&draw);
	EXPECT_ONLY(VERTEX_NONFINITE, "a NaN in a vertex attribute the shader reads");
	scene_make(GPU_VALIDATION_LOG, NULL);
	scene.vertices[3][5] = infinity;
	draw_make(&draw);
	submit(&draw);
	EXPECT_ONLY(VERTEX_NONFINITE, "an infinity in a vertex attribute the shader reads");
	scene_make(GPU_VALIDATION_LOG, NULL);
	scene.vertices[0][0] = 3.0e20f;
	scene.vertices[0][1] = 1.0e-40f;
	draw_make(&draw);
	submit(&draw);
	EXPECT(gpu_validation_count(GPU_VALIDATION_VERTEX_HUGE) == 1 && gpu_validation_count(GPU_VALIDATION_VERTEX_DENORMAL) == 1 &&
		findings() == 2, "a huge magnitude and a denormal are warnings");

	/* a NaN in a uniform, and in a constant the shader reads; one in a register it doesn't read is left alone */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	scene.uniforms.fog_parameters[0][1] = nan_value;
	scene.uniforms.serial++;
	submit(&draw);
	EXPECT_ONLY(UNIFORM_NONFINITE, "a NaN in a uniform");
	EXPECT(strstr(last_line, "fog_parameters[0].y") != NULL, "the finding names the uniform");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	scene.constants.c[100][0] = nan_value;
	scene.constants.log[++scene.constants.serial % GPU_CONSTANT_LOG_SIZE] = 100;
	submit(&draw);
	EXPECT(findings() == 0, "a NaN in a constant register the shader does not read");
	scene.constants.c[7][3] = nan_value;
	scene.constants.log[++scene.constants.serial % GPU_CONSTANT_LOG_SIZE] = 7;
	draw_make(&draw);
	submit(&draw);
	EXPECT_ONLY(CONSTANT_NONFINITE, "a NaN in a constant register the shader reads");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.constant_values[9][1] = nan_value;
	submit(&draw);
	EXPECT(findings() == 0, "a NaN in the constant value of an attribute the shader does not read");

	/* samplers and textures */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	memset(&draw.stages[1], 0, sizeof(draw.stages[1]));
	submit(&draw);
	EXPECT_ONLY(SAMPLER_UNBOUND, "a stage the shader samples with no texture");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.stages[1].texture = scene.texture;
	draw.stages[1].type = GPU_TEXTURE_2D;
	submit(&draw);
	EXPECT_ONLY(TEXTURE_TYPE_MISMATCH, "a 2D texture where the shader samples a cube");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.stages[0].texture = scene.mipped;
	draw.stages[0].sampler.mip_filter = GPU_FILTER_LINEAR;
	submit(&draw);
	EXPECT_ONLY(TEXTURE_INCOMPLETE_MIPS, "a mipmapped lookup of a texture with levels missing");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.stages[0].texture = scene.color;
	submit(&draw);
	EXPECT_ONLY(TEXTURE_IS_TARGET, "a stage that samples the draw's own color target");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	gpu.texture_destroy(scene.texture);
	submit(&draw);
	EXPECT_ONLY(TEXTURE_UNKNOWN, "a stage whose texture was destroyed");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.stages[0].sampler.mag_filter = GPU_FILTER_GAUSSIAN_CUBIC;
	submit(&draw);
	EXPECT_ONLY(FILTER_UNSUPPORTED, "a gaussian filter, which is sampled as linear");
	/* a stage the shader doesn't sample is not looked at */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.stages[3].texture = 77777;
	draw.stages[3].type = GPU_TEXTURE_3D;
	submit(&draw);
	EXPECT(findings() == 0, "a bad stage the shader does not sample");
	scene_make(GPU_VALIDATION_LOG, NULL);
	{
		struct gpu_texture_description description = { GPU_TEXTURE_2D, 9, GPU_USAGE_UPLOAD, 0, 8, 8, 1, 1 };

		gpu.texture_create(&description);
		EXPECT_ONLY(TEXTURE_FORMAT_UNSUPPORTED, "a texture format the backends do not take");
	}

	/* ASTC 4x4 has one 16-byte block even at the smallest mip. */
	scene_make(GPU_VALIDATION_LOG, NULL);
	{
		struct gpu_texture_description description = { GPU_TEXTURE_2D, GPU_FORMAT_ASTC4X4, GPU_USAGE_UPLOAD, 0, 1, 1, 1, 1 };
		unsigned char block[16] = { 0 };
		gpu_texture texture = gpu.texture_create(&description);

		gpu.texture_upload(texture, 0, 0, block, sizeof(block));
		EXPECT(findings() == 0, "a complete ASTC block uploads at 1x1");
	}

	scene_make(GPU_VALIDATION_LOG, NULL);
	{
		struct gpu_texture_description description = { GPU_TEXTURE_CUBE, GPU_FORMAT_ASTC4X4, GPU_USAGE_UPLOAD, 0, 8, 8, 1, 4 };
		unsigned char blocks[64] = { 0 };
		gpu_texture texture = gpu.texture_create(&description);
		unsigned face, level;

		for (face = 0; face < 6; face++)
			for (level = 0; level < 4; level++)
				gpu.texture_upload(texture, face, level, blocks, level ? 16 : 64);
		EXPECT(findings() == 0, "ASTC cube faces and the full mip chain use block sizing");
		gpu.texture_upload(texture, 5, 3, blocks, 15);
		EXPECT_ONLY(UPLOAD_SHORT, "the 1x1 ASTC mip still needs 16 bytes");
	}
	scene_make(GPU_VALIDATION_LOG, NULL);
	texture_make(GPU_TEXTURE_2D, GPU_FORMAT_ASTC4X4, GPU_USAGE_RENDER_TARGET, 8, 8, 1, 0);
	EXPECT_ONLY(TEXTURE_FORMAT_UNSUPPORTED, "ASTC cannot be a render target");
	scene_make(GPU_VALIDATION_LOG, NULL);
	texture_make(GPU_TEXTURE_3D, GPU_FORMAT_ASTC4X4, GPU_USAGE_UPLOAD, 8, 8, 1, 0);
	EXPECT_ONLY(TEXTURE_FORMAT_UNSUPPORTED, "ASTC 3D is excluded");
	mock_astc = 0;
	scene_make(GPU_VALIDATION_LOG, NULL);
	texture_make(GPU_TEXTURE_2D, GPU_FORMAT_ASTC4X4, GPU_USAGE_UPLOAD, 8, 8, 1, 0);
	EXPECT_ONLY(TEXTURE_FORMAT_UNSUPPORTED, "ASTC needs the backend capability");
	mock_astc = 1;

	/* targets */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.depth_target = scene.small_depth;
	submit(&draw);
	EXPECT_ONLY(DEPTH_TARGET_SMALLER, "a depth target smaller than the color target");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.depth_target = scene.color;
	submit(&draw);
	EXPECT_ONLY(TARGET_FORMAT, "a color texture as the depth target");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.color_target = scene.texture;
	submit(&draw);
	EXPECT(gpu_validation_count(GPU_VALIDATION_TARGET_NOT_RENDER_TARGET) == 1, "an upload texture as the color target");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.depth_target = 0;
	draw.depth_stencil.depth_test = 1;
	submit(&draw);
	EXPECT_ONLY(DEPTH_STATE_WITHOUT_TARGET, "a depth test without a depth target");

	/* viewport, scissor and state */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.viewport.width = 800;
	draw.scissor.width = 800;
	submit(&draw);
	EXPECT(gpu_validation_count(GPU_VALIDATION_VIEWPORT_OUTSIDE_TARGET) == 1 &&
		gpu_validation_count(GPU_VALIDATION_SCISSOR_OUTSIDE_TARGET) == 1 && findings() == 2, "a viewport and scissor wider than the target");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.viewport.max_z = nan_value;
	submit(&draw);
	EXPECT_ONLY(VIEWPORT_DEPTH_RANGE, "a NaN in the viewport's depth range");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.raster.fill_mode = GPU_FILL_LINE;
	submit(&draw);
	EXPECT_ONLY(FILL_MODE_IGNORED, "wireframe, which is drawn filled");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.blend.source = 200;
	submit(&draw);
	EXPECT_ONLY(STATE_INVALID, "a blend factor outside the enum");

	/* shaders: none, and one the backend could not compile */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw.pixel_shader = 0;
	draw_result = 0;
	submit(&draw);
	EXPECT_ONLY(SHADER_MISSING, "a draw without a pixel shader");
	scene_make(GPU_VALIDATION_LOG, NULL);
	EXPECT(gpu.shader_create(GPU_SHADER_PIXEL, "broken") == 0, "the backend refuses the shader");
	EXPECT_ONLY(SHADER_COMPILE_FAILED, "a shader the backend could not compile");

	/* a draw the backend skips: with its reason, and without one */
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw_result = 0;
	report_in_draw = GPU_VALIDATION_PIPELINE_COMPILING;
	submit(&draw);
	EXPECT_ONLY(PIPELINE_COMPILING, "a draw skipped while its pipeline compiles");
	EXPECT(strstr(last_line, "expected") != NULL, "which is reported as expected, not as a failure");
	scene_make(GPU_VALIDATION_LOG, NULL);
	draw_make(&draw);
	draw_result = 0;
	submit(&draw);
	EXPECT_ONLY(DRAW_SKIPPED_UNEXPLAINED, "a draw the backend skips without saying why");

	/* clears and visibility tests */
	scene_make(GPU_VALIDATION_LOG, NULL);
	{
		struct gpu_clear clear = { scene.color, 0, GPU_CLEAR_COLOR | GPU_CLEAR_DEPTH, 15, 0, 1.0f, 0 };
		struct gpu_rect rect = { 0, 0, 640, 480 };

		gpu.clear(&clear, &rect, 1);
		EXPECT_ONLY(CLEAR_DEPTH_WITHOUT_TARGET, "a depth clear without a depth target");
	}
	scene_make(GPU_VALIDATION_LOG, NULL);
	gpu.visibility_begin();
	gpu.visibility_end(GPU_VISIBILITY_SLOTS);
	EXPECT_ONLY(VISIBILITY_INVALID, "a visibility slot past the last");
	EXPECT(visibility_ends == 0, "which is not passed on to the backend");

	/* the front end's identity is in the line, and its own notes are counted */
	scene_make(GPU_VALIDATION_LOG, NULL);
	scene.indices[0] = 9;
	draw_make(&draw);
	{
		struct gpu_validation_identity identity = { GPU_VALIDATION_DRAW_INDEXED, 59, 0, 0x6c143490, 0x80123456, 0 };

		gpu_validation_identity(&identity);
	}
	submit(&draw);
	EXPECT(strstr(last_line, "indexed, vs059_0 ps_6c143490, from 0x80123456") != NULL, "the draw's identity from the front end");
	scene_make(GPU_VALIDATION_LOG, NULL);
	gpu_validation_note(GPU_VALIDATION_FRONT_NO_PROGRAM, 0, 0);
	EXPECT_ONLY(FRONT_NO_PROGRAM, "the front end's skipped draw");

	/* the summary lists each cause with its count */
	scene_make(GPU_VALIDATION_LOG, NULL);
	scene.indices[4] = 4;
	draw_make(&draw);
	submit(&draw);
	draw_make(&draw);
	submit(&draw);
	all_lines[0] = 0;
	gpu_validation_summary();
	EXPECT(strstr(all_lines, "1 failures") == NULL && strstr(all_lines, "2 failures, 0 fallbacks") != NULL &&
		strstr(all_lines, "2 x FAILURE index-out-of-range, first at frame 0") != NULL, "the summary counts each cause");

	/* strict mode: the first failure stops the run, before the backend draws; a warning or an expected
	skip does not; an allowed reason does not */
	scene_make(GPU_VALIDATION_STRICT, NULL);
	scene.indices[4] = 4;
	draw_make(&draw);
	stops = 0;
	if (!setjmp(stopped))
		submit(&draw);
	EXPECT(stops == 1 && draws == 0 && strstr(last_line, "strict mode stops the run at its first failure: index-out-of-range"),
		"strict mode stops at the first failure");
	scene_make(GPU_VALIDATION_STRICT, NULL);
	draw_make(&draw);
	draw.viewport.width = 800;
	draw_result = 0;
	report_in_draw = GPU_VALIDATION_PIPELINE_COMPILING;
	stops = 0;
	if (!setjmp(stopped))
		submit(&draw);
	EXPECT(stops == 0 && draws == 1, "strict mode goes on past a warning and an expected skip");
	scene_make(GPU_VALIDATION_STRICT, "viewport-empty, fill-mode-ignored");
	draw_make(&draw);
	draw.raster.fill_mode = GPU_FILL_LINE;
	stops = 0;
	if (!setjmp(stopped))
		submit(&draw);
	EXPECT(stops == 0 && draws == 1 && gpu_validation_count(GPU_VALIDATION_FILL_MODE_IGNORED) == 1,
		"strict mode goes on past a reason it was told to allow");

	/* off: the table is the backend's own */
	gpu = mock;
	{
		struct gpu_validation_host host = { log_line, stop_line, GPU_VALIDATION_OFF, NULL };

		gpu_validation_install(&gpu, &host);
		EXPECT(gpu.draw == mock_draw && gpu.present == mock_present, "mode off leaves the backend's table alone");
	}

	if (failures)
	{
		printf("gpu validate probe: %d checks failed\n", failures);
		return 1;
	}
	printf("gpu validate probe: all checks passed\n");
	return 0;
}
