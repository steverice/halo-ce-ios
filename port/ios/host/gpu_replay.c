/*
GPU_REPLAY.C

The replaying half of gpu_capture.h: reads a capture file and makes its
calls into a backend, with no game. The file's handles are the capturing
run's; each is mapped to the handle this run's backend gives for the same
object, and a draw's streamed ranges to where this run's gpu_stream put the
same bytes.

Nothing in a file is trusted: every record's size is checked against what
its operation holds before a byte of it is used, a handle the file never
created maps to none, and a record that does not hold together stops the
replay (or, for the fuzzer, is skipped and counted). What the calls
themselves ask of the backend is the validator's to judge (gpu_validate.c).
*/

#include "gpu_capture.h"

#if HALO_RENDER_VALIDATION

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define HANDLE_LIMIT (1u << 22)
enum { STREAM_RANGES = 256 };

struct handle_map
{
	uint32_t *actual;
	uint32_t count;
};

struct replay
{
	const struct gpu_backend *backend;
	const struct gpu_replay_host *host;
	struct gpu_replay_result *result;
	struct handle_map textures, buffers, shaders;
	/* The validator reads vertex and index data where the caller keeps it,
	until the draw that uses it (gpu_validate.c): the game's memory, in the
	game. Here it is the replay's: each mirror segment's bytes, by the
	file's handle, and the frame's streamed ranges, kept until its present */
	uint8_t **segments;
	uint32_t *segment_sizes, segment_count;
	void **streamed;
	uint32_t streamed_count, streamed_capacity;
	/* each mapped texture's size, by the file's handle */
	uint32_t (*sizes)[2];
	uint32_t size_count;
	struct { uint32_t buffer, offset, size, actual_buffer, actual_offset; } ranges[STREAM_RANGES];
	unsigned long range_next;
	struct gpu_constant_store constants;
	struct gpu_uniforms uniforms;
	uint32_t uniforms_serial;
	uint32_t frame;
};

static void replay_log(const struct gpu_replay_host *host, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void replay_log(const struct gpu_replay_host *host, const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (host && host->log)
		host->log(line);
	else
		fprintf(stderr, "%s\n", line);
}

static int map_set(struct handle_map *map, uint32_t handle, uint32_t actual)
{
	if (!handle || handle >= HANDLE_LIMIT)
		return 0;
	if (handle >= map->count)
	{
		uint32_t count = map->count ? map->count : 256, *grown;

		while (count <= handle)
			count *= 2;
		grown = realloc(map->actual, count * sizeof(*grown));
		if (!grown)
			return 0;
		memset(grown + map->count, 0, (count - map->count) * sizeof(*grown));
		map->actual = grown;
		map->count = count;
	}
	map->actual[handle] = actual;
	return 1;
}

/* this run's handle for the file's, 0 for one the file never created */
static uint32_t map_get(const struct handle_map *map, uint32_t handle)
{
	return handle && handle < map->count ? map->actual[handle] : 0;
}

static void size_set(struct replay *replay, uint32_t handle, uint32_t width, uint32_t height)
{
	if (!handle || handle >= HANDLE_LIMIT)
		return;
	if (handle >= replay->size_count)
	{
		uint32_t count = replay->size_count ? replay->size_count : 256;
		void *grown;

		while (count <= handle)
			count *= 2;
		grown = realloc(replay->sizes, count * sizeof(*replay->sizes));
		if (!grown)
			return;
		replay->sizes = grown;
		memset(replay->sizes + replay->size_count, 0, (count - replay->size_count) * sizeof(*replay->sizes));
		replay->size_count = count;
	}
	replay->sizes[handle][0] = width;
	replay->sizes[handle][1] = height;
}

static uint8_t *segment_make(struct replay *replay, uint32_t handle, uint32_t size)
{
	if (!handle || handle >= HANDLE_LIMIT)
		return NULL;
	if (handle >= replay->segment_count)
	{
		uint32_t count = replay->segment_count ? replay->segment_count : 256;
		uint8_t **segments;
		uint32_t *sizes;

		while (count <= handle)
			count *= 2;
		segments = realloc(replay->segments, count * sizeof(*segments));
		if (!segments)
			return NULL;
		replay->segments = segments;
		memset(segments + replay->segment_count, 0, (count - replay->segment_count) * sizeof(*segments));
		sizes = realloc(replay->segment_sizes, count * sizeof(*sizes));
		if (!sizes)
			return NULL;
		replay->segment_sizes = sizes;
		memset(sizes + replay->segment_count, 0, (count - replay->segment_count) * sizeof(*sizes));
		replay->segment_count = count;
	}
	free(replay->segments[handle]);
	replay->segments[handle] = calloc(size ? size : 1, 1);
	replay->segment_sizes[handle] = replay->segments[handle] ? size : 0;
	return replay->segments[handle];
}

static void *streamed_keep(struct replay *replay, const void *bytes, uint32_t size)
{
	void *copy;

	if (replay->streamed_count == replay->streamed_capacity)
	{
		uint32_t capacity = replay->streamed_capacity ? replay->streamed_capacity * 2 : 1024;
		void **grown = realloc(replay->streamed, capacity * sizeof(*grown));

		if (!grown)
			return NULL;
		replay->streamed = grown;
		replay->streamed_capacity = capacity;
	}
	copy = malloc(size);
	if (copy)
	{
		memcpy(copy, bytes, size);
		replay->streamed[replay->streamed_count++] = copy;
	}
	return copy;
}

static void streamed_release(struct replay *replay)
{
	while (replay->streamed_count)
		free(replay->streamed[--replay->streamed_count]);
}

/* a buffer and offset of the file's as this run's: a mirror segment's
handle, or the streamed range that starts there */
static void map_range(struct replay *replay, uint32_t *buffer, uint32_t *offset)
{
	uint32_t actual = map_get(&replay->buffers, *buffer);
	unsigned long back;

	if (!*buffer)
		return;
	if (actual)
	{
		*buffer = actual;
		return;
	}
	for (back = 1; back <= STREAM_RANGES && back <= replay->range_next; back++)
	{
		unsigned long slot = (replay->range_next - back) % STREAM_RANGES;

		if (replay->ranges[slot].buffer == *buffer && *offset >= replay->ranges[slot].offset &&
			*offset - replay->ranges[slot].offset < (replay->ranges[slot].size ? replay->ranges[slot].size : 1))
		{
			*offset = replay->ranges[slot].actual_offset + (*offset - replay->ranges[slot].offset);
			*buffer = replay->ranges[slot].actual_buffer;
			return;
		}
	}
	*buffer = 0;
}

static void present_frame(struct replay *replay, const uint32_t *file_handles, uint32_t count)
{
	gpu_texture pictures[2];
	uint32_t sizes[2][2], index;

	for (index = 0; index < count && index < 2; index++)
	{
		pictures[index] = map_get(&replay->textures, file_handles[index]);
		sizes[index][0] = file_handles[index] < replay->size_count ? replay->sizes[file_handles[index]][0] : 0;
		sizes[index][1] = file_handles[index] < replay->size_count ? replay->sizes[file_handles[index]][1] : 0;
	}
	if (replay->host->frame)
		replay->host->frame(replay->host->context, replay->frame, pictures, (const uint32_t (*)[2])sizes, count < 2 ? count : 2);
}

/* plays one record; 0 if it does not hold together */
static int play(struct replay *replay, uint32_t operation, uint32_t *words, uint32_t size)
{
	const struct gpu_backend *backend = replay->backend;
	const uint8_t *bytes = (const uint8_t *)words;

#define NEED(count) do { if (size < (count)) return 0; } while (0)
	switch (operation)
	{
	case GPU_CAPTURE_TEXTURE_CREATE:
	{
		struct gpu_texture_description description;

		NEED(4 + sizeof(description));
		memcpy(&description, bytes + 4, sizeof(description));
		if (!map_set(&replay->textures, words[0], backend->texture_create(&description)))
			return 0;
		size_set(replay, words[0], description.width, description.height);
		return 1;
	}
	case GPU_CAPTURE_TEXTURE_UPLOAD:
		NEED(16);
		if (words[3] > size - 16)
			return 0;
		backend->texture_upload(map_get(&replay->textures, words[0]), words[1], words[2], bytes + 16, words[3]);
		return 1;
	case GPU_CAPTURE_TEXTURE_COPY_LEVEL:
		NEED(12);
		backend->texture_copy_level(map_get(&replay->textures, words[0]), map_get(&replay->textures, words[1]), words[2]);
		return 1;
	case GPU_CAPTURE_TEXTURE_MIPMAPS:
		NEED(8);
		backend->texture_generate_mipmaps(map_get(&replay->textures, words[0]), words[1]);
		return 1;
	case GPU_CAPTURE_TEXTURE_DESTROY:
		NEED(4);
		backend->texture_destroy(map_get(&replay->textures, words[0]));
		map_set(&replay->textures, words[0], 0);
		return 1;
	case GPU_CAPTURE_BUFFER_CREATE:
		NEED(8);
		/* (a mirror segment is 4 MB; nothing of the game's is near this) */
		if (!words[1] || words[1] > (1u << 28) || !segment_make(replay, words[0], words[1]))
			return 0;
		return map_set(&replay->buffers, words[0], backend->buffer_create(words[1]));
	case GPU_CAPTURE_BUFFER_WRITE:
		NEED(16);
		if (words[2] > size - 16)
			return 0;
		/* into the replay's copy of the segment first, when the write fits
		it; one that does not is the validator's to refuse */
		if (words[0] < replay->segment_count && replay->segments[words[0]] && words[1] <= replay->segment_sizes[words[0]] &&
			words[2] <= replay->segment_sizes[words[0]] - words[1])
		{
			memcpy(replay->segments[words[0]] + words[1], bytes + 16, words[2]);
			backend->buffer_write(map_get(&replay->buffers, words[0]), words[1], words[2], replay->segments[words[0]] + words[1],
				words[3]);
		}
		else
		{
			backend->buffer_write(map_get(&replay->buffers, words[0]), words[1], words[2], bytes + 16, words[3]);
		}
		return 1;
	case GPU_CAPTURE_STREAM_RESERVE:
		NEED(8);
		backend->stream_reserve(words[0], words[1]);
		return 1;
	case GPU_CAPTURE_STREAM:
	{
		unsigned long slot = replay->range_next % STREAM_RANGES;
		gpu_buffer buffer = 0;
		void *kept;

		NEED(16);
		if (!words[1] || words[1] > size - 16 || !(kept = streamed_keep(replay, bytes + 16, words[1])))
			return 0;
		replay->ranges[slot].actual_offset = backend->stream(words[0], kept, words[1], &buffer);
		replay->ranges[slot].actual_buffer = buffer;
		replay->ranges[slot].size = words[1];
		replay->ranges[slot].buffer = words[2];
		replay->ranges[slot].offset = words[3];
		replay->range_next++;
		return 1;
	}
	case GPU_CAPTURE_SHADER_CREATE:
	{
		char *text;
		int mapped;

		NEED(12);
		if (words[2] > size - 12 || memchr(bytes + 12, 0, words[2]))
			return 0;
		text = malloc((size_t)words[2] + 1);
		if (!text)
			return 0;
		memcpy(text, bytes + 12, words[2]);
		text[words[2]] = 0;
		mapped = map_set(&replay->shaders, words[0], backend->shader_create(words[1], text));
		free(text);
		return mapped;
	}
	case GPU_CAPTURE_CONSTANTS:
	{
		uint32_t index;

		NEED(4);
		if (words[0] > GPU_CONSTANT_COUNT || size - 4 < words[0] * 20)
			return 0;
		for (index = 0; index < words[0]; index++)
			if (words[1 + index * 5] >= GPU_CONSTANT_COUNT)
				return 0;
		/* as the front end stores them (d3d8_device.c's constants_store) */
		for (index = 0; index < words[0]; index++)
		{
			uint32_t reg = words[1 + index * 5];

			memcpy(replay->constants.c[reg], &words[2 + index * 5], 16);
			replay->constants.serial++;
			replay->constants.serials[reg] = replay->constants.serial;
			replay->constants.log[replay->constants.serial % GPU_CONSTANT_LOG_SIZE] = (uint8_t)reg;
		}
		return 1;
	}
	case GPU_CAPTURE_UNIFORMS:
		NEED(sizeof(replay->uniforms));
		memcpy(&replay->uniforms, bytes, sizeof(replay->uniforms));
		replay->uniforms.serial = ++replay->uniforms_serial;
		return 1;
	case GPU_CAPTURE_CLEAR:
	{
		struct gpu_clear clear;

		NEED(4 + sizeof(clear));
		if (words[0] > 4096 || size - 4 - sizeof(clear) < words[0] * sizeof(struct gpu_rect))
			return 0;
		memcpy(&clear, bytes + 4, sizeof(clear));
		clear.color_target = map_get(&replay->textures, clear.color_target);
		clear.depth_target = map_get(&replay->textures, clear.depth_target);
		backend->clear(&clear, (const struct gpu_rect *)(const void *)(bytes + 4 + sizeof(clear)), words[0]);
		return 1;
	}
	case GPU_CAPTURE_DRAW:
	{
		static struct gpu_draw draw;
		int index;

		NEED(4 + sizeof(draw));
		memcpy(&draw, bytes + 4, sizeof(draw));
		draw.color_target = map_get(&replay->textures, draw.color_target);
		draw.depth_target = map_get(&replay->textures, draw.depth_target);
		draw.vertex_shader = map_get(&replay->shaders, draw.vertex_shader);
		draw.pixel_shader = map_get(&replay->shaders, draw.pixel_shader);
		for (index = 0; index < GPU_STAGE_COUNT; index++)
			draw.stages[index].texture = map_get(&replay->textures, draw.stages[index].texture);
		for (index = 0; index < 16; index++)
			map_range(replay, &draw.streams[index].buffer, &draw.streams[index].offset);
		map_range(replay, &draw.index_buffer, &draw.index_offset);
		replay->result->draws++;
		if (!backend->draw(&draw, &replay->constants, &replay->uniforms))
			replay->result->draws_skipped++;
		return 1;
	}
	case GPU_CAPTURE_VISIBILITY_BEGIN:
		backend->visibility_begin();
		return 1;
	case GPU_CAPTURE_VISIBILITY_END:
		NEED(4);
		backend->visibility_end(words[0]);
		return 1;
	case GPU_CAPTURE_VISIBILITY_RESULT:
	{
		uint32_t samples = 0;

		NEED(4);
		/* (the backends index their slots with it) */
		if (words[0] >= GPU_VISIBILITY_SLOTS)
			return 0;
		backend->visibility_result(words[0], &samples);
		return 1;
	}
	case GPU_CAPTURE_FLUSH:
		backend->flush();
		return 1;
	case GPU_CAPTURE_PRESENT:
		NEED(4);
		present_frame(replay, &words[0], 1);
		backend->present(map_get(&replay->textures, words[0]));
		streamed_release(replay);
		replay->frame++;
		replay->result->frames++;
		return 1;
	case GPU_CAPTURE_PRESENT_STEREO:
	{
		static struct gpu_stereo_present present;
		uint32_t pictures[2];
		int index;

		NEED(sizeof(present));
		memcpy(&present, bytes, sizeof(present));
		pictures[0] = present.zoom ? present.zoom : present.eye_color[0];
		pictures[1] = present.zoom ? present.zoom : present.eye_color[1];
		present_frame(replay, pictures, present.zoom ? 1 : 2);
		for (index = 0; index < 2; index++)
		{
			present.eye_color[index] = map_get(&replay->textures, present.eye_color[index]);
			present.eye_depth[index] = map_get(&replay->textures, present.eye_depth[index]);
		}
		present.hud = map_get(&replay->textures, present.hud);
		present.zoom = map_get(&replay->textures, present.zoom);
		present.reticle_layer = map_get(&replay->textures, present.reticle_layer);
		present.ui = map_get(&replay->textures, present.ui);
		for (index = 0; index < HALO_HUD_GROUP_COUNT; index++)
			present.hud_group[index] = map_get(&replay->textures, present.hud_group[index]);
		backend->present_stereo(&present);
		streamed_release(replay);
		replay->frame++;
		replay->result->frames++;
		return 1;
	}
	default:
		return 0;
	}
#undef NEED
}

static int header_read(gzFile file, struct gpu_capture_header *header, char *error, size_t size)
{
	if (gzread(file, header, sizeof(*header)) != (int)sizeof(*header) || memcmp(header->magic, GPU_CAPTURE_MAGIC, sizeof(GPU_CAPTURE_MAGIC)))
	{
		snprintf(error, size, "not a render capture");
		return -1;
	}
	header->guest_sha256[sizeof(header->guest_sha256) - 1] = 0;
	header->label[sizeof(header->label) - 1] = 0;
	if (header->format != GPU_CAPTURE_FORMAT)
	{
		snprintf(error, size, "capture format %u; this build reads format %u", header->format, GPU_CAPTURE_FORMAT);
		return -1;
	}
	return 0;
}

int gpu_replay_header(const char *path, struct gpu_capture_header *header, char *error, size_t size)
{
	gzFile file = gzopen(path, "rb");
	int status;

	if (!file)
	{
		snprintf(error, size, "cannot read %s", path);
		return -1;
	}
	status = header_read(file, header, error, size);
	gzclose(file);
	return status;
}

int gpu_replay_run(const char *path, const struct gpu_backend *backend, const struct gpu_replay_host *host,
	struct gpu_replay_result *result)
{
	static struct replay replay;
	struct gpu_capture_header header;
	struct gpu_capabilities capabilities;
	uint32_t *payload = NULL, capacity = 0;
	gzFile file = gzopen(path, "rb");
	int status = -1, ended = 0;

	memset(result, 0, sizeof(*result));
	memset(&replay, 0, sizeof(replay));
	if (!file)
	{
		snprintf(result->error, sizeof(result->error), "cannot read %s", path);
		return -1;
	}
	if (header_read(file, &header, result->error, sizeof(result->error)))
		goto done;
	/* stale: gpu.h is not what the capture was made against */
	if (header.interface_hash != host->interface_hash || header.draw_size != sizeof(struct gpu_draw) ||
		header.uniforms_size != sizeof(struct gpu_uniforms) || header.stereo_size != sizeof(struct gpu_stereo_present) ||
		header.description_size != sizeof(struct gpu_texture_description) || header.clear_size != sizeof(struct gpu_clear) ||
		header.capabilities_size != sizeof(struct gpu_capabilities))
	{
		snprintf(result->error, sizeof(result->error), "the capture is stale: it was made against gpu.h %08x (a draw of %u bytes), "
			"and this build has %08x (%zu bytes); record it again", header.interface_hash, header.draw_size, host->interface_hash,
			sizeof(struct gpu_draw));
		if (!host->force || header.draw_size != sizeof(struct gpu_draw) || header.uniforms_size != sizeof(struct gpu_uniforms) ||
			header.stereo_size != sizeof(struct gpu_stereo_present) || header.description_size != sizeof(struct gpu_texture_description) ||
			header.clear_size != sizeof(struct gpu_clear) || header.capabilities_size != sizeof(struct gpu_capabilities))
			goto done;
		replay_log(host, "render replay: %s; played anyway, as asked", result->error);
		result->error[0] = 0;
	}
	if (header.front_end_hash != host->front_end_hash)
	{
		result->stale_front_end = 1;
		replay_log(host, "render replay: the front end has changed since this capture was made (%08x then, %08x now): the replay "
			"tests the backend against the old front end's calls; record again to cover the front end's change",
			header.front_end_hash, host->front_end_hash);
	}
	replay.backend = backend;
	replay.host = host;
	replay.result = result;
	replay.frame = header.first_frame;
	backend->initialize(host->initialize_flags ? host->initialize_flags(host->context, header.initialize_flags) :
		header.initialize_flags, &capabilities);
	/* the capabilities the front end built the calls for */
	if (capabilities.vertex_bgra != header.capabilities.vertex_bgra || capabilities.base_vertex != header.capabilities.base_vertex ||
		capabilities.triangle_fans != header.capabilities.triangle_fans || capabilities.line_loops != header.capabilities.line_loops ||
		capabilities.sampler_lod_bias != header.capabilities.sampler_lod_bias ||
		capabilities.occlusion_mode != header.capabilities.occlusion_mode || capabilities.s3tc != header.capabilities.s3tc ||
		capabilities.border_clamp != header.capabilities.border_clamp ||
		capabilities.shader_language != header.capabilities.shader_language || capabilities.shader_es != header.capabilities.shader_es ||
		capabilities.clip_y_flip != header.capabilities.clip_y_flip || capabilities.clip_z_remap != header.capabilities.clip_z_remap)
	{
		snprintf(result->error, sizeof(result->error), "the capture was made for another backend: shader language %u (this one %u), "
			"vertex BGRA %u (%u), base vertex %u (%u), fans %u (%u), S3TC %u (%u), border clamp %u (%u), occlusion %u (%u)",
			header.capabilities.shader_language, capabilities.shader_language, header.capabilities.vertex_bgra,
			capabilities.vertex_bgra, header.capabilities.base_vertex, capabilities.base_vertex, header.capabilities.triangle_fans,
			capabilities.triangle_fans, header.capabilities.s3tc, capabilities.s3tc, header.capabilities.border_clamp,
			capabilities.border_clamp, header.capabilities.occlusion_mode, capabilities.occlusion_mode);
		goto done;
	}
	replay_log(host, "render replay: %s: frames %u to %u%s%s", path, header.first_frame, header.first_frame + header.frame_count - 1,
		header.label[0] ? ", " : "", header.label);
	while (!ended)
	{
		struct gpu_capture_record record;

		if (gzread(file, &record, sizeof(record)) != (int)sizeof(record))
		{
			snprintf(result->error, sizeof(result->error), "the capture ends without its end record, after %u records", result->records);
			goto done;
		}
		if (record.size > GPU_CAPTURE_RECORD_LIMIT || (record.size & 3) || !record.operation ||
			record.operation >= GPU_CAPTURE_OPERATION_COUNT)
		{
			snprintf(result->error, sizeof(result->error), "record %u is malformed: operation %u, %u bytes", result->records,
				record.operation, record.size);
			goto done;
		}
		if (record.size + 8 > capacity)
		{
			uint32_t *grown = realloc(payload, record.size + 8);

			if (!grown)
			{
				snprintf(result->error, sizeof(result->error), "no memory for record %u (%u bytes)", result->records, record.size);
				goto done;
			}
			payload = grown;
			capacity = record.size + 8;
		}
		if (record.size && gzread(file, payload, record.size) != (int)record.size)
		{
			snprintf(result->error, sizeof(result->error), "record %u is cut short", result->records);
			goto done;
		}
		if (record.operation == GPU_CAPTURE_END)
		{
			ended = 1;
			break;
		}
		if (host->mutate && host->mutate(host->context, record.operation, payload, record.size))
		{
			int refused;

			result->mutated++;
			/* a mutated record that no longer holds together is refused
			here, which is a pass */
			refused = !play(&replay, record.operation, payload, record.size);
			if (host->mutated)
				host->mutated(host->context, record.operation, refused);
		}
		else if (!play(&replay, record.operation, payload, record.size))
		{
			snprintf(result->error, sizeof(result->error), "record %u (operation %u, %u bytes) does not hold together; the replay "
				"stops before it", result->records, record.operation, record.size);
			goto done;
		}
		result->records++;
	}
	status = 0;
done:
	gzclose(file);
	free(payload);
	free(replay.textures.actual);
	free(replay.buffers.actual);
	free(replay.shaders.actual);
	free(replay.sizes);
	streamed_release(&replay);
	free(replay.streamed);
	{
		uint32_t handle;

		for (handle = 0; handle < replay.segment_count; handle++)
			free(replay.segments[handle]);
	}
	free(replay.segments);
	free(replay.segment_sizes);
	return status;
}

#endif
