/*
GPU_REPLAY_PROBE.C

Capture and replay (port/ios/host/gpu_capture.c, gpu_replay.c) without a GPU:
a scripted frame goes through the capture layer into a backend that only
notes its calls, the file is replayed into the same kind of backend, and the
two notes must agree; then the file is damaged in every way a file can be,
and the fuzzer's mutations are played through the validator
(gpu_validate.c), which must refuse each one. Built with
HALO_RENDER_VALIDATION=1 under the address sanitizer (tools/ios_test.py).
*/

#include "gpu_capture.h"
#include "gpu_replay_fuzz.h"
#include "gpu_validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

/* ---------- a backend that notes each call: its name and a hash of what
it was given, with handles and stream offsets left out (a replay's differ) */

static char notes[1 << 16];
static uint32_t next_handle, stream_offset, call_count, stream_base;

static uint32_t hash_bytes(const void *bytes, size_t size)
{
	const unsigned char *at = bytes;
	uint32_t hash = 2166136261u;

	while (size--)
		hash = (hash ^ *at++) * 16777619u;
	return hash;
}

static void note(const char *name, uint32_t a, uint32_t b)
{
	size_t length = strlen(notes);

	call_count++;
	if (length + 64 < sizeof(notes))
		snprintf(notes + length, sizeof(notes) - length, "%s %08x %08x\n", name, a, b);
}

static gpu_texture mock_texture_create(const struct gpu_texture_description *d) { note("texture_create", hash_bytes(d, sizeof(*d)), 0); return next_handle += 3; }
static void mock_texture_upload(gpu_texture t, uint32_t f, uint32_t l, const void *d, uint32_t s) { (void)t; note("texture_upload", f << 8 | l, hash_bytes(d, s)); }
static void mock_texture_copy_level(gpu_texture s, gpu_texture d, uint32_t l) { (void)s; (void)d; note("copy_level", l, 0); }
static void mock_texture_copy_rects(gpu_texture s, const struct gpu_texture_copy *c, uint32_t n) { (void)s; note("copy_rects", n, hash_bytes(c, n * sizeof(*c))); }
static void mock_texture_generate_mipmaps(gpu_texture t, uint32_t b) { (void)t; note("mipmaps", b, 0); }
static void mock_texture_destroy(gpu_texture t) { (void)t; note("texture_destroy", 0, 0); }
static uint32_t mock_texture_read(gpu_texture t, void *p, uint32_t s) { (void)t; (void)p; (void)s; return 0; }
static void mock_texture_set_content(gpu_texture t, uint32_t w, uint32_t h) { (void)t; (void)w; (void)h; }
static gpu_buffer mock_buffer_create(uint32_t size) { note("buffer_create", size, 0); return next_handle += 3; }
static void mock_buffer_write(gpu_buffer b, uint32_t o, uint32_t s, const void *d, uint32_t f) { (void)b; (void)f; note("buffer_write", o, hash_bytes(d, s)); }
static void mock_stream_reserve(uint32_t v, uint32_t i) { note("stream_reserve", v, i); }
static uint32_t mock_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	uint32_t offset = stream_base + stream_offset;

	note("stream", kind, hash_bytes(data, size));
	stream_offset += (size + 15) & ~15u;
	*buffer = 900000 + stream_base;
	return offset;
}
static gpu_shader mock_shader_create(uint32_t stage, const char *source) { note("shader_create", stage, hash_bytes(source, strlen(source))); return next_handle += 3; }
static void mock_clear(const struct gpu_clear *c, const struct gpu_rect *r, uint32_t n) { note("clear", c->flags, hash_bytes(r, n * sizeof(*r))); }
static uint32_t mock_draw(const struct gpu_draw *d, const struct gpu_constant_store *c, const struct gpu_uniforms *u)
{
	/* the state, the constants and the uniforms' values; not the handles */
	note("draw", hash_bytes(&d->viewport, offsetof(struct gpu_draw, stages) - offsetof(struct gpu_draw, viewport)) ^ d->count ^
		d->primitive << 8 ^ (uint32_t)d->base_vertex << 12, hash_bytes(c->c, sizeof(c->c)) ^
		hash_bytes(u, offsetof(struct gpu_uniforms, serial)));
	return 1;
}
static void mock_visibility_begin(void) { note("visibility_begin", 0, 0); }
static void mock_visibility_end(uint32_t slot) { note("visibility_end", slot, 0); }
static uint32_t mock_visibility_result(uint32_t slot, uint32_t *samples) { note("visibility_result", slot, 0); *samples = 1; return 1; }
static void mock_flush(void) { note("flush", 0, 0); }
static uint32_t mock_vertical_blank_wait(void) { return 0; }
static void mock_frame_start(void) {}
static uint32_t mock_present(gpu_texture b) { (void)b; note("present", 0, 0); stream_offset = 0; return 0; }
static uint32_t mock_present_stereo(const struct gpu_stereo_present *p) { note("present_stereo", (uint32_t)p->mode, 0); stream_offset = 0; return 0; }
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
static uint8_t mock_astc;
static void mock_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	(void)flags;
	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->vertex_bgra = capabilities->base_vertex = capabilities->border_clamp = capabilities->s3tc = 1;
	capabilities->shader_language = GPU_SHADER_LANGUAGE_MSL;
	capabilities->astc = mock_astc;
	capabilities->max_texture_size = 4096;
}

#define MOCK_FUNCTION(type, name, parameters, arguments) .name = mock_##name,
#define MOCK_PROCEDURE(name, parameters, arguments) .name = mock_##name,
static const struct gpu_backend mock = { GPU_FUNCTIONS(MOCK_FUNCTION, MOCK_PROCEDURE) };

/* ---------- the probe */

static int failures;
static char log_text[1 << 16];

static void log_line(const char *line)
{
	if (strlen(log_text) + strlen(line) + 2 < sizeof(log_text))
	{
		strcat(log_text, line);
		strcat(log_text, "\n");
	}
}

#define EXPECT(condition, what) \
	do { if (!(condition)) { failures++; printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static const char vertex_source[] = "void main()\n{\n\tvec4 v0 = v0_in;\n\tR0 = v0.xyzw * c[4];\n}\n";
static const char pixel_source[] = "uniform sampler2D tex0;\nvoid main()\n{\n\tt0 = texture(tex0, (xT0).xy * texture_scale[0].xy, texture_lod_bias[0]);\n}\n";

/* the "game": a frame of three draws (streamed, mirrored and indexed, and a
depth-only one) with a clear and a visibility test, drawn frames times */
static struct gpu_constant_store constants;
static struct gpu_uniforms uniforms;
static float game_memory[4096];

static void frames_draw(struct gpu_backend *gpu, int frames)
{
	static unsigned char texels[32 * 32 * 4];
	struct gpu_texture_description color_description = { GPU_TEXTURE_2D, GPU_FORMAT_BGRA8, GPU_USAGE_RENDER_TARGET, 0, 320, 240, 1, 1 };
	struct gpu_texture_description depth_description = { GPU_TEXTURE_2D, GPU_FORMAT_DEPTH_STENCIL, GPU_USAGE_RENDER_TARGET, 0, 320, 240, 1, 1 };
	/* one level uploaded, the rest generated, before the window: the file must generate them too */
	struct gpu_texture_description texture_description = { GPU_TEXTURE_2D, GPU_FORMAT_BGRA8, GPU_USAGE_UPLOAD, 0, 32, 32, 1, 6 };
	gpu_texture color, depth, texture, unused;
	gpu_shader vertex, pixel;
	gpu_buffer mirror;
	struct gpu_draw draw;
	int frame, index;

	for (index = 0; index < (int)sizeof(texels); index++)
		texels[index] = (unsigned char)(index * 7);
	for (index = 0; index < 4096; index++)
		game_memory[index] = (float)index * 0.5f;
	if (mock_astc)
		texture_description.format = GPU_FORMAT_ASTC4X4;
	color = gpu->texture_create(&color_description);
	depth = gpu->texture_create(&depth_description);
	texture = gpu->texture_create(&texture_description);
	if (mock_astc)
	{
		for (index = 0; index < 6; index++)
		{
			unsigned width = 32u >> index;
			unsigned size = ((width + 3) / 4) * ((width + 3) / 4) * 16;

			gpu->texture_upload(texture, 0, index, texels, size);
		}
	}
	else
	{
		gpu->texture_upload(texture, 0, 0, texels, sizeof(texels));
		gpu->texture_generate_mipmaps(texture, 0);
	}
	/* a texture no draw uses: it must not reach the file */
	unused = gpu->texture_create(&texture_description);
	gpu->texture_upload(unused, 0, 0, texels, sizeof(texels));
	vertex = gpu->shader_create(GPU_SHADER_VERTEX, vertex_source);
	pixel = gpu->shader_create(GPU_SHADER_PIXEL, pixel_source);
	mirror = gpu->buffer_create(0x10000);
	gpu->buffer_write(mirror, 0, 0x4000, game_memory, GPU_WRITE_UNUSED);
	constants.serial = 1;
	uniforms.serial = 1;
	uniforms.point_size[0][0] = 1.0f;
	for (frame = 0; frame < frames; frame++)
	{
		struct gpu_clear clear = { color, depth, GPU_CLEAR_COLOR | GPU_CLEAR_DEPTH, 15, 0xff102030, 1.0f, 0 };
		struct gpu_rect whole = { 0, 0, 320, 240 };
		float quad[4][4] = { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 0, 1, (float)frame, 1 }, { 1, 1, 0, 1 } };
		uint16_t indices[6] = { 0, 1, 2, 2, 1, 3 };

		gpu->clear(&clear, &whole, 1);
		memset(&draw, 0, sizeof(draw));
		draw.color_target = color;
		draw.depth_target = depth;
		draw.vertex_shader = vertex;
		draw.pixel_shader = pixel;
		draw.viewport = (struct gpu_viewport){ 0, 0, 320, 240, 0.0f, 1.0f };
		draw.scissor = whole;
		draw.blend.color_write_mask = 15;
		draw.stages[0].texture = texture;
		draw.stages[0].type = GPU_TEXTURE_2D;
		draw.stages[0].sampler.min_filter = draw.stages[0].sampler.mag_filter = draw.stages[0].sampler.mip_filter = GPU_FILTER_LINEAR;
		for (index = 0; index < GPU_ATTRIBUTE_COUNT; index++)
		{
			draw.attributes[index].format = GPU_ATTRIBUTE_FLOAT4;
			draw.attributes[index].stream = GPU_STREAM_CONSTANT;
		}
		draw.attributes[0].stream = 0;
		draw.primitive = GPU_PRIMITIVE_TRIANGLES;
		draw.count = 6;
		draw.streams[0].stride = 16;
		/* streamed vertices and indices */
		constants.c[4][0] = (float)frame;
		constants.serials[4] = ++constants.serial;
		constants.log[constants.serial % GPU_CONSTANT_LOG_SIZE] = 4;
		gpu->stream_reserve(sizeof(quad), 0);
		draw.streams[0].offset = gpu->stream(GPU_STREAM_KIND_VERTEX, quad, sizeof(quad), &draw.streams[0].buffer);
		draw.index_offset = gpu->stream(GPU_STREAM_KIND_INDEX, indices, sizeof(indices), &draw.index_buffer);
		gpu->visibility_begin();
		gpu->draw(&draw, &constants, &uniforms);
		gpu->visibility_end(7);
		/* mirrored vertices on the segment's second page, streamed indices, a base vertex */
		uniforms.fog_color[0][1] = (float)frame * 0.25f;
		uniforms.serial++;
		draw.streams[0].buffer = mirror;
		draw.streams[0].offset = 0x1000 + 64;
		draw.index_offset = gpu->stream(GPU_STREAM_KIND_INDEX, indices, sizeof(indices), &draw.index_buffer);
		draw.base_vertex = 2;
		gpu->draw(&draw, &constants, &uniforms);
		draw.base_vertex = 0;
		/* depth only, not indexed */
		draw.color_target = 0;
		draw.index_buffer = 0;
		draw.count = 3;
		gpu->draw(&draw, &constants, &uniforms);
		{
			uint32_t samples;

			gpu->visibility_result(7, &samples);
		}
		gpu->flush();
		gpu->present(color);
	}
}

static void reset_mock(uint32_t handle_start, uint32_t base)
{
	notes[0] = 0;
	next_handle = handle_start;
	stream_offset = call_count = 0;
	stream_base = base;
}

/* the notes from the first present on (the calls of frames 1 and later),
without the resources' own lines, which a capture writes when first used */
static void drawing_calls(const char *all, char *out, size_t size)
{
	const char *line = all;
	int presents = 0;

	out[0] = 0;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t length = (size_t)(end - line) + 1;
		int resource = !strncmp(line, "texture_create", 14) || !strncmp(line, "texture_upload", 14) ||
			!strncmp(line, "buffer_", 7) || !strncmp(line, "shader_create", 13);

		if (presents >= 1 && presents < 3 && !resource && strlen(out) + length < size)
			strncat(out, line, length);
		if (!strncmp(line, "present", 7))
			presents++;
		line = end + 1;
	}
}

static uint32_t same_flags(void *context, uint32_t captured) { (void)context; return captured; }

static struct gpu_replay_fuzz fuzz;
static unsigned long holes, mutated_records, reader_refusals, failures_before;

static int fuzz_mutate(void *context, uint32_t operation, uint32_t *payload, uint32_t size)
{
	failures_before = gpu_validation_failures();
	return gpu_replay_fuzz_mutate(context, operation, payload, size);
}

static void fuzz_mutated(void *context, uint32_t operation, int refused)
{
	struct gpu_replay_fuzz *state = context;

	mutated_records++;
	if (refused)
		reader_refusals++;
	else if (gpu_validation_failures() == failures_before)
	{
		holes++;
		printf("  not refused: %s (operation %u)\n", state->last, operation);
	}
}

int main(void)
{
	char folder[] = "/tmp/gpu-replay-probe-XXXXXX", path[256], damaged[256], expected[1 << 15], replayed[1 << 15];
	struct gpu_capture_host capture_host = { log_line, folder, "1+2,5+2", 0x1234abcd, 0x5678, "guest", "probe" };
	struct gpu_replay_host replay_host = { log_line, NULL, same_flags, NULL, NULL, NULL, 0x1234abcd, 0x5678, 0 };
	struct gpu_replay_result result;
	struct gpu_backend gpu = mock, player;
	struct gpu_capabilities capabilities;
	struct gpu_capture_header header;
	unsigned char *file_bytes;
	long file_size;
	char error[256];

	if (!mkdtemp(folder))
		return 2;
	snprintf(path, sizeof(path), "%s/frames-00001.capture", folder);
	snprintf(damaged, sizeof(damaged), "%s/damaged.capture", folder);

	/* capture frames 1 and 2 of four */
	reset_mock(100, 0);
	gpu_capture_install(&gpu, &capture_host);
	gpu.initialize(0, &capabilities);
	frames_draw(&gpu, 4);
	drawing_calls(notes, expected, sizeof(expected));
	EXPECT(strstr(log_text, "wrote") && !strstr(log_text, "FAILED"), "the window's file is written");
	EXPECT(gpu_replay_header(path, &header, error, sizeof(error)) == 0 && header.first_frame == 1 && header.frame_count == 2 &&
		header.interface_hash == 0x1234abcd && !strcmp(header.label, "probe"), "the file's header says what it holds");

	/* replay it into a backend whose handles and stream offsets are others */
	reset_mock(5000, 4096);
	player = mock;
	EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == 0 && result.frames == 2 && result.draws == 6, "the file replays");
	drawing_calls(notes, replayed, sizeof(replayed));
	{
		/* the replay's notes begin at its first call, the capture's at frame 1: compare the replay's calls
		from its start, resources left out, with the capture's frames 1 and 2 */
		char all[1 << 15];
		const char *line = notes;

		all[0] = 0;
		while (*line)
		{
			const char *end = strchr(line, '\n');
			size_t length = (size_t)(end - line) + 1;

			if (strncmp(line, "texture_create", 14) && strncmp(line, "texture_upload", 14) && strncmp(line, "buffer_", 7) &&
				strncmp(line, "shader_create", 13) && strncmp(line, "mipmaps", 7))
				strncat(all, line, length);
			line = end + 1;
		}
		EXPECT(!strcmp(all, expected), "the replayed calls are the captured frames' calls, with the same data");
		if (strcmp(all, expected))
			printf("--- captured\n%s--- replayed\n%s", expected, all);
	}
	/* what the file holds of the resources: the two targets, the one texture a draw used (not the unused
	one), both shaders, and of the segment only the page a draw read */
	{
		int creates = 0, uploads = 0, writes = 0, shaders = 0;
		const char *line;

		for (line = notes; (line = strstr(line, "texture_create")) != NULL; line++) creates++;
		for (line = notes; (line = strstr(line, "texture_upload")) != NULL; line++) uploads++;
		for (line = notes; (line = strstr(line, "buffer_write")) != NULL; line++) writes++;
		for (line = notes; (line = strstr(line, "shader_create")) != NULL; line++) shaders++;
		EXPECT(creates == 3 && uploads == 1 && shaders == 2 && writes == 1, "the file holds only what its frames use");
		EXPECT(strstr(notes, "mipmaps 00000000") != NULL, "mipmaps generated before the window are generated in the replay");
		{
			char wanted[64];

			snprintf(wanted, sizeof(wanted), "buffer_write 00001000 %08x", hash_bytes((const char *)game_memory + 0x1000, 0x1000));
			EXPECT(strstr(notes, wanted) != NULL, "the mirrored page is the game's memory, read where it lies");
		}
	}

	/* ASTC changes the front end's uploads, so captures require the same capability. */
	mock_astc = 1;
	reset_mock(1, 0);
	EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == -1 && strstr(result.error, "ASTC"),
		"a capture from a backend with another ASTC capability is refused");
	mock_astc = 0;

	/* stale and foreign captures */
	replay_host.interface_hash = 0x999;
	reset_mock(1, 0);
	EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == -1 && strstr(result.error, "stale") && call_count == 0,
		"a capture made against another gpu.h is refused before any call");
	replay_host.force = 1;
	EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == 0 && result.frames == 2, "unless the replay is forced");
	replay_host.force = 0;
	replay_host.interface_hash = 0x1234abcd;
	replay_host.front_end_hash = 0x1;
	log_text[0] = 0;
	EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == 0 && result.stale_front_end && strstr(log_text, "front end has changed"),
		"a capture from another front end plays, with a warning");
	replay_host.front_end_hash = 0x5678;

	/* damage: every prefix of the file, and bytes flipped all through it, must be refused or played without a
	fault, through the validator in REJECT mode (the address sanitizer watches) */
	{
		gzFile whole = gzopen(path, "rb");
		static unsigned char raw[1 << 20];
		struct gpu_validation_host validation_host = { log_line, NULL, GPU_VALIDATION_REJECT, NULL };
		int raw_size = gzread(whole, raw, sizeof(raw)), trial;
		uint64_t state = 88172645463325252ull;

		gzclose(whole);
		EXPECT(raw_size > (int)sizeof(struct gpu_capture_header) && raw_size < (int)sizeof(raw), "the file's bytes");
		file_bytes = raw;
		file_size = raw_size;
		for (trial = 0; trial < 600; trial++)
		{
			static unsigned char copy[1 << 20];
			long size = file_size;
			gzFile out;
			int flips, flip;

			memcpy(copy, file_bytes, (size_t)file_size);
			if (trial < 200)
				size = (long)(sizeof(struct gpu_capture_header) / 2 + (unsigned long)trial * (unsigned long)file_size / 200);
			else
			{
				flips = 1 + trial % 7;
				for (flip = 0; flip < flips; flip++)
				{
					state = state * 6364136223846793005ull + 1442695040888963407ull;
					/* (past the header's hashes and sizes, which have their own tests) */
					copy[sizeof(struct gpu_capture_header) + (state >> 33) % ((unsigned long)file_size - sizeof(struct gpu_capture_header))] ^=
						(unsigned char)(1u << ((state >> 20) & 7));
				}
			}
			if (size > file_size)
				size = file_size;
			out = gzopen(damaged, "wb1");
			gzwrite(out, copy, (unsigned)size);
			gzclose(out);
			reset_mock(1, 0);
			player = mock;
			gpu_validation_install(&player, &validation_host);
			gpu_replay_run(damaged, &player, &replay_host, &result);
		}
		EXPECT(1, "600 damaged files were read");
	}

	/* the fuzzer: every mutation it makes is refused, by the reader or by the validator */
	{
		struct gpu_validation_host validation_host = { log_line, NULL, GPU_VALIDATION_REJECT, NULL };
		int seed;

		replay_host.mutate = fuzz_mutate;
		replay_host.mutated = fuzz_mutated;
		replay_host.context = &fuzz;
		for (seed = 1; seed <= 400; seed++)
		{
			memset(&fuzz, 0, sizeof(fuzz));
			fuzz.state = (uint64_t)seed * 2654435761u;
			fuzz.every = 3;
			reset_mock(1, 0);
			player = mock;
			gpu_validation_install(&player, &validation_host);
			EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == 0, "a mutated capture plays to its end");
		}
		EXPECT(mutated_records > 1000 && holes == 0, "every mutation is refused");
		printf("fuzz: %lu mutated records, %lu refused by the reader, %lu not refused\n", mutated_records, reader_refusals, holes);
		/* and without mutations the validator has nothing to say about the capture */
		replay_host.mutate = NULL;
		replay_host.mutated = NULL;
		reset_mock(1, 0);
		player = mock;
		gpu_validation_install(&player, &validation_host);
		EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == 0 && gpu_validation_failures() == 0 &&
			gpu_validation_rejected() == 0, "the unmutated capture has no failures");
	}
	/* Keep ASTC's full blocks in captures, including the 2x2 and 1x1 mips. */
	mock_astc = 1;
	reset_mock(100, 0);
	gpu.initialize(0, &capabilities);
	frames_draw(&gpu, 4);
	snprintf(path, sizeof(path), "%s/frames-00005.capture", folder);
	reset_mock(5000, 4096);
	player = mock;
	EXPECT(gpu_replay_run(path, &player, &replay_host, &result) == 0, "ASTC uploads replay");
	{
		unsigned char block[16];
		char wanted[64];
		unsigned index;

		for (index = 0; index < sizeof(block); index++)
			block[index] = (unsigned char)(index * 7);
		snprintf(wanted, sizeof(wanted), "texture_upload 00000005 %08x", hash_bytes(block, sizeof(block)));
		EXPECT(strstr(notes, wanted) != NULL, "the 1x1 ASTC capture keeps all 16 bytes");
	}
	unlink(path);
	snprintf(path, sizeof(path), "%s/frames-00001.capture", folder);
	unlink(path);
	unlink(damaged);
	rmdir(folder);
	if (failures)
	{
		printf("gpu replay probe: %d checks failed\n", failures);
		return 1;
	}
	printf("gpu replay probe: all checks passed\n");
	return 0;
}
