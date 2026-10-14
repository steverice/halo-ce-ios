/*
GPU_CAPTURE.C

The capturing half of gpu_capture.h: a struct gpu_backend whose functions
record the front end's calls while a window of frames is open.

Outside a window it only keeps track: each texture's description and a copy
of its latest uploads (a capture build's cost in memory, about what the GPU
holds in textures; freed when the last window closes), each shader's text,
and where each mirror segment's bytes come from (gpu_buffer_write's data
pointer less its offset is the segment's base in the game's memory, as
gpu_validate.c reads it). Inside a window, the first call that uses a
texture, a shader or a page of a segment writes it into the file first.
*/

#include "gpu_capture.h"

#if HALO_RENDER_VALIDATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define HANDLE_LIMIT (1u << 22)
enum { LEVELS = 16, WINDOWS = 16, STREAM_RANGES = 256 };

struct texture_entry
{
	uint8_t live;
	uint32_t epoch;
	struct gpu_texture_description description;
	uint8_t *data[6][LEVELS];
	uint32_t size[6][LEVELS];
	/* the level its mipmaps were last generated from, plus one; 0 for never
	(the high-res HUD and text upload one level and generate the rest) */
	uint32_t mipmaps_from;
};

struct buffer_entry
{
	uint8_t live, source_unknown;
	uint32_t epoch, size;
	const uint8_t *source;
	/* per page: written at all, and the window it was last written into */
	uint8_t *written;
	uint32_t *page_epoch;
};

struct shader_entry
{
	uint8_t live, stage;
	uint32_t epoch;
	char *text;
};

struct table
{
	void *entries;
	uint32_t count;
	size_t size;
};

struct stream_range
{
	gpu_buffer buffer;
	uint32_t offset, size;
	const uint8_t *data;
};

static struct
{
	struct gpu_backend backend;
	struct gpu_capture_host host;
	char folder[1024];
	struct { uint32_t first, count; } windows[WINDOWS];
	unsigned window_count, windows_done;
	struct table textures, buffers, shaders;
	struct stream_range ranges[STREAM_RANGES];
	unsigned long range_next;
	uint32_t initialize_flags;
	struct gpu_capabilities capabilities;
	uint32_t frame;
	/* the open window */
	gzFile file;
	char path[1280];
	uint32_t epoch, window_end;
	unsigned long records;
	int failed;
	/* the log has said a call went unrecorded (capture_unrecorded) */
	int unrecorded_logged;
	/* what the open window's file has of the constants and uniforms */
	int constants_sent, uniforms_sent;
	uint64_t constants_serial;
	float constants[GPU_CONSTANT_COUNT][4];
	struct gpu_uniforms uniforms;
	int installed;
} capture;

static void capture_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>

static void capture_log(const char *format, ...)
{
	char line[1536];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (capture.host.log)
		capture.host.log(line);
	else
		fprintf(stderr, "%s\n", line);
}

/* ---------- tables by handle */

static void *table_entry(struct table *table, uint32_t handle, int create)
{
	if (!handle || handle >= HANDLE_LIMIT)
		return NULL;
	if (handle >= table->count)
	{
		uint32_t count = table->count ? table->count : 256;
		void *entries;

		if (!create)
			return NULL;
		while (count <= handle)
			count *= 2;
		entries = realloc(table->entries, count * table->size);
		if (!entries)
			return NULL;
		memset((char *)entries + table->count * table->size, 0, (count - table->count) * table->size);
		table->entries = entries;
		table->count = count;
	}
	return (char *)table->entries + handle * table->size;
}

static struct texture_entry *texture_entry(gpu_texture handle)
{
	struct texture_entry *entry = table_entry(&capture.textures, handle, 0);

	return entry && entry->live ? entry : NULL;
}

static struct buffer_entry *buffer_entry(gpu_buffer handle)
{
	struct buffer_entry *entry = table_entry(&capture.buffers, handle, 0);

	return entry && entry->live ? entry : NULL;
}

static struct shader_entry *shader_entry(gpu_shader handle)
{
	struct shader_entry *entry = table_entry(&capture.shaders, handle, 0);

	return entry && entry->live ? entry : NULL;
}

/* whether windows are still to come or one is open: the uploads are kept */
static int armed(void)
{
	return capture.file != NULL || capture.windows_done < capture.window_count;
}

/* ---------- writing */

static void put_bytes(const void *bytes, uint32_t size)
{
	static const char padding[4];

	if (!size || capture.failed)
		return;
	if (gzwrite(capture.file, bytes, size) != (int)size || ((size & 3) && gzwrite(capture.file, padding, 4 - (size & 3)) <= 0))
		capture.failed = 1;
}

/* one record: words, then a struct, then bytes, each padded to four */
static void put(uint32_t operation, const uint32_t *words, uint32_t word_count, const void *structure, uint32_t structure_size,
	const void *bytes, uint32_t byte_count)
{
	struct gpu_capture_record record = { operation, word_count * 4 + ((structure_size + 3) & ~3u) + ((byte_count + 3) & ~3u) };

	if (!capture.file || capture.failed)
		return;
	if (gzwrite(capture.file, &record, sizeof(record)) != (int)sizeof(record))
		capture.failed = 1;
	put_bytes(words, word_count * 4);
	put_bytes(structure, structure_size);
	put_bytes(bytes, byte_count);
	capture.records++;
}

static void window_close(void)
{
	if (!capture.file)
		return;
	put(GPU_CAPTURE_END, NULL, 0, NULL, 0, NULL, 0);
	if (gzclose(capture.file) != Z_OK)
		capture.failed = 1;
	capture.file = NULL;
	capture.windows_done++;
	{
		FILE *written = fopen(capture.path, "rb");
		long size = 0;

		if (written)
		{
			fseek(written, 0, SEEK_END);
			size = ftell(written);
			fclose(written);
		}
		capture_log("render capture: %s %s: %lu records, %.1f MB", capture.failed ? "FAILED writing" : "wrote", capture.path,
			capture.records, (double)size / 1048576.0);
	}
	/* nothing more to capture: the kept uploads go */
	if (!armed())
	{
		uint32_t handle;

		for (handle = 0; handle < capture.textures.count; handle++)
		{
			struct texture_entry *entry = &((struct texture_entry *)capture.textures.entries)[handle];
			int face, level;

			for (face = 0; face < 6; face++)
				for (level = 0; level < LEVELS; level++)
				{
					free(entry->data[face][level]);
					entry->data[face][level] = NULL;
				}
		}
	}
}

void gpu_capture_finish(void)
{
	if (capture.file)
	{
		capture_log("render capture: the run ended inside a window, at frame %u of %u", capture.frame, capture.window_end);
		window_close();
	}
}

static void window_open_if_due(void)
{
	struct gpu_capture_header header;
	unsigned index;

	if (capture.file)
		return;
	for (index = 0; index < capture.window_count; index++)
		if (capture.windows[index].first == capture.frame)
			break;
	if (index == capture.window_count)
		return;
	snprintf(capture.path, sizeof(capture.path), "%s/frames-%05u.capture", capture.folder, capture.frame);
	capture.file = gzopen(capture.path, "wb6");
	if (!capture.file)
	{
		capture_log("render capture: cannot write %s", capture.path);
		capture.windows_done++;
		return;
	}
	capture.epoch++;
	capture.window_end = capture.frame + capture.windows[index].count;
	capture.records = 0;
	capture.failed = 0;
	capture.constants_sent = capture.uniforms_sent = 0;
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, GPU_CAPTURE_MAGIC, sizeof(GPU_CAPTURE_MAGIC));
	header.format = GPU_CAPTURE_FORMAT;
	header.interface_hash = capture.host.interface_hash;
	header.front_end_hash = capture.host.front_end_hash;
	header.draw_size = sizeof(struct gpu_draw);
	header.uniforms_size = sizeof(struct gpu_uniforms);
	header.stereo_size = sizeof(struct gpu_stereo_present);
	header.description_size = sizeof(struct gpu_texture_description);
	header.clear_size = sizeof(struct gpu_clear);
	header.capabilities_size = sizeof(struct gpu_capabilities);
	header.initialize_flags = capture.initialize_flags;
	header.capabilities = capture.capabilities;
	header.first_frame = capture.frame;
	header.frame_count = capture.windows[index].count;
	snprintf(header.guest_sha256, sizeof(header.guest_sha256), "%s", capture.host.guest_sha256 ? capture.host.guest_sha256 : "");
	snprintf(header.label, sizeof(header.label), "%s", capture.host.label ? capture.host.label : "");
	if (gzwrite(capture.file, &header, sizeof(header)) != (int)sizeof(header))
		capture.failed = 1;
	capture_log("render capture: frames %u to %u go to %s", capture.frame, capture.window_end - 1, capture.path);
}

/* ---------- what a call uses, written before the call */

static uint32_t level_dimension(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

static uint64_t level_bytes(const struct gpu_texture_description *description, uint32_t level)
{
	uint64_t width = level_dimension(description->width, level), height = level_dimension(description->height, level);
	uint64_t depth = description->type == GPU_TEXTURE_3D ? level_dimension(description->depth, level) : 1;

	if (description->format == GPU_FORMAT_BC1)
		return ((width + 3) / 4) * ((height + 3) / 4) * 8 * depth;
	if (description->format == GPU_FORMAT_BC2 || description->format == GPU_FORMAT_BC3)
		return ((width + 3) / 4) * ((height + 3) / 4) * 16 * depth;
	return width * height * 4 * depth;
}

static void put_upload(gpu_texture handle, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	uint32_t words[4] = { handle, face, level, size };

	put(GPU_CAPTURE_TEXTURE_UPLOAD, words, 4, NULL, 0, data, size);
}

static void ensure_texture(gpu_texture handle)
{
	struct texture_entry *entry = texture_entry(handle);
	uint32_t face, level;

	if (!entry || entry->epoch == capture.epoch)
		return;
	entry->epoch = capture.epoch;
	put(GPU_CAPTURE_TEXTURE_CREATE, &handle, 1, &entry->description, sizeof(entry->description), NULL, 0);
	for (face = 0; face < 6; face++)
		for (level = 0; level < LEVELS; level++)
			if (entry->data[face][level])
				put_upload(handle, face, level, entry->data[face][level], entry->size[face][level]);
	if (entry->mipmaps_from && entry->description.usage == GPU_USAGE_UPLOAD)
	{
		uint32_t words[2] = { handle, entry->mipmaps_from - 1 };

		put(GPU_CAPTURE_TEXTURE_MIPMAPS, words, 2, NULL, 0, NULL, 0);
	}
}

static void ensure_shader(gpu_shader handle)
{
	struct shader_entry *entry = shader_entry(handle);
	uint32_t words[3];

	if (!entry || entry->epoch == capture.epoch || !entry->text)
		return;
	entry->epoch = capture.epoch;
	words[0] = handle;
	words[1] = entry->stage;
	words[2] = (uint32_t)strlen(entry->text);
	put(GPU_CAPTURE_SHADER_CREATE, words, 3, NULL, 0, entry->text, words[2]);
}

/* the pages of a mirror segment that hold [offset, offset + size), from the
game's memory as it is now: the front end has just made them current for the
draw that reads them (mirror_range) */
static void ensure_range(gpu_buffer handle, uint64_t offset, uint64_t size)
{
	struct buffer_entry *entry = buffer_entry(handle);
	uint64_t page, first, last;

	if (!entry || !size || offset >= entry->size)
		return;
	if (entry->epoch != capture.epoch)
	{
		uint32_t words[2] = { handle, entry->size };

		entry->epoch = capture.epoch;
		put(GPU_CAPTURE_BUFFER_CREATE, words, 2, NULL, 0, NULL, 0);
	}
	if (!entry->source || entry->source_unknown)
		return;
	if (offset + size > entry->size)
		size = entry->size - offset;
	first = offset >> PAGE_SHIFT;
	last = (offset + size - 1) >> PAGE_SHIFT;
	for (page = first; page <= last; page++)
	{
		uint64_t run = page;
		uint32_t words[4];

		if (!entry->written[page] || entry->page_epoch[page] == capture.epoch)
			continue;
		/* a run of pages in one record */
		while (run <= last && entry->written[run] && entry->page_epoch[run] != capture.epoch)
			entry->page_epoch[run++] = capture.epoch;
		words[0] = handle;
		words[1] = (uint32_t)(page << PAGE_SHIFT);
		words[2] = (uint32_t)((run << PAGE_SHIFT) > entry->size ? entry->size - (page << PAGE_SHIFT) : (run - page) << PAGE_SHIFT);
		words[3] = GPU_WRITE_UNUSED;
		put(GPU_CAPTURE_BUFFER_WRITE, words, 4, NULL, 0, entry->source + words[1], words[2]);
		page = run - 1;
	}
}

/* the bytes at offset of a buffer, when they can be read: a streamed
range's, or a mirror segment's (which are written first) */
static const uint8_t *buffer_bytes(gpu_buffer handle, uint64_t offset, uint64_t size)
{
	struct buffer_entry *entry = buffer_entry(handle);
	unsigned long back;

	if (entry)
	{
		ensure_range(handle, offset, size);
		return entry->source && !entry->source_unknown && offset + size <= entry->size ? entry->source + offset : NULL;
	}
	for (back = 1; back <= STREAM_RANGES && back <= capture.range_next; back++)
	{
		const struct stream_range *range = &capture.ranges[(capture.range_next - back) % STREAM_RANGES];

		if (range->buffer == handle && offset >= range->offset && offset + size <= (uint64_t)range->offset + range->size)
			return range->data + (offset - range->offset);
	}
	return NULL;
}

static void ensure_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants, const struct gpu_uniforms *uniforms)
{
	int64_t first = 0, last = (int64_t)draw->count - 1;
	int stage, stream;
	uint16_t streams_used = 0;

	ensure_texture(draw->color_target);
	ensure_texture(draw->depth_target);
	ensure_shader(draw->vertex_shader);
	ensure_shader(draw->pixel_shader);
	for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
		if (draw->stages[stage].type)
			ensure_texture(draw->stages[stage].texture);
	if (draw->index_buffer && draw->count)
	{
		const uint8_t *indices = buffer_bytes(draw->index_buffer, draw->index_offset, (uint64_t)draw->count * 2);

		/* without the indices, every vertex they could name */
		first = 0;
		last = 0xffff;
		if (indices)
		{
			uint32_t index, low = 0xffff, high = 0;

			for (index = 0; index < draw->count; index++)
			{
				uint16_t value;

				memcpy(&value, indices + index * 2u, sizeof(value));
				if (value < low)
					low = value;
				if (value > high)
					high = value;
			}
			first = low;
			last = high;
		}
		first += draw->base_vertex;
		last += draw->base_vertex;
		if (first < 0)
			first = 0;
	}
	for (stage = 0; stage < GPU_ATTRIBUTE_COUNT; stage++)
		if (draw->attributes[stage].stream < 16)
			streams_used |= (uint16_t)(1u << draw->attributes[stage].stream);
	for (stream = 0; stream < 16 && last >= first; stream++)
	{
		const struct gpu_vertex_stream *packet = &draw->streams[stream];
		uint32_t stride = packet->stride ? packet->stride : 64;

		if ((streams_used & (1u << stream)) && packet->buffer && buffer_entry(packet->buffer))
			ensure_range(packet->buffer, packet->offset + (uint64_t)first * stride, (uint64_t)(last - first + 1) * stride);
	}
	/* the constants that changed since the last draw in the file; all of
	them the first time */
	if (!capture.constants_sent || capture.constants_serial != constants->serial)
	{
		static uint32_t changes[GPU_CONSTANT_COUNT * 5];
		uint32_t count = 0, reg;

		for (reg = 0; reg < GPU_CONSTANT_COUNT; reg++)
		{
			if (capture.constants_sent && !memcmp(capture.constants[reg], constants->c[reg], sizeof(capture.constants[reg])))
				continue;
			memcpy(capture.constants[reg], constants->c[reg], sizeof(capture.constants[reg]));
			changes[count * 5] = reg;
			memcpy(&changes[count * 5 + 1], constants->c[reg], 16);
			count++;
		}
		capture.constants_sent = 1;
		capture.constants_serial = constants->serial;
		if (count)
			put(GPU_CAPTURE_CONSTANTS, &count, 1, NULL, 0, changes, count * 20);
	}
	if (!capture.uniforms_sent || memcmp(&capture.uniforms, uniforms, sizeof(*uniforms)))
	{
		capture.uniforms_sent = 1;
		capture.uniforms = *uniforms;
		put(GPU_CAPTURE_UNIFORMS, NULL, 0, uniforms, sizeof(*uniforms), NULL, 0);
	}
}

/* ---------- the backend's functions */

static gpu_texture capture_texture_create(const struct gpu_texture_description *description)
{
	gpu_texture handle = capture.backend.texture_create(description);
	struct texture_entry *entry = description ? table_entry(&capture.textures, handle, 1) : NULL;

	if (entry)
	{
		int face, level;

		for (face = 0; face < 6; face++)
			for (level = 0; level < LEVELS; level++)
				free(entry->data[face][level]);
		memset(entry, 0, sizeof(*entry));
		entry->live = 1;
		entry->description = *description;
	}
	return handle;
}

static void capture_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	struct texture_entry *entry = texture_entry(texture);

	if (entry && data && face < 6 && level < LEVELS && armed())
	{
		uint64_t wanted = level_bytes(&entry->description, level);
		uint32_t kept = size < wanted ? size : (uint32_t)wanted;

		free(entry->data[face][level]);
		entry->data[face][level] = malloc(kept ? kept : 1);
		entry->size[face][level] = entry->data[face][level] ? kept : 0;
		if (entry->data[face][level])
			memcpy(entry->data[face][level], data, kept);
		if (capture.file && entry->epoch == capture.epoch)
			put_upload(texture, face, level, data, kept);
	}
	capture.backend.texture_upload(texture, face, level, data, size);
}

static void capture_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	if (capture.file)
	{
		uint32_t words[3] = { source, destination, level };

		ensure_texture(source);
		ensure_texture(destination);
		put(GPU_CAPTURE_TEXTURE_COPY_LEVEL, words, 3, NULL, 0, NULL, 0);
	}
	capture.backend.texture_copy_level(source, destination, level);
}

static void capture_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	struct texture_entry *entry = texture_entry(texture);

	if (entry)
		entry->mipmaps_from = base_level + 1;
	if (capture.file)
	{
		uint32_t words[2] = { texture, base_level };

		ensure_texture(texture);
		put(GPU_CAPTURE_TEXTURE_MIPMAPS, words, 2, NULL, 0, NULL, 0);
	}
	capture.backend.texture_generate_mipmaps(texture, base_level);
}

static void capture_texture_destroy(gpu_texture texture)
{
	struct texture_entry *entry = texture_entry(texture);

	if (entry)
	{
		int face, level;

		if (capture.file && entry->epoch == capture.epoch)
			put(GPU_CAPTURE_TEXTURE_DESTROY, &texture, 1, NULL, 0, NULL, 0);
		for (face = 0; face < 6; face++)
			for (level = 0; level < LEVELS; level++)
			{
				free(entry->data[face][level]);
				entry->data[face][level] = NULL;
			}
		entry->live = 0;
	}
	capture.backend.texture_destroy(texture);
}

static uint32_t capture_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	return capture.backend.texture_read(texture, pixels, size);
}

static gpu_buffer capture_buffer_create(uint32_t size)
{
	gpu_buffer handle = capture.backend.buffer_create(size);
	struct buffer_entry *entry = table_entry(&capture.buffers, handle, 1);

	if (entry)
	{
		free(entry->written);
		free(entry->page_epoch);
		memset(entry, 0, sizeof(*entry));
		entry->live = 1;
		entry->size = size;
		entry->written = calloc((size >> PAGE_SHIFT) + 1, 1);
		entry->page_epoch = calloc((size >> PAGE_SHIFT) + 1, sizeof(*entry->page_epoch));
		if (!entry->written || !entry->page_epoch)
			entry->live = 0;
	}
	return handle;
}

static void capture_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	struct buffer_entry *entry = buffer_entry(buffer);

	if (entry && data && size && offset <= entry->size && size <= entry->size - offset)
	{
		const uint8_t *source = (const uint8_t *)data - offset;
		uint32_t page;

		if (entry->source && entry->source != source)
			entry->source_unknown = 1;
		entry->source = source;
		for (page = offset >> PAGE_SHIFT; page <= (offset + size - 1) >> PAGE_SHIFT; page++)
			entry->written[page] = 1;
		/* a segment the file already has takes the write as it comes; one
		it does not have yet is read from the game's memory when a draw
		first uses it */
		if (capture.file && entry->epoch == capture.epoch)
		{
			uint32_t words[4] = { buffer, offset, size, flags };

			put(GPU_CAPTURE_BUFFER_WRITE, words, 4, NULL, 0, data, size);
			for (page = offset >> PAGE_SHIFT; page <= (offset + size - 1) >> PAGE_SHIFT; page++)
				if ((page << PAGE_SHIFT) >= offset && ((uint64_t)(page + 1) << PAGE_SHIFT) <= (uint64_t)offset + size)
					entry->page_epoch[page] = capture.epoch;
		}
	}
	capture.backend.buffer_write(buffer, offset, size, data, flags);
}

static void capture_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	if (capture.file)
	{
		uint32_t words[2] = { vertex_bytes, index_bytes };

		put(GPU_CAPTURE_STREAM_RESERVE, words, 2, NULL, 0, NULL, 0);
	}
	capture.backend.stream_reserve(vertex_bytes, index_bytes);
}

static uint32_t capture_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	uint32_t offset = capture.backend.stream(kind, data, size, buffer);

	if (buffer && data)
	{
		struct stream_range *range = &capture.ranges[capture.range_next++ % STREAM_RANGES];

		range->buffer = *buffer;
		range->offset = offset;
		range->size = size;
		range->data = data;
		if (capture.file)
		{
			uint32_t words[4] = { kind, size, *buffer, offset };

			put(GPU_CAPTURE_STREAM, words, 4, NULL, 0, data, size);
		}
	}
	return offset;
}

static gpu_shader capture_shader_create(uint32_t stage, const char *source)
{
	gpu_shader handle = capture.backend.shader_create(stage, source);
	struct shader_entry *entry = source ? table_entry(&capture.shaders, handle, 1) : NULL;

	if (entry)
	{
		free(entry->text);
		memset(entry, 0, sizeof(*entry));
		entry->live = 1;
		entry->stage = (uint8_t)stage;
		entry->text = strdup(source);
	}
	return handle;
}

static void capture_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
	if (capture.file && clear && (rectangles || !count) && count < 4096)
	{
		ensure_texture(clear->color_target);
		ensure_texture(clear->depth_target);
		put(GPU_CAPTURE_CLEAR, &count, 1, clear, sizeof(*clear), rectangles, count * (uint32_t)sizeof(*rectangles));
	}
	capture.backend.clear(clear, rectangles, count);
}

static uint32_t capture_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	uint32_t drawn;

	if (capture.file && draw && constants && uniforms)
		ensure_draw(draw, constants, uniforms);
	drawn = capture.backend.draw(draw, constants, uniforms);
	if (capture.file && draw && constants && uniforms)
		put(GPU_CAPTURE_DRAW, &drawn, 1, draw, sizeof(*draw), NULL, 0);
	return drawn;
}

static void capture_visibility_begin(void)
{
	put(GPU_CAPTURE_VISIBILITY_BEGIN, NULL, 0, NULL, 0, NULL, 0);
	capture.backend.visibility_begin();
}

static void capture_visibility_end(uint32_t slot)
{
	put(GPU_CAPTURE_VISIBILITY_END, &slot, 1, NULL, 0, NULL, 0);
	capture.backend.visibility_end(slot);
}

static uint32_t capture_visibility_result(uint32_t slot, uint32_t *samples)
{
	put(GPU_CAPTURE_VISIBILITY_RESULT, &slot, 1, NULL, 0, NULL, 0);
	return capture.backend.visibility_result(slot, samples);
}

static void capture_flush(void)
{
	put(GPU_CAPTURE_FLUSH, NULL, 0, NULL, 0, NULL, 0);
	capture.backend.flush();
}

/* (the front end's vertical blank thread: nothing of the capture's is touched) */
static uint32_t capture_vertical_blank_wait(void)
{
	return capture.backend.vertical_blank_wait();
}

static void frame_end(void)
{
	capture.frame++;
	if (capture.file && capture.frame >= capture.window_end)
		window_close();
	window_open_if_due();
}

static uint32_t capture_present(gpu_texture back_buffer)
{
	uint32_t due;

	if (capture.file)
	{
		ensure_texture(back_buffer);
		put(GPU_CAPTURE_PRESENT, &back_buffer, 1, NULL, 0, NULL, 0);
	}
	due = capture.backend.present(back_buffer);
	frame_end();
	return due;
}

static uint32_t capture_present_stereo(const struct gpu_stereo_present *present)
{
	uint32_t due;

	if (capture.file && present)
	{
		int index;

		for (index = 0; index < 2; index++)
		{
			ensure_texture(present->eye_color[index]);
			ensure_texture(present->eye_depth[index]);
		}
		ensure_texture(present->hud);
		ensure_texture(present->zoom);
		ensure_texture(present->reticle_layer);
		ensure_texture(present->ui);
		for (index = 0; index < HALO_HUD_GROUP_COUNT; index++)
			ensure_texture(present->hud_group[index]);
		put(GPU_CAPTURE_PRESENT_STEREO, NULL, 0, present, sizeof(*present), NULL, 0);
	}
	due = capture.backend.present_stereo(present);
	frame_end();
	return due;
}

static uint32_t capture_call_count_take(void)
{
	return capture.backend.call_count_take();
}

static uint32_t capture_warm_list_read(const char *name, char *text, uint32_t size)
{
	return capture.backend.warm_list_read(name, text, size);
}

static void capture_warm_begin(void)
{
	capture.backend.warm_begin();
}

static void capture_pipeline_warm(const struct gpu_pipeline_description *description)
{
	capture.backend.pipeline_warm(description);
}

static void capture_warm_end(void)
{
	capture.backend.warm_end();
}

static uint32_t capture_pipeline_built_take(struct gpu_pipeline_description *description)
{
	return capture.backend.pipeline_built_take(description);
}

/* display.native_hud's stretch and display.anti_aliasing's passes are not
recorded yet: a replay of a window that holds them leaves them out, which a
line in the log says once a capture */
static void capture_unrecorded(const char *call)
{
	if (capture.file && !capture.unrecorded_logged)
	{
		capture.unrecorded_logged = 1;
		capture_log("render capture: %s is not recorded; a replay of this window leaves it out", call);
	}
}

static void capture_upscale(gpu_texture source, gpu_texture destination, const struct gpu_rect *area, uint32_t anti_alias)
{
	capture_unrecorded("gpu_upscale");
	capture.backend.upscale(source, destination, area, anti_alias);
}

static uint32_t capture_anti_alias_prepare(uint32_t method, const char *smaa_shader, const void *area_texels,
	const void *search_texels)
{
	return capture.backend.anti_alias_prepare(method, smaa_shader, area_texels, search_texels);
}

static void capture_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect)
{
	capture_unrecorded("gpu_anti_alias");
	capture.backend.anti_alias(method, target, rect);
}

static void capture_frame_context(uint32_t scene_class, uint32_t frame_rate, const char *level)
{
	capture.backend.frame_context(scene_class, frame_rate, level);
}

static uint32_t capture_compiles_outstanding(void)
{
	return capture.backend.compiles_outstanding();
}

static void capture_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	capture.backend.initialize(flags, capabilities);
	capture.initialize_flags = flags;
	if (capabilities)
		capture.capabilities = *capabilities;
	window_open_if_due();
}

/* ---------- installing */

void gpu_capture_install(struct gpu_backend *table, const struct gpu_capture_host *host)
{
	const char *at;

	if (!table || !host || !host->folder || !*host->folder || !host->frames || capture.installed)
		return;
	memset(&capture, 0, sizeof(capture));
	capture.host = *host;
	snprintf(capture.folder, sizeof(capture.folder), "%s", host->folder);
	for (at = host->frames; *at && capture.window_count < WINDOWS;)
	{
		char *end;
		unsigned long first = strtoul(at, &end, 10), count = 1;

		if (end == at)
			break;
		if (*end == '+')
			count = strtoul(end + 1, &end, 10);
		if (count)
		{
			capture.windows[capture.window_count].first = (uint32_t)first;
			capture.windows[capture.window_count].count = (uint32_t)count;
			capture.window_count++;
		}
		at = *end == ',' ? end + 1 : end;
		if (*end && *end != ',')
			break;
	}
	if (!capture.window_count)
	{
		capture_log("render capture: \"%s\" names no frames (\"600+30,1200+30\"); nothing is captured", host->frames);
		return;
	}
	capture.backend = *table;
	capture.textures.size = sizeof(struct texture_entry);
	capture.buffers.size = sizeof(struct buffer_entry);
	capture.shaders.size = sizeof(struct shader_entry);
	capture.installed = 1;
#define INSTALL_FUNCTION(type, name, parameters, arguments) table->name = capture_##name;
#define INSTALL_PROCEDURE(name, parameters, arguments) table->name = capture_##name;
	GPU_FUNCTIONS(INSTALL_FUNCTION, INSTALL_PROCEDURE)
#undef INSTALL_FUNCTION
#undef INSTALL_PROCEDURE
	atexit(gpu_capture_finish);
	capture_log("render capture: %u window%s (%s) into %s", capture.window_count, capture.window_count == 1 ? "" : "s",
		host->frames, capture.folder);
}

#endif
