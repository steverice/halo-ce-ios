/*
GPU_VALIDATE.C

The validator of gpu_validate.h: a struct gpu_backend whose every function
checks its arguments, then calls the backend's. It believes only what
crosses gpu.h (the handles, sizes and bytes the front end passed), never the
front end's own records: a wrong offset the front end computed is caught
here, as innerhat's host catches one in a packet (tag
test-v0.3.0-net28-mac13, host_metal.mm and metal_draw_encoder.mm; the checks
taken from it are marked "his").

Nothing here copies vertex or index data. A mirror segment's bytes are the
game's memory the front end uploaded them from (gpu_buffer_write's data
pointer minus its offset is the segment's base), and a streamed range's are
the bytes gpu_stream was given; both are read in place. What the mirror
holds changes only when a page is uploaded again, so a scan of it (an index
range's smallest and largest index, a vertex range's floats) is kept until
then, as d3d8_device.c's index_extent keeps its own.

Host only: it is compiled into the app, never into the guest.
*/

#include "gpu_validate.h"

#if HALO_RENDER_VALIDATION && !HALO_ILP32

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- what the validator knows */

#define PAGE_SHIFT 12
#define HANDLE_LIMIT (1u << 22)

struct texture_entry
{
	uint8_t live, unsupported;
	struct gpu_texture_description description;
	/* the levels the description can have */
	uint32_t levels;
	/* the levels each face has been uploaded (bit n: level n) */
	uint32_t uploaded[6];
};

enum { BUFFER_NONE, BUFFER_PERSISTENT, BUFFER_TRANSIENT };

struct buffer_entry
{
	uint8_t kind;
	/* the uploads disagree about where the buffer's bytes come from, so
	its contents are not read */
	uint8_t source_unknown;
	uint32_t size;
	/* the game's memory this buffer mirrors: byte n is source[n] */
	const uint8_t *source;
	/* per page: the write that last uploaded it, 0 for never */
	uint32_t *pages;
};

struct shader_entry
{
	uint8_t live, stage;
	/* vertex: the attributes the shader reads, and which of them packed */
	uint16_t attributes, packed;
	/* vertex: the constant registers it reads by number, and whether it
	also reads some by a computed index (skinning) */
	uint64_t constants[3];
	uint8_t constants_indexed;
	/* pixel: the stages it samples and each one's GPU_TEXTURE_* */
	uint8_t sampled, types[GPU_STAGE_COUNT];
	uint32_t hash;
};

struct table
{
	void *entries;
	uint32_t count;
	size_t size;
};

/* a range gpu_stream copied this frame */
struct stream_range
{
	gpu_buffer buffer;
	uint32_t offset, size, kind;
	const uint8_t *data;
	uint64_t frame;
};

/* a scan of mirrored bytes that found nothing wrong, and an index range's extent */
struct scan_entry
{
	uint64_t key;
	uint32_t write, minimum, maximum;
};

enum { STREAM_RANGES = 256, SCAN_ENTRIES = 8192, CAUSE_ENTRIES = 1024 };

struct cause_entry
{
	uint32_t reason;
	uint64_t cause;
	unsigned long count;
	char first[384];
};

static struct
{
	struct gpu_backend backend;
	struct gpu_validation_host host;
	struct gpu_capabilities capabilities;
	uint8_t allowed[GPU_VALIDATION_REASON_COUNT];
	struct table textures, buffers, shaders;
	struct stream_range ranges[STREAM_RANGES];
	unsigned long range_next;
	struct scan_entry scans[SCAN_ENTRIES];
	uint32_t writes;
	/* where the run is */
	uint64_t frame;
	unsigned long pass, draw;
	gpu_texture pass_color, pass_depth;
	/* the call being checked */
	const struct gpu_draw *current;
	struct gpu_validation_identity identity;
	int identity_valid;
	const char *call;
	/* reports made during the call being forwarded */
	unsigned long reported;
	/* the constants and uniforms last looked at */
	uint64_t constants_serial;
	int constants_seen;
	uint64_t constants_nonfinite[3];
	uint32_t uniforms_serial;
	int uniforms_seen;
	/* the uniforms last looked at had a nonfinite value: every draw with
	them is a failure, not only the first */
	int uniforms_nonfinite;
	int visibility_active;
	/* findings */
	struct cause_entry causes[CAUSE_ENTRIES];
	unsigned long cause_count, cause_overflow;
	unsigned long counts[GPU_VALIDATION_REASON_COUNT];
	/* failures so far, and the calls a REJECT run kept from the backend */
	unsigned long failures, rejected;
	unsigned long summarized;
	int installed, stopping;
} validation;

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;

#define REASON_NAME(name, text, class) text,
static const char *const reason_names[GPU_VALIDATION_REASON_COUNT] = { GPU_VALIDATION_REASONS(REASON_NAME) };
#undef REASON_NAME
#define REASON_CLASS(name, text, class) class,
static const uint8_t reason_classes[GPU_VALIDATION_REASON_COUNT] = { GPU_VALIDATION_REASONS(REASON_CLASS) };
#undef REASON_CLASS
static const char *const class_names[] = { "FAILURE", "FALLBACK", "warning", "expected" };

const char *gpu_validation_reason_name(uint32_t reason)
{
	return reason < GPU_VALIDATION_REASON_COUNT ? reason_names[reason] : "unknown-reason";
}

unsigned long gpu_validation_count(uint32_t reason)
{
	return reason < GPU_VALIDATION_REASON_COUNT ? validation.counts[reason] : 0;
}

unsigned long gpu_validation_rejected(void)
{
	return validation.rejected;
}

unsigned long gpu_validation_failures(void)
{
	return validation.failures;
}

/* in a REJECT run: whether the call being checked has had a failure since
before (the failure count when its checks began), so is not passed on */
static int rejected(unsigned long before)
{
	if (validation.host.mode != GPU_VALIDATION_REJECT || validation.failures == before)
		return 0;
	validation.rejected++;
	return 1;
}

/* ---------- reporting */

static void emit(const char *line)
{
	if (validation.host.log)
		validation.host.log(line);
	else
		fprintf(stderr, "%s\n", line);
}

/* "frame 12 pass 3 draw 41 (indexed, vs059_0 ps_6c143490, from 0x8012abcc)" */
static void where(char *text, size_t size)
{
	int length = snprintf(text, size, "frame %llu pass %lu %s %lu", (unsigned long long)validation.frame, validation.pass,
		validation.call ? validation.call : "call", validation.draw);

	if (length < 0 || (size_t)length >= size)
		return;
	if (!validation.current && !validation.identity_valid)
		return;
	if (validation.current)
		length += snprintf(text + length, size - (size_t)length, " (shaders %u/%u", validation.current->vertex_shader,
			validation.current->pixel_shader);
	else
		length += snprintf(text + length, size - (size_t)length, " (");
	if ((size_t)length >= size)
		return;
	if (validation.identity_valid)
	{
		static const char *const kinds[] = { "", "vertices", "indexed", "immediate", "clear" };
		const struct gpu_validation_identity *identity = &validation.identity;

		if (identity->kind == GPU_VALIDATION_DRAW_CLEAR)
			length += snprintf(text + length, size - (size_t)length, "%sclear, from %#x", validation.current ? ": " : "",
				identity->caller);
		else
			length += snprintf(text + length, size - (size_t)length, ": %s, vs%03u_%u ps_%08x, from %#x",
				identity->kind < 5 ? kinds[identity->kind] : "", identity->vertex_program, identity->vertex_variant,
				identity->pixel_key, identity->caller);
		if ((size_t)length >= size)
			return;
	}
	snprintf(text + length, size - (size_t)length, ")");
}

void gpu_validation_report(uint32_t reason, uint64_t cause, const char *format, ...)
{
	char detail[256], place[160], line[512];
	struct cause_entry *entry = NULL;
	unsigned long index;
	va_list arguments;
	int stop;

	if (reason >= GPU_VALIDATION_REASON_COUNT || !validation.installed)
		return;
	pthread_mutex_lock(&report_lock);
	validation.reported++;
	validation.counts[reason]++;
	if (reason_classes[reason] == GPU_VALIDATION_FAILURE)
		validation.failures++;
	for (index = 0; index < validation.cause_count; index++)
	{
		if (validation.causes[index].reason == reason && validation.causes[index].cause == cause)
		{
			validation.causes[index].count++;
			pthread_mutex_unlock(&report_lock);
			return;
		}
	}
	va_start(arguments, format);
	vsnprintf(detail, sizeof(detail), format, arguments);
	va_end(arguments);
	where(place, sizeof(place));
	snprintf(line, sizeof(line), "render validation: %s %s: %s: %s", class_names[reason_classes[reason]], reason_names[reason],
		place, detail);
	if (validation.cause_count < CAUSE_ENTRIES)
	{
		entry = &validation.causes[validation.cause_count++];
		entry->reason = reason;
		entry->cause = cause;
		entry->count = 1;
		snprintf(entry->first, sizeof(entry->first), "%s: %s", place, detail);
	}
	else
	{
		validation.cause_overflow++;
	}
	stop = validation.host.mode == GPU_VALIDATION_STRICT && reason_classes[reason] <= GPU_VALIDATION_FALLBACK &&
		!validation.allowed[reason] && !validation.stopping;
	if (stop)
		validation.stopping = 1;
	pthread_mutex_unlock(&report_lock);
	/* a cause past the table is still logged, each time */
	emit(line);
	if (stop)
	{
		gpu_validation_summary();
		snprintf(line, sizeof(line), "render validation: strict mode stops the run at its first %s: %s: %s: %s",
			reason_classes[reason] == GPU_VALIDATION_FAILURE ? "failure" : "fallback", reason_names[reason], place, detail);
		if (validation.host.stop)
			validation.host.stop(line);
		emit(line);
		exit(3);
	}
}

void gpu_validation_summary(void)
{
	unsigned long index, total[4] = { 0, 0, 0, 0 };
	char line[640];

	if (!validation.installed)
		return;
	pthread_mutex_lock(&report_lock);
	for (index = 0; index < GPU_VALIDATION_REASON_COUNT; index++)
		total[reason_classes[index]] += validation.counts[index];
	snprintf(line, sizeof(line), "render validation: summary after %llu frames: %lu failures, %lu fallbacks, %lu warnings, "
		"%lu expected, in %lu distinct causes%s", (unsigned long long)validation.frame, total[GPU_VALIDATION_FAILURE],
		total[GPU_VALIDATION_FALLBACK], total[GPU_VALIDATION_WARNING], total[GPU_VALIDATION_EXPECTED], validation.cause_count,
		validation.cause_overflow ? " (and more than the table holds)" : "");
	emit(line);
	for (index = 0; index < validation.cause_count; index++)
	{
		const struct cause_entry *entry = &validation.causes[index];

		snprintf(line, sizeof(line), "render validation:   %lu x %s %s, first at %s", entry->count,
			class_names[reason_classes[entry->reason]], reason_names[entry->reason], entry->first);
		emit(line);
	}
	pthread_mutex_unlock(&report_lock);
}

static void summary_at_exit(void)
{
	if (!validation.stopping)
		gpu_validation_summary();
}

#define REPORT(reason, cause, ...) gpu_validation_report(GPU_VALIDATION_##reason, (uint64_t)(cause), __VA_ARGS__)

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
	struct texture_entry *entry = table_entry(&validation.textures, handle, 0);

	return entry && entry->live ? entry : NULL;
}

static struct buffer_entry *buffer_entry(gpu_buffer handle)
{
	struct buffer_entry *entry = table_entry(&validation.buffers, handle, 0);

	return entry && entry->kind ? entry : NULL;
}

static struct shader_entry *shader_entry(gpu_shader handle)
{
	struct shader_entry *entry = table_entry(&validation.shaders, handle, 0);

	return entry && entry->live ? entry : NULL;
}

/* ---------- floats, by their bits: reading them as floats would be the bug
this looks for */

enum { FLOAT_FINE, FLOAT_DENORMAL, FLOAT_HUGE, FLOAT_NONFINITE };

/* a magnitude of 2^60 (1.2e18) or more: no coordinate, color or matrix of
the game's is near it, and squaring one overflows */
#define FLOAT_HUGE_EXPONENT (127 + 60)

static inline int float_class(uint32_t bits)
{
	uint32_t exponent = (bits >> 23) & 0xff;

	if (exponent == 0xff)
		return FLOAT_NONFINITE;
	if (exponent >= FLOAT_HUGE_EXPONENT)
		return FLOAT_HUGE;
	if (!exponent && (bits << 1))
		return FLOAT_DENORMAL;
	return FLOAT_FINE;
}

static inline uint32_t float_bits(const void *address)
{
	uint32_t bits;

	memcpy(&bits, address, sizeof(bits));
	return bits;
}

static int floats_worst(const void *floats, unsigned long count, unsigned long *which)
{
	unsigned long index;
	int worst = FLOAT_FINE;

	for (index = 0; index < count; index++)
	{
		int class = float_class(float_bits((const uint8_t *)floats + index * 4));

		if (class > worst)
		{
			worst = class;
			*which = index;
		}
	}
	return worst;
}

/* ---------- textures */

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
	if (description->format == GPU_FORMAT_BC2 || description->format == GPU_FORMAT_BC3 ||
		description->format == GPU_FORMAT_ASTC4X4)
		return ((width + 3) / 4) * ((height + 3) / 4) * 16 * depth;
	return width * height * 4 * depth;
}

static gpu_texture validate_texture_create(const struct gpu_texture_description *description)
{
	gpu_texture handle;
	struct texture_entry *entry;
	uint32_t largest, possible = 1;
	int unsupported = 0;
	unsigned long before = validation.failures;

	validation.call = "texture_create";
	if (!description)
	{
		REPORT(TEXTURE_DESCRIPTION_INVALID, 0, "no description");
		return 0;
	}
	largest = description->width > description->height ? description->width : description->height;
	if (description->type == GPU_TEXTURE_3D && description->depth > largest)
		largest = description->depth;
	while (possible < 32 && (largest >> possible))
		possible++;
	/* his make_texture: sizes, the mip count within log2, square cube faces,
	render targets 2D */
	if (description->type < GPU_TEXTURE_2D || description->type > GPU_TEXTURE_CUBE ||
		description->usage < GPU_USAGE_UPLOAD || description->usage > GPU_USAGE_RENDER_TARGET ||
		!description->width || !description->height || (description->type == GPU_TEXTURE_3D && !description->depth) ||
		(description->type == GPU_TEXTURE_CUBE && description->width != description->height) ||
		(description->usage == GPU_USAGE_RENDER_TARGET && description->type != GPU_TEXTURE_2D) ||
		(description->format == GPU_FORMAT_DEPTH_STENCIL && description->usage != GPU_USAGE_RENDER_TARGET) ||
		description->levels > 32)
		REPORT(TEXTURE_DESCRIPTION_INVALID, description->type | description->usage << 8 | description->format << 16,
			"type %u format %u usage %u, %ux%ux%u", description->type, description->format, description->usage,
			description->width, description->height, description->depth);
	if (description->format < GPU_FORMAT_BGRA8 || description->format > GPU_FORMAT_ASTC4X4 ||
		(description->format >= GPU_FORMAT_BC1 && description->format <= GPU_FORMAT_BC3 &&
			(!validation.capabilities.s3tc || description->usage == GPU_USAGE_RENDER_TARGET)) ||
		(description->format == GPU_FORMAT_ASTC4X4 && (!validation.capabilities.astc ||
			description->usage != GPU_USAGE_UPLOAD || description->type == GPU_TEXTURE_3D)))
	{
		unsupported = 1;
		REPORT(TEXTURE_FORMAT_UNSUPPORTED, description->format | description->usage << 8,
			"format %u (usage %u, S3TC %u, ASTC %u) is not one the backend takes; the texture samples black", description->format,
			description->usage, validation.capabilities.s3tc, validation.capabilities.astc);
	}
	if (description->levels < 1 || description->levels > possible)
		REPORT(TEXTURE_LEVELS_CLAMPED, description->levels > possible, "%u levels for a %ux%ux%u texture, which has %u",
			description->levels, description->width, description->height, description->depth, possible);
	if (validation.capabilities.max_texture_size && largest > validation.capabilities.max_texture_size)
		REPORT(TEXTURE_TOO_LARGE, largest, "%ux%ux%u is larger than the GPU's %u", description->width, description->height,
			description->depth, validation.capabilities.max_texture_size);
	if (rejected(before))
		return 0;
	handle = validation.backend.texture_create(description);
	entry = table_entry(&validation.textures, handle, 1);
	if (entry)
	{
		memset(entry, 0, sizeof(*entry));
		entry->live = 1;
		entry->unsupported = (uint8_t)unsupported;
		entry->description = *description;
		entry->levels = description->levels < 1 ? 1 : description->levels > possible ? possible : description->levels;
	}
	return handle;
}

static void validate_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	struct texture_entry *entry = texture_entry(texture);
	unsigned long before = validation.failures;

	validation.call = "texture_upload";
	if (!entry)
		REPORT(UPLOAD_INVALID, 1, "texture %u is not a live texture", texture);
	else if (entry->description.usage != GPU_USAGE_UPLOAD)
		REPORT(UPLOAD_INVALID, 2, "texture %u is a render target", texture);
	else if (!data)
		REPORT(UPLOAD_INVALID, 3, "texture %u: no data", texture);
	else if (level >= entry->levels || face >= (entry->description.type == GPU_TEXTURE_CUBE ? 6u : 1u))
		REPORT(UPLOAD_INVALID, 4, "texture %u: face %u level %u of a texture with %u levels", texture, face, level, entry->levels);
	/* his validate_upload wants the exact size; ours has always passed the
	source's size, which may be longer */
	else if (size < level_bytes(&entry->description, level))
		REPORT(UPLOAD_SHORT, entry->description.format, "texture %u face %u level %u: %u bytes for a level of %llu", texture,
			face, level, size, (unsigned long long)level_bytes(&entry->description, level));
	else if (!entry->unsupported)
		entry->uploaded[face] |= 1u << level;
	if (rejected(before))
		return;
	validation.backend.texture_upload(texture, face, level, data, size);
}

static void validate_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	struct texture_entry *from = texture_entry(source), *to = texture_entry(destination);
	unsigned long before = validation.failures;

	validation.call = "texture_copy_level";
	/* his copy: source and destination distinct, formats equal, in bounds */
	if (!from || !to || source == destination)
		REPORT(COPY_INVALID, 1, "from texture %u to texture %u", source, destination);
	else if (level >= to->levels || from->description.format != to->description.format ||
		from->description.type != GPU_TEXTURE_2D || to->description.type != GPU_TEXTURE_2D ||
		level_dimension(to->description.width, level) > from->description.width ||
		level_dimension(to->description.height, level) > from->description.height)
		REPORT(COPY_INVALID, 2, "level 0 of texture %u (%ux%u, format %u) into level %u of texture %u (%ux%u, %u levels, format %u)",
			source, from->description.width, from->description.height, from->description.format, level, destination,
			to->description.width, to->description.height, to->levels, to->description.format);
	else
		to->uploaded[0] |= 1u << level;
	if (rejected(before))
		return;
	validation.backend.texture_copy_level(source, destination, level);
}

static void validate_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	struct texture_entry *entry = texture_entry(texture);
	unsigned long before = validation.failures;

	validation.call = "texture_generate_mipmaps";
	if (!entry || entry->description.type != GPU_TEXTURE_2D || base_level >= entry->levels)
		REPORT(MIPMAPS_INVALID, entry != NULL, "texture %u from level %u (%u levels)", texture, base_level, entry ? entry->levels : 0);
	else
		entry->uploaded[0] |= ~0u << base_level;
	if (rejected(before))
		return;
	validation.backend.texture_generate_mipmaps(texture, base_level);
}

static void validate_texture_destroy(gpu_texture texture)
{
	struct texture_entry *entry = texture_entry(texture);
	unsigned long before = validation.failures;

	validation.call = "texture_destroy";
	if (!entry)
		REPORT(DESTROY_UNKNOWN, 0, "texture %u is not a live texture", texture);
	else
		entry->live = 0;
	if (rejected(before))
		return;
	validation.backend.texture_destroy(texture);
}

static uint32_t validate_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	validation.call = "texture_read";
	return validation.backend.texture_read(texture, pixels, size);
}

/* ---------- buffers and streams */

static gpu_buffer validate_buffer_create(uint32_t size)
{
	unsigned long before = validation.failures;
	gpu_buffer handle;
	struct buffer_entry *entry;

	validation.call = "buffer_create";
	/* (the mirror's segments are 4 MB) */
	if (!size || size > (1u << 28))
		REPORT(BUFFER_SIZE_INVALID, size != 0, "a buffer of %u bytes", size);
	if (rejected(before))
		return 0;
	handle = validation.backend.buffer_create(size);
	entry = table_entry(&validation.buffers, handle, 1);
	if (entry)
	{
		free(entry->pages);
		memset(entry, 0, sizeof(*entry));
		entry->kind = BUFFER_PERSISTENT;
		entry->size = size;
		entry->pages = calloc((size >> PAGE_SHIFT) + 1, sizeof(*entry->pages));
	}
	return handle;
}

static void validate_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	struct buffer_entry *entry = buffer_entry(buffer);
	unsigned long before = validation.failures;

	validation.call = "buffer_write";
	if (!entry || entry->kind != BUFFER_PERSISTENT)
		REPORT(BUFFER_UNKNOWN, 1, "write to buffer %u, which gpu_buffer_create did not make", buffer);
	else if (!data || !size || offset > entry->size || size > entry->size - offset)
		REPORT(BUFFER_WRITE_OUT_OF_RANGE, 0, "%u bytes at %u of buffer %u (%u bytes)", size, offset, buffer, entry->size);
	else if (entry->pages)
	{
		const uint8_t *source = (const uint8_t *)data - offset;
		uint32_t page, write = ++validation.writes;

		if (!write)
			write = ++validation.writes;
		if (entry->source && entry->source != source)
			entry->source_unknown = 1;
		entry->source = source;
		/* a write that ends inside a page still makes that page's start
		readable: the mirror writes whole pages, and anything else is
		checked by its bytes' own range */
		for (page = offset >> PAGE_SHIFT; page <= (offset + size - 1) >> PAGE_SHIFT; page++)
			entry->pages[page] = write;
	}
	if (rejected(before))
		return;
	validation.backend.buffer_write(buffer, offset, size, data, flags);
}

static void validate_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	validation.call = "stream_reserve";
	validation.backend.stream_reserve(vertex_bytes, index_bytes);
}

static uint32_t validate_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	struct stream_range *range;
	struct buffer_entry *entry;
	uint32_t offset;
	unsigned long before = validation.failures;

	validation.call = "stream";
	if ((kind != GPU_STREAM_KIND_VERTEX && kind != GPU_STREAM_KIND_INDEX) || !data || !size || !buffer)
		REPORT(STREAM_INVALID, kind, "kind %u, %u bytes at %p", kind, size, data);
	if (rejected(before))
	{
		if (buffer)
			*buffer = 0;
		return 0;
	}
	offset = validation.backend.stream(kind, data, size, buffer);
	if (!buffer || !data)
		return offset;
	entry = table_entry(&validation.buffers, *buffer, 1);
	if (entry && entry->kind == BUFFER_NONE)
		entry->kind = BUFFER_TRANSIENT;
	range = &validation.ranges[validation.range_next++ % STREAM_RANGES];
	range->buffer = *buffer;
	range->offset = offset;
	range->size = size;
	range->kind = kind;
	range->data = data;
	range->frame = validation.frame;
	return offset;
}

/* the bytes readable from offset of a buffer, as far as needed: their count
(needed or fewer) and, when the validator can read them, where they are.
why: what stopped it short */
enum { EXTENT_FINE, EXTENT_UNKNOWN_BUFFER, EXTENT_STALE, EXTENT_PAST_END, EXTENT_NOT_UPLOADED };

static const char *const extent_names[] = { "", "the buffer is unknown", "nothing streamed there this frame",
	"it runs past the end of its buffer or streamed range", "it reaches a page the mirror never uploaded" };

static int buffer_extent(gpu_buffer buffer, uint64_t offset, uint64_t needed, uint64_t *available, const uint8_t **bytes,
	uint32_t *write)
{
	struct buffer_entry *entry = buffer_entry(buffer);

	*available = 0;
	*bytes = NULL;
	*write = 0;
	if (!entry)
		return EXTENT_UNKNOWN_BUFFER;
	if (entry->kind == BUFFER_TRANSIENT)
	{
		unsigned long back;

		/* the newest range that holds the offset: a buffer's offsets start
		again when it fills within a frame */
		for (back = 1; back <= STREAM_RANGES && back <= validation.range_next; back++)
		{
			const struct stream_range *range = &validation.ranges[(validation.range_next - back) % STREAM_RANGES];

			if (range->buffer != buffer || offset < range->offset || offset >= (uint64_t)range->offset + range->size)
				continue;
			if (range->frame != validation.frame)
				return EXTENT_STALE;
			*available = (uint64_t)range->offset + range->size - offset;
			*bytes = range->data + (offset - range->offset);
			if (*available >= needed)
			{
				*available = needed;
				return EXTENT_FINE;
			}
			return EXTENT_PAST_END;
		}
		return EXTENT_STALE;
	}
	if (offset >= entry->size)
		return EXTENT_PAST_END;
	{
		uint64_t end = offset + needed > entry->size ? entry->size : offset + needed, page;
		uint32_t newest = 0;

		for (page = offset >> PAGE_SHIFT; page <= (end - 1) >> PAGE_SHIFT; page++)
		{
			if (!entry->pages || !entry->pages[page])
			{
				*available = (page << PAGE_SHIFT) > offset ? (page << PAGE_SHIFT) - offset : 0;
				return EXTENT_NOT_UPLOADED;
			}
			if (entry->pages[page] > newest)
				newest = entry->pages[page];
		}
		*available = end - offset;
		*write = newest;
		if (entry->source && !entry->source_unknown)
			*bytes = entry->source + offset;
		return end - offset >= needed ? EXTENT_FINE : EXTENT_PAST_END;
	}
}

/* ---------- shaders: what a translated shader reads, from its text
(nv2a_vsh.c, nv2a_psh.c and nv2a_msl.c write every read the same way) */

static int identifier_character(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void shader_parse_vertex(struct shader_entry *entry, const char *source)
{
	const char *at;

	/* an attribute is read as vN.xyzw, a packed one declared vN_packed */
	for (at = source; (at = strchr(at, 'v')) != NULL; at++)
	{
		char *end;
		unsigned long index;

		if ((at > source && identifier_character(at[-1])) || at[1] < '0' || at[1] > '9')
			continue;
		index = strtoul(at + 1, &end, 10);
		if (index >= GPU_ATTRIBUTE_COUNT)
			continue;
		if (*end == '.')
			entry->attributes |= (uint16_t)(1u << index);
		else if (!strncmp(end, "_packed", 7))
			entry->packed |= (uint16_t)(1u << index);
	}
	/* a constant is read as c[N], or by a computed index */
	for (at = source; (at = strstr(at, "c[")) != NULL; at += 2)
	{
		char *end;
		unsigned long index;

		if (at > source && identifier_character(at[-1]))
			continue;
		if (at[2] < '0' || at[2] > '9')
		{
			entry->constants_indexed = 1;
			continue;
		}
		index = strtoul(at + 2, &end, 10);
		/* (the declaration is c[192]) */
		if (*end == ']' && index < GPU_CONSTANT_COUNT)
			entry->constants[index / 64] |= 1ull << (index % 64);
	}
}

static void shader_parse_pixel(struct shader_entry *entry, const char *source)
{
	int stage;

	for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
	{
		char text[48];

		entry->types[stage] = GPU_TEXTURE_2D;
		snprintf(text, sizeof(text), "samplerCube tex%d;", stage);
		if (strstr(source, text))
			entry->types[stage] = GPU_TEXTURE_CUBE;
		snprintf(text, sizeof(text), "sampler3D tex%d;", stage);
		if (strstr(source, text))
			entry->types[stage] = GPU_TEXTURE_3D;
		snprintf(text, sizeof(text), "texturecube<float> tex%d ", stage);
		if (strstr(source, text))
			entry->types[stage] = GPU_TEXTURE_CUBE;
		snprintf(text, sizeof(text), "texture3d<float> tex%d ", stage);
		if (strstr(source, text))
			entry->types[stage] = GPU_TEXTURE_3D;
		/* texture(texN, ...) and texture_foveated(N, texN, ...) */
		snprintf(text, sizeof(text), "texture(tex%d,", stage);
		if (strstr(source, text))
			entry->sampled |= (uint8_t)(1u << stage);
		snprintf(text, sizeof(text), "texture_foveated(%d, tex%d,", stage, stage);
		if (strstr(source, text))
			entry->sampled |= (uint8_t)(1u << stage);
	}
}

static gpu_shader validate_shader_create(uint32_t stage, const char *source)
{
	gpu_shader handle;
	struct shader_entry *entry;
	unsigned long before = validation.failures;

	validation.call = "shader_create";
	if ((stage != GPU_SHADER_VERTEX && stage != GPU_SHADER_PIXEL) || !source || !*source)
		REPORT(SHADER_COMPILE_FAILED, 1, "stage %u, source %s", stage, source ? "empty" : "missing");
	if (rejected(before))
		return 0;
	handle = validation.backend.shader_create(stage, source);
	if (!handle)
	{
		REPORT(SHADER_COMPILE_FAILED, stage << 8, "the backend could not compile a %s shader",
			stage == GPU_SHADER_VERTEX ? "vertex" : "pixel");
		return 0;
	}
	entry = table_entry(&validation.shaders, handle, 1);
	if (entry && source)
	{
		const char *at;

		memset(entry, 0, sizeof(*entry));
		entry->live = 1;
		entry->stage = (uint8_t)stage;
		entry->hash = 2166136261u;
		for (at = source; *at; at++)
			entry->hash = (entry->hash ^ (uint8_t)*at) * 16777619u;
		if (stage == GPU_SHADER_VERTEX)
			shader_parse_vertex(entry, source);
		else
			shader_parse_pixel(entry, source);
	}
	return handle;
}

/* ---------- targets, shared by draws and clears. Returns the target's size
(0 when it has none to check against: no target, or a foveated eye's, whose
viewports are in its map's screen coordinates, gpu.h) */

static int check_targets(gpu_texture color, gpu_texture depth, uint32_t *width, uint32_t *height)
{
	struct texture_entry *color_entry = texture_entry(color), *depth_entry = texture_entry(depth);
	const struct texture_entry *size;
	int fine = 1;

	*width = *height = 0;
	if (validation.pass_color != color || validation.pass_depth != depth)
	{
		validation.pass++;
		validation.pass_color = color;
		validation.pass_depth = depth;
	}
	/* his "Draw has no original attachment" */
	if (!color && !depth)
	{
		REPORT(TARGET_NONE, 0, "neither a color target nor a depth target");
		return 0;
	}
	if ((color && !color_entry) || (depth && !depth_entry))
	{
		REPORT(TARGET_UNKNOWN, 0, "color target %u%s, depth target %u%s", color, color && !color_entry ? " is not a live texture" : "",
			depth, depth && !depth_entry ? " is not a live texture" : "");
		return 0;
	}
	/* his "attachments must be single-sample 2D targets" (every gpu.h
	texture is single-sample) and the two format messages */
	if ((color_entry && color_entry->description.usage != GPU_USAGE_RENDER_TARGET) ||
		(depth_entry && depth_entry->description.usage != GPU_USAGE_RENDER_TARGET))
	{
		REPORT(TARGET_NOT_RENDER_TARGET, 0, "color target %u (usage %u), depth target %u (usage %u)", color,
			color_entry ? color_entry->description.usage : 0, depth, depth_entry ? depth_entry->description.usage : 0);
		fine = 0;
	}
	if ((color_entry && (color_entry->description.format != GPU_FORMAT_BGRA8 || color_entry->description.type != GPU_TEXTURE_2D)) ||
		(depth_entry && (depth_entry->description.format != GPU_FORMAT_DEPTH_STENCIL ||
			depth_entry->description.type != GPU_TEXTURE_2D)))
	{
		REPORT(TARGET_FORMAT, (color_entry ? color_entry->description.format : 0) | (depth_entry ? depth_entry->description.format << 8 : 0),
			"color target %u has format %u, depth target %u has format %u", color,
			color_entry ? color_entry->description.format : 0, depth, depth_entry ? depth_entry->description.format : 0);
		fine = 0;
	}
	if (color_entry && depth_entry)
	{
		const struct gpu_texture_description *c = &color_entry->description, *d = &depth_entry->description;

		/* his rule: the depth target covers the color target */
		if (d->width < c->width || d->height < c->height)
		{
			REPORT(DEPTH_TARGET_SMALLER, ((uint64_t)c->width << 32 | c->height) ^ ((uint64_t)d->width << 48 | (uint64_t)d->height << 16),
				"color target %u is %ux%u, depth target %u is %ux%u", color, c->width, c->height, depth, d->width, d->height);
			fine = 0;
		}
		else if (d->width != c->width || d->height != c->height)
			REPORT(DEPTH_TARGET_LARGER, ((uint64_t)c->width << 32 | c->height) ^ ((uint64_t)d->width << 48 | (uint64_t)d->height << 16),
				"color target %u is %ux%u, depth target %u is %ux%u", color, c->width, c->height, depth, d->width, d->height);
		if (c->foveated_eye != d->foveated_eye)
		{
			REPORT(TARGET_FOVEATION_MISMATCH, c->foveated_eye | d->foveated_eye << 8,
				"color target %u is eye %u's, depth target %u is eye %u's", color, c->foveated_eye, depth, d->foveated_eye);
			fine = 0;
		}
	}
	size = color_entry ? color_entry : depth_entry;
	if (!size->description.foveated_eye)
	{
		*width = size->description.width;
		*height = size->description.height;
	}
	return fine;
}

/* 0 inside, 1 partly outside, 2 wholly outside */
static int rect_outside(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t target_width, uint32_t target_height)
{
	int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;

	if (right <= 0 || bottom <= 0 || x >= (int64_t)target_width || y >= (int64_t)target_height)
		return 2;
	return x < 0 || y < 0 || right > (int64_t)target_width || bottom > (int64_t)target_height;
}

/* ---------- clears */

static void validate_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
	uint32_t width, height, index;
	unsigned long before = validation.failures;

	validation.call = "clear";
	validation.current = NULL;
	validation.draw++;
	if (!clear || (count && !rectangles))
	{
		REPORT(CLEAR_INVALID, 0, "no clear or no rectangles");
		return;
	}
	check_targets(clear->color_target, clear->depth_target, &width, &height);
	/* his clear: planes nonzero and a subset of 7, the channel mask at most
	15 and nonzero with the color plane, depth finite and in 0..1 */
	if (clear->flags & ~7u || clear->channel_mask & ~15u || clear->stencil > 0xffu)
		REPORT(CLEAR_INVALID, 1, "flags %#x, channel mask %#x, stencil %#x", clear->flags, clear->channel_mask, clear->stencil);
	if (!clear->flags || !count || ((clear->flags & GPU_CLEAR_COLOR) && !clear->channel_mask && clear->flags == GPU_CLEAR_COLOR))
		REPORT(CLEAR_EMPTY, clear->flags, "flags %#x, channel mask %#x, %u rectangles: nothing is cleared", clear->flags,
			clear->channel_mask, count);
	if ((clear->flags & (GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL)) && !clear->depth_target)
		REPORT(CLEAR_DEPTH_WITHOUT_TARGET, clear->flags, "flags %#x without a depth target", clear->flags);
	if ((clear->flags & GPU_CLEAR_DEPTH) && (float_class(float_bits(&clear->depth)) == FLOAT_NONFINITE ||
		!(clear->depth >= 0.0f && clear->depth <= 1.0f)))
		REPORT(CLEAR_DEPTH_RANGE, 0, "depth %g", clear->depth);
	for (index = 0; index < count; index++)
	{
		const struct gpu_rect *rect = &rectangles[index];

		if (rect->width <= 0 || rect->height <= 0)
			REPORT(CLEAR_EMPTY, 0x100, "rectangle %d,%d %dx%d", rect->x, rect->y, rect->width, rect->height);
		else if (width && rect_outside(rect->x, rect->y, rect->width, rect->height, width, height))
			REPORT(CLEAR_RECT_OUTSIDE_TARGET, 0, "rectangle %d,%d %dx%d in a %ux%u target", rect->x, rect->y, rect->width,
				rect->height, width, height);
	}
	if (!rejected(before))
		validation.backend.clear(clear, rectangles, count);
	validation.identity_valid = 0;
}

/* ---------- draws */

static uint32_t attribute_bytes(uint8_t format)
{
	if (format >= GPU_ATTRIBUTE_FLOAT1 && format <= GPU_ATTRIBUTE_FLOAT4)
		return 4u * (format - GPU_ATTRIBUTE_FLOAT1 + 1u);
	if (format >= GPU_ATTRIBUTE_SHORT1 && format <= GPU_ATTRIBUTE_SHORT4)
		return 2u * (format - GPU_ATTRIBUTE_SHORT1 + 1u);
	if (format >= GPU_ATTRIBUTE_NORMSHORT1 && format <= GPU_ATTRIBUTE_NORMSHORT4)
		return 2u * (format - GPU_ATTRIBUTE_NORMSHORT1 + 1u);
	if (format >= GPU_ATTRIBUTE_UBYTE1 && format <= GPU_ATTRIBUTE_UBYTE4)
		return format - GPU_ATTRIBUTE_UBYTE1 + 1u;
	return 4;
}

static struct scan_entry *scan_slot(uint64_t a, uint64_t b, uint64_t c, uint64_t *key)
{
	uint64_t hash = (a * 0x9e3779b97f4a7c15ull) ^ (b * 0xc2b2ae3d27d4eb4full) ^ (c * 0x165667b19e3779f9ull);

	hash ^= hash >> 29;
	*key = hash ? hash : 1;
	return &validation.scans[hash % SCAN_ENTRIES];
}

/* the smallest and largest of count 16-bit indices; a mirrored range's are
kept until one of its pages is uploaded again */
static void index_extent(const struct gpu_draw *draw, const uint8_t *bytes, uint32_t write, uint32_t *minimum, uint32_t *maximum)
{
	uint64_t key;
	struct scan_entry *slot = scan_slot(draw->index_buffer, draw->index_offset, draw->count | 1ull << 40, &key);
	uint32_t index, low = 0xffff, high = 0;

	if (write && slot->key == key && slot->write == write)
	{
		*minimum = slot->minimum;
		*maximum = slot->maximum;
		return;
	}
	for (index = 0; index < draw->count; index++)
	{
		uint16_t value;

		memcpy(&value, bytes + index * 2u, sizeof(value));
		if (value < low)
			low = value;
		if (value > high)
			high = value;
	}
	if (write)
	{
		slot->key = key;
		slot->write = write;
		slot->minimum = low;
		slot->maximum = high;
	}
	*minimum = low;
	*maximum = high;
}

/* every float of one attribute over vertices [first, last]; a mirrored
range that is clean is not scanned again until it is uploaded again */
static void check_attribute_floats(const struct gpu_draw *draw, unsigned attribute, const uint8_t *bytes, uint32_t stride,
	uint32_t components, uint64_t first, uint64_t last, uint32_t write)
{
	const struct gpu_vertex_stream *stream = &draw->streams[draw->attributes[attribute].stream];
	uint64_t key, vertex;
	struct scan_entry *slot = scan_slot(stream->buffer | (uint64_t)stride << 32,
		stream->offset + first * stride + draw->attributes[attribute].offset, (last - first) << 3 | components | 2ull << 40, &key);
	int clean = 1;

	if (write && slot->key == key && slot->write == write)
		return;
	for (vertex = first; vertex <= last; vertex++)
	{
		const uint8_t *floats = bytes + (vertex - first) * stride;
		uint32_t component;

		for (component = 0; component < components; component++)
		{
			uint32_t bits = float_bits(floats + component * 4u);
			int class = float_class(bits);

			if (class == FLOAT_FINE)
				continue;
			clean = 0;
			/* his "nonfinite vertex: vertex, register, component, raw" */
			if (class == FLOAT_NONFINITE)
				REPORT(VERTEX_NONFINITE, attribute, "attribute %u of vertex %llu, component %u, is %#010x (stream %u, buffer %u)",
					attribute, (unsigned long long)vertex, component, bits, draw->attributes[attribute].stream, stream->buffer);
			else if (class == FLOAT_HUGE)
				REPORT(VERTEX_HUGE, attribute, "attribute %u of vertex %llu, component %u, is %g", attribute,
					(unsigned long long)vertex, component, (double)*(const float *)(const void *)&bits);
			else
				REPORT(VERTEX_DENORMAL, attribute, "attribute %u of vertex %llu, component %u, is the denormal %#010x", attribute,
					(unsigned long long)vertex, component, bits);
		}
	}
	if (write && clean)
	{
		slot->key = key;
		slot->write = write;
	}
}

static void check_geometry(const struct gpu_draw *draw, const struct shader_entry *vertex)
{
	/* the vertices the draw reads: [first, last] */
	int64_t first = 0, last = (int64_t)draw->count - 1;
	unsigned attribute;
	uint16_t used = vertex ? vertex->attributes | vertex->packed : 0xffff;
	/* streams already found short: one finding a stream, not one an attribute */
	uint16_t short_streams = 0;

	if (draw->primitive > GPU_PRIMITIVE_TRIANGLE_FAN)
		REPORT(PRIMITIVE_INVALID, draw->primitive, "primitive %u", draw->primitive);
	else if ((draw->primitive == GPU_PRIMITIVE_TRIANGLE_FAN && !validation.capabilities.triangle_fans) ||
		(draw->primitive == GPU_PRIMITIVE_LINE_LOOP && !validation.capabilities.line_loops))
		REPORT(PRIMITIVE_UNSUPPORTED, draw->primitive, "primitive %u, which the backend said it does not draw", draw->primitive);
	if (!draw->count)
	{
		REPORT(DRAW_EMPTY, 0, "no vertices");
		return;
	}
	/* (indices are 16-bit and the game's largest draws a few thousand
	triangles; a count like this is a corrupt packet, and a GPU hang) */
	if (draw->count > (1u << 24))
	{
		REPORT(DRAW_TOO_LARGE, 0, "%u vertices", draw->count);
		return;
	}
	/* his topology check: whole primitives */
	if ((draw->primitive == GPU_PRIMITIVE_TRIANGLES && draw->count % 3) || (draw->primitive == GPU_PRIMITIVE_LINES && draw->count % 2) ||
		((draw->primitive == GPU_PRIMITIVE_TRIANGLE_STRIP || draw->primitive == GPU_PRIMITIVE_TRIANGLE_FAN) && draw->count < 3) ||
		((draw->primitive == GPU_PRIMITIVE_LINE_STRIP || draw->primitive == GPU_PRIMITIVE_LINE_LOOP) && draw->count < 2))
		REPORT(PRIMITIVE_INCOMPLETE, draw->primitive, "%u vertices for primitive %u", draw->count, draw->primitive);
	if (!draw->index_buffer)
	{
		if (draw->base_vertex)
			REPORT(INDEX_BELOW_ZERO, 1, "base vertex %d on a draw without indices", draw->base_vertex);
	}
	else
	{
		uint64_t available;
		const uint8_t *bytes;
		uint32_t write, minimum, maximum;
		int why = buffer_extent(draw->index_buffer, draw->index_offset, (uint64_t)draw->count * 2, &available, &bytes, &write);

		if (draw->index_offset & 1)
			REPORT(INDEX_OFFSET_MISALIGNED, 0, "index offset %u is odd", draw->index_offset);
		if (why == EXTENT_UNKNOWN_BUFFER)
		{
			REPORT(BUFFER_UNKNOWN, 2, "index buffer %u", draw->index_buffer);
			return;
		}
		if (why != EXTENT_FINE)
		{
			gpu_validation_report(why == EXTENT_STALE ? GPU_VALIDATION_STREAM_STALE : GPU_VALIDATION_INDEX_RANGE_OUT_OF_RANGE,
				(uint64_t)why | 0x100, "%u indices at %u of buffer %u: %s (%llu bytes are there)", draw->count, draw->index_offset,
				draw->index_buffer, extent_names[why], (unsigned long long)available);
			return;
		}
		if (draw->base_vertex && !validation.capabilities.base_vertex)
			REPORT(BASE_VERTEX_UNSUPPORTED, 0, "base vertex %d, which the backend said it does not take", draw->base_vertex);
		if (!bytes)
			return;
		/* his "index exceeds its vertex range": every index, against what
		each stream holds (below) */
		index_extent(draw, bytes, write, &minimum, &maximum);
		first = (int64_t)minimum + draw->base_vertex;
		last = (int64_t)maximum + draw->base_vertex;
		if (first < 0)
		{
			REPORT(INDEX_BELOW_ZERO, 0, "index %u with base vertex %d is vertex %lld", minimum, draw->base_vertex, (long long)first);
			return;
		}
	}
	for (attribute = 0; attribute < GPU_ATTRIBUTE_COUNT; attribute++)
	{
		const struct gpu_vertex_attribute *packet = &draw->attributes[attribute];
		const struct gpu_vertex_stream *stream;
		uint32_t size, stride, write;
		uint64_t needed, available, start;
		const uint8_t *bytes;
		int why;

		/* every attribute's format and stream (the backends index the
		streams with it); the data of those the shader reads */
		if (packet->format < GPU_ATTRIBUTE_FLOAT1 || packet->format > GPU_ATTRIBUTE_NORMPACKED3 ||
			(packet->stream >= 16 && packet->stream != GPU_STREAM_CONSTANT) ||
			(packet->format == GPU_ATTRIBUTE_BGRA8 && !validation.capabilities.vertex_bgra))
		{
			REPORT(ATTRIBUTE_INVALID, packet->format | packet->stream << 8, "attribute %u has format %u and stream %u", attribute,
				packet->format, packet->stream);
			continue;
		}
		if (!(used & (1u << attribute)))
			continue;
		if (packet->stream == GPU_STREAM_CONSTANT)
		{
			unsigned long which = 0;

			if (packet->format <= GPU_ATTRIBUTE_FLOAT4 &&
				floats_worst(draw->constant_values[attribute], 4, &which) == FLOAT_NONFINITE)
				REPORT(ATTRIBUTE_CONSTANT_NONFINITE, attribute, "attribute %u's constant value, component %lu, is %#010x", attribute,
					which, float_bits(&draw->constant_values[attribute][which]));
			continue;
		}
		stream = &draw->streams[packet->stream];
		if (!stream->buffer)
		{
			REPORT(STREAM_UNBOUND, attribute, "attribute %u reads stream %u, which has no buffer", attribute, packet->stream);
			continue;
		}
		size = attribute_bytes(packet->format);
		stride = stream->stride ? stream->stride : size;
		if (stream->stride && (uint32_t)packet->offset + size > stream->stride)
			REPORT(ATTRIBUTE_OUTSIDE_STRIDE, attribute, "attribute %u is %u bytes at %u of a %u-byte vertex", attribute, size,
				packet->offset, stream->stride);
		start = stream->offset + (uint64_t)first * stride + packet->offset;
		needed = (uint64_t)(last - first) * stride + size;
		why = buffer_extent(stream->buffer, start, needed, &available, &bytes, &write);
		if (why == EXTENT_UNKNOWN_BUFFER)
		{
			REPORT(BUFFER_UNKNOWN, 3, "attribute %u reads stream %u's buffer %u", attribute, packet->stream, stream->buffer);
			continue;
		}
		if (why == EXTENT_STALE)
		{
			REPORT(STREAM_STALE, attribute, "attribute %u reads %llu bytes at %llu of buffer %u: %s", attribute,
				(unsigned long long)needed, (unsigned long long)start, stream->buffer, extent_names[why]);
			continue;
		}
		if (why != EXTENT_FINE)
		{
			uint64_t vertices = available >= size ? (available - size) / stride + 1 : 0;

			if (short_streams & (1u << packet->stream))
				continue;
			short_streams |= (uint16_t)(1u << packet->stream);
			if (draw->index_buffer)
				REPORT(INDEX_OUT_OF_RANGE, packet->stream, "the largest index with base vertex %d is vertex %lld, but attribute %u's "
					"stream %u (buffer %u at %u, stride %u) holds %llu vertices from vertex %lld: %s", draw->base_vertex,
					(long long)last, attribute, packet->stream, stream->buffer, stream->offset, stream->stride,
					(unsigned long long)vertices, (long long)first, extent_names[why]);
			else
				REPORT(VERTEX_RANGE_OUT_OF_RANGE, packet->stream, "%u vertices, but attribute %u's stream %u (buffer %u at %u, stride %u) "
					"holds %llu: %s", draw->count, attribute, packet->stream, stream->buffer, stream->offset, stream->stride,
					(unsigned long long)vertices, extent_names[why]);
			continue;
		}
		if (bytes && packet->format <= GPU_ATTRIBUTE_FLOAT4)
			check_attribute_floats(draw, attribute, bytes, stride, size / 4, (uint64_t)first, (uint64_t)last, write);
	}
}

static void check_stages(const struct gpu_draw *draw, const struct shader_entry *pixel)
{
	int stage;

	for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
	{
		const struct gpu_stage *packet = &draw->stages[stage];
		const struct gpu_sampler_state *sampler = &packet->sampler;
		const struct texture_entry *texture;
		uint32_t face, faces, complete;

		/* a stage the shader does not sample is not looked at, as in his
		validateBindings */
		if (pixel && !(pixel->sampled & (1u << stage)))
			continue;
		if (!packet->type)
		{
			if (pixel)
				REPORT(SAMPLER_UNBOUND, stage | (uint64_t)pixel->hash << 8, "the pixel shader samples stage %d, which has no "
					"texture; it samples black", stage);
			continue;
		}
		texture = texture_entry(packet->texture);
		/* his "Used original texture is absent, mismatched or aliases a draw
		attachment" */
		if (!texture)
		{
			REPORT(TEXTURE_UNKNOWN, stage, "stage %d's texture %u is not a live texture", stage, packet->texture);
			continue;
		}
		if (packet->type != texture->description.type || (pixel && pixel->types[stage] != packet->type))
			REPORT(TEXTURE_TYPE_MISMATCH, stage | packet->type << 8 | texture->description.type << 16 |
				(pixel ? pixel->types[stage] << 24 : 0), "stage %d is bound as type %u, its texture %u is type %u, the shader "
				"samples type %u", stage, packet->type, packet->texture, texture->description.type, pixel ? pixel->types[stage] : 0);
		if (packet->texture == draw->color_target || packet->texture == draw->depth_target)
			REPORT(TEXTURE_IS_TARGET, stage, "stage %d samples texture %u, which the draw also renders into", stage, packet->texture);
		if (texture->unsupported)
			REPORT(TEXTURE_FORMAT_UNSUPPORTED, texture->description.format | 0x10000, "stage %d samples texture %u, whose format "
				"%u the backend does not take; it samples black", stage, packet->texture, texture->description.format);
		else if (texture->description.format == GPU_FORMAT_DEPTH_STENCIL)
			REPORT(TEXTURE_DEPTH_SAMPLED, stage, "stage %d samples the depth target %u", stage, packet->texture);
		else if (texture->description.usage == GPU_USAGE_UPLOAD)
		{
			/* his HALO_METAL_UNDEFINED_CONTENT: every level a lookup can
			reach has been uploaded */
			faces = texture->description.type == GPU_TEXTURE_CUBE ? 6 : 1;
			complete = texture->levels >= 32 ? ~0u : (1u << texture->levels) - 1u;
			for (face = 0; face < faces; face++)
			{
				if (!(texture->uploaded[face] & 1u))
				{
					REPORT(TEXTURE_NO_STORAGE, stage, "stage %d samples texture %u, of which face %u was never uploaded; it "
						"samples black", stage, packet->texture, face);
					break;
				}
				if (sampler->mip_filter != GPU_FILTER_NONE && (texture->uploaded[face] & complete) != complete)
				{
					REPORT(TEXTURE_INCOMPLETE_MIPS, stage, "stage %d samples texture %u with mipmaps, but face %u has levels %#x "
						"of %u", stage, packet->texture, face, texture->uploaded[face], texture->levels);
					break;
				}
			}
		}
		/* his sampler_valid, with our enums */
		if (sampler->min_filter > GPU_FILTER_GAUSSIAN_CUBIC || sampler->mag_filter > GPU_FILTER_GAUSSIAN_CUBIC ||
			sampler->mip_filter > GPU_FILTER_GAUSSIAN_CUBIC || sampler->address_u > GPU_ADDRESS_CLAMP_TO_EDGE ||
			sampler->address_v > GPU_ADDRESS_CLAMP_TO_EDGE || sampler->address_w > GPU_ADDRESS_CLAMP_TO_EDGE ||
			sampler->min_filter == GPU_FILTER_NONE || sampler->mag_filter == GPU_FILTER_NONE)
			REPORT(SAMPLER_STATE_INVALID, sampler->min_filter | sampler->mag_filter << 8 | sampler->mip_filter << 16,
				"stage %d: filters %u/%u/%u, addressing %u/%u/%u", stage, sampler->min_filter, sampler->mag_filter,
				sampler->mip_filter, sampler->address_u, sampler->address_v, sampler->address_w);
		else if (sampler->min_filter >= GPU_FILTER_QUINCUNX || sampler->mag_filter >= GPU_FILTER_ANISOTROPIC ||
			sampler->mip_filter >= GPU_FILTER_ANISOTROPIC)
			REPORT(FILTER_UNSUPPORTED, sampler->min_filter | sampler->mag_filter << 8 | sampler->mip_filter << 16,
				"stage %d: filters %u/%u/%u (minification, magnification, mip) are sampled as linear", stage, sampler->min_filter,
				sampler->mag_filter, sampler->mip_filter);
		if (sampler->min_filter == GPU_FILTER_ANISOTROPIC && sampler->max_anisotropy > 16)
			REPORT(ANISOTROPY_CLAMPED, sampler->max_anisotropy, "stage %d: anisotropy %u is sampled as 16", stage,
				sampler->max_anisotropy);
		/* (the Metal backend reports no border clamp, so that its line of
		capabilities reads as OpenGL ES's, and samples borders all the same
		where the GPU has them: it says so itself where it has not) */
		if (!validation.capabilities.border_clamp && validation.capabilities.shader_language != GPU_SHADER_LANGUAGE_MSL &&
			(sampler->address_u == GPU_ADDRESS_BORDER ||
			sampler->address_v == GPU_ADDRESS_BORDER || (packet->type == GPU_TEXTURE_3D && sampler->address_w == GPU_ADDRESS_BORDER)))
			REPORT(BORDER_UNSUPPORTED, 0, "stage %d: BORDER addressing clamps to the edge on this GPU", stage);
		if (sampler->max_mip_level >= texture->levels && sampler->max_mip_level)
			REPORT(SAMPLER_MIP_OUT_OF_RANGE, 0, "stage %d: first level %u of texture %u, which has %u", stage,
				sampler->max_mip_level, packet->texture, texture->levels);
		if (float_class(float_bits(&sampler->lod_bias)) == FLOAT_NONFINITE)
			REPORT(SAMPLER_LOD_BIAS_NONFINITE, stage, "stage %d: LOD bias %#010x", stage, float_bits(&sampler->lod_bias));
	}
}

static void check_state(const struct gpu_draw *draw, uint32_t width, uint32_t height)
{
	const struct gpu_depth_stencil_state *depth = &draw->depth_stencil;
	const struct gpu_blend_state *blend = &draw->blend;
	const struct gpu_raster_state *raster = &draw->raster;
	const struct gpu_viewport *viewport = &draw->viewport;
	const struct gpu_rect *scissor = &draw->scissor;

	/* his "Invalid or unsupported numeric original draw state": every enum
	in range */
	if (depth->depth_function > GPU_COMPARE_ALWAYS || depth->stencil_function > GPU_COMPARE_ALWAYS ||
		depth->stencil_fail > GPU_STENCIL_DECREMENT_WRAP || depth->stencil_depth_fail > GPU_STENCIL_DECREMENT_WRAP ||
		depth->stencil_pass > GPU_STENCIL_DECREMENT_WRAP || blend->source > GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA ||
		blend->destination > GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA || blend->operation > GPU_BLEND_OP_MAX ||
		blend->alpha_source > GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA || blend->alpha_destination > GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA ||
		blend->color_write_mask > 15 || raster->cull_mode > GPU_CULL_BACK || raster->front_face > GPU_FRONT_COUNTER_CLOCKWISE ||
		raster->fill_mode > GPU_FILL_POINT)
		REPORT(STATE_INVALID, 0, "depth function %u, stencil %u/%u/%u/%u, blend %u/%u/%u (alpha %u/%u), write mask %#x, cull %u, "
			"front %u, fill %u", depth->depth_function, depth->stencil_function, depth->stencil_fail, depth->stencil_depth_fail,
			depth->stencil_pass, blend->source, blend->destination, blend->operation, blend->alpha_source,
			blend->alpha_destination, blend->color_write_mask, raster->cull_mode, raster->front_face, raster->fill_mode);
	/* his "point fill requires separate native contract": ours draws lines
	and points filled (Metal, OpenGL ES) */
	else if (raster->fill_mode != GPU_FILL_SOLID)
		REPORT(FILL_MODE_IGNORED, raster->fill_mode, "fill mode %u is drawn filled", raster->fill_mode);
	/* his "Enabled original write/test has no matching attachment" */
	if (!draw->depth_target && (depth->depth_test || depth->depth_write || depth->stencil_test))
		REPORT(DEPTH_STATE_WITHOUT_TARGET, 0, "depth test %u, depth write %u, stencil test %u without a depth target",
			depth->depth_test, depth->depth_write, depth->stencil_test);
	if (raster->depth_bias_enable && (float_class(float_bits(&raster->depth_bias_slope)) == FLOAT_NONFINITE ||
		float_class(float_bits(&raster->depth_bias_constant)) == FLOAT_NONFINITE))
		REPORT(DEPTH_BIAS_NONFINITE, 0, "depth bias slope %#010x, constant %#010x", float_bits(&raster->depth_bias_slope),
			float_bits(&raster->depth_bias_constant));
	/* his "viewport/depth range/scissor exceeds the target contract" */
	if (viewport->width <= 0 || viewport->height <= 0)
		REPORT(VIEWPORT_EMPTY, 0, "viewport %d,%d %dx%d", viewport->x, viewport->y, viewport->width, viewport->height);
	else if (width && rect_outside(viewport->x, viewport->y, viewport->width, viewport->height, width, height))
		REPORT(VIEWPORT_OUTSIDE_TARGET, (uint64_t)width << 32 | height, "viewport %d,%d %dx%d in a %ux%u target", viewport->x,
			viewport->y, viewport->width, viewport->height, width, height);
	if (float_class(float_bits(&viewport->min_z)) == FLOAT_NONFINITE || float_class(float_bits(&viewport->max_z)) == FLOAT_NONFINITE ||
		!(viewport->min_z >= 0.0f && viewport->min_z <= viewport->max_z && viewport->max_z <= 1.0f))
		REPORT(VIEWPORT_DEPTH_RANGE, 0, "depth range %g to %g", viewport->min_z, viewport->max_z);
	if (scissor->width < 0 || scissor->height < 0)
		REPORT(SCISSOR_INVALID, 0, "scissor %d,%d %dx%d", scissor->x, scissor->y, scissor->width, scissor->height);
	else if (scissor->width && scissor->height && width)
	{
		int outside = rect_outside(scissor->x, scissor->y, scissor->width, scissor->height, width, height);

		if (outside == 2)
			REPORT(SCISSOR_EMPTY, (uint64_t)width << 32 | height, "scissor %d,%d %dx%d is wholly outside a %ux%u target; the draw "
				"draws nothing", scissor->x, scissor->y, scissor->width, scissor->height, width, height);
		else if (outside)
			REPORT(SCISSOR_OUTSIDE_TARGET, (uint64_t)width << 32 | height, "scissor %d,%d %dx%d in a %ux%u target", scissor->x,
				scissor->y, scissor->width, scissor->height, width, height);
	}
}

#define UNIFORM_CHECK_GPU_UNIFORM_VERTEX(name, count) \
	if (floats_worst(uniforms->name, count * 4, &which) == FLOAT_NONFINITE) \
	{ \
		validation.uniforms_nonfinite = 1; \
		REPORT(UNIFORM_NONFINITE, field, #name "[%lu].%c is %#010x", which / 4, "xyzw"[which % 4], \
			float_bits(&uniforms->name[which / 4][which % 4])); \
	} \
	field++;
#define UNIFORM_CHECK_GPU_UNIFORM_PIXEL(name, count) UNIFORM_CHECK_GPU_UNIFORM_VERTEX(name, count)
#define UNIFORM_CHECK_GPU_UNIFORM_PIXEL_LOD_BIAS(name, count) UNIFORM_CHECK_GPU_UNIFORM_VERTEX(name, count)
#define UNIFORM_CHECK_GPU_UNIFORM_VERTEX_CONSTANTS(name, count)
#define UNIFORM_CHECK(name, glsl_type, count, stage) UNIFORM_CHECK_##stage(name, count)

static void check_numbers(const struct shader_entry *vertex, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	/* his host checks the whole pixel block and the vertex block's own
	uniforms, and leaves the 192 constants alone: registers no program
	reads hold whatever was last left there. Ours looks at the registers
	this draw's vertex shader reads by number */
	if (uniforms && (!validation.uniforms_seen || validation.uniforms_serial != uniforms->serial))
	{
		unsigned long which = 0, field = 0;

		validation.uniforms_seen = 1;
		validation.uniforms_serial = uniforms->serial;
		validation.uniforms_nonfinite = 0;
		GPU_UNIFORMS(UNIFORM_CHECK)
		(void)field;
	}
	else if (validation.uniforms_nonfinite)
	{
		REPORT(UNIFORM_NONFINITE, 0x1000, "the uniforms still hold the nonfinite value an earlier draw was told of");
	}
	if (constants && (!validation.constants_seen || validation.constants_serial != constants->serial))
	{
		uint64_t changed = constants->serial - validation.constants_serial, step;
		unsigned long which = 0;

		if (!validation.constants_seen || changed >= GPU_CONSTANT_LOG_SIZE)
		{
			unsigned reg;

			memset(validation.constants_nonfinite, 0, sizeof(validation.constants_nonfinite));
			for (reg = 0; reg < GPU_CONSTANT_COUNT; reg++)
				if (floats_worst(constants->c[reg], 4, &which) == FLOAT_NONFINITE)
					validation.constants_nonfinite[reg / 64] |= 1ull << (reg % 64);
		}
		else
		{
			for (step = 0; step < changed; step++)
			{
				unsigned reg = constants->log[(constants->serial - step) % GPU_CONSTANT_LOG_SIZE];

				if (reg >= GPU_CONSTANT_COUNT)
					continue;
				if (floats_worst(constants->c[reg], 4, &which) == FLOAT_NONFINITE)
					validation.constants_nonfinite[reg / 64] |= 1ull << (reg % 64);
				else
					validation.constants_nonfinite[reg / 64] &= ~(1ull << (reg % 64));
			}
		}
		validation.constants_seen = 1;
		validation.constants_serial = constants->serial;
	}
	if (vertex && constants)
	{
		unsigned word;

		for (word = 0; word < 3; word++)
		{
			uint64_t bad = vertex->constants[word] & validation.constants_nonfinite[word];

			if (bad)
			{
				unsigned reg = word * 64 + (unsigned)__builtin_ctzll(bad);

				REPORT(CONSTANT_NONFINITE, reg, "the vertex shader reads c[%u], which is %#010x %#010x %#010x %#010x", reg,
					float_bits(&constants->c[reg][0]), float_bits(&constants->c[reg][1]), float_bits(&constants->c[reg][2]),
					float_bits(&constants->c[reg][3]));
			}
		}
	}
}

static uint32_t validate_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	const struct shader_entry *vertex, *pixel;
	uint32_t width, height, drawn;
	unsigned long reported, before = validation.failures;

	validation.call = "draw";
	validation.draw++;
	if (!draw || !constants || !uniforms)
	{
		validation.current = NULL;
		REPORT(STATE_INVALID, 1, "a draw without its packet, constants or uniforms");
		return 0;
	}
	validation.current = draw;
	vertex = shader_entry(draw->vertex_shader);
	pixel = shader_entry(draw->pixel_shader);
	check_targets(draw->color_target, draw->depth_target, &width, &height);
	if (!draw->vertex_shader || !draw->pixel_shader)
		REPORT(SHADER_MISSING, (draw->vertex_shader != 0) | (draw->pixel_shader != 0) << 1, "no %s shader; the draw is skipped",
			!draw->vertex_shader ? "vertex" : "pixel");
	else if (!vertex || !pixel)
		REPORT(SHADER_UNKNOWN, 0, "shader %u is not one gpu_shader_create returned", !vertex ? draw->vertex_shader : draw->pixel_shader);
	else if (vertex->stage != GPU_SHADER_VERTEX || pixel->stage != GPU_SHADER_PIXEL)
		REPORT(SHADER_STAGE_MISMATCH, 0, "shaders %u and %u are not a vertex and a pixel shader", draw->vertex_shader,
			draw->pixel_shader);
	if (vertex && vertex->stage != GPU_SHADER_VERTEX)
		vertex = NULL;
	if (pixel && pixel->stage != GPU_SHADER_PIXEL)
		pixel = NULL;
	/* a depth target larger than the color target is tested pixel for
	pixel: the draw compares against the depth target's top left corner */
	if (draw->color_target && draw->depth_target && (draw->depth_stencil.depth_test || draw->depth_stencil.stencil_test))
	{
		const struct texture_entry *color = texture_entry(draw->color_target), *depth = texture_entry(draw->depth_target);

		if (color && depth && (depth->description.width > color->description.width || depth->description.height > color->description.height))
			REPORT(DEPTH_TARGET_LARGER, 1ull << 63 | (uint64_t)color->description.width << 32 | depth->description.width,
				"the draw tests depth (test %u, write %u, stencil %u) in a %ux%u color target against the top left of a %ux%u depth "
				"target", draw->depth_stencil.depth_test, draw->depth_stencil.depth_write, draw->depth_stencil.stencil_test,
				color->description.width, color->description.height, depth->description.width, depth->description.height);
	}
	check_state(draw, width, height);
	check_stages(draw, pixel);
	check_numbers(vertex, constants, uniforms);
	check_geometry(draw, vertex);
	if (rejected(before))
	{
		validation.current = NULL;
		validation.identity_valid = 0;
		return 0;
	}
	reported = validation.reported;
	validation.call = "draw (in the backend)";
	drawn = validation.backend.draw(draw, constants, uniforms);
	validation.call = "draw";
	/* a draw the backend dropped says why (GPU_VALIDATION_REPORT), or the
	validator has (no shader) */
	if (!drawn && reported == validation.reported && draw->vertex_shader && draw->pixel_shader)
		REPORT(DRAW_SKIPPED_UNEXPLAINED, 0, "the backend skipped the draw without a reason");
	validation.current = NULL;
	validation.identity_valid = 0;
	return drawn;
}

/* ---------- visibility tests (his queries: no nested begin, an end matches) */

static void validate_visibility_begin(void)
{
	validation.call = "visibility_begin";
	if (validation.visibility_active)
		REPORT(VISIBILITY_INVALID, 1, "a test begins inside another");
	validation.visibility_active = 1;
	validation.backend.visibility_begin();
}

static void validate_visibility_end(uint32_t slot)
{
	validation.call = "visibility_end";
	if (!validation.visibility_active)
		REPORT(VISIBILITY_INVALID, 2, "a test ends that never began");
	validation.visibility_active = 0;
	/* the backends index their slots with it unchecked */
	if (!slot || slot >= GPU_VISIBILITY_SLOTS)
	{
		REPORT(VISIBILITY_INVALID, 3, "slot %u is outside 1 to %u; the call is not passed on", slot, GPU_VISIBILITY_SLOTS - 1);
		return;
	}
	validation.backend.visibility_end(slot);
}

static uint32_t validate_visibility_result(uint32_t slot, uint32_t *samples)
{
	validation.call = "visibility_result";
	if (!slot || slot >= GPU_VISIBILITY_SLOTS || !samples)
	{
		REPORT(VISIBILITY_INVALID, 4, "slot %u is outside 1 to %u; the call is not passed on", slot, GPU_VISIBILITY_SLOTS - 1);
		if (samples)
			*samples = 0;
		return 1;
	}
	return validation.backend.visibility_result(slot, samples);
}

/* ---------- frames */

static void validate_flush(void)
{
	validation.call = "flush";
	validation.backend.flush();
}

/* (from the front end's vertical blank thread, not the game's: nothing of
the validator's is touched) */
static uint32_t validate_vertical_blank_wait(void)
{
	return validation.backend.vertical_blank_wait();
}

/* (before the frame reads its input: no GPU state is involved) */
static void validate_frame_start(void)
{
	validation.backend.frame_start();
}

static void frame_end(void)
{
	validation.frame++;
	validation.pass = 0;
	validation.draw = 0;
	validation.pass_color = validation.pass_depth = 0;
	validation.identity_valid = 0;
	/* a line whenever there is something new, at most every ten seconds or so */
	if (validation.frame % 600 == 0 && validation.cause_count != validation.summarized)
	{
		validation.summarized = validation.cause_count;
		gpu_validation_summary();
	}
}

static void check_picture(gpu_texture texture, const char *name, int color)
{
	const struct texture_entry *entry = texture_entry(texture);

	if (!entry || entry->description.usage != GPU_USAGE_RENDER_TARGET || entry->description.type != GPU_TEXTURE_2D ||
		(entry->description.format == GPU_FORMAT_BGRA8) != (color != 0))
		REPORT(PRESENT_INVALID, entry != NULL, "%s %u is not a live %s target", name, texture, color ? "color" : "depth");
}

/* display.native_hud's stretch and display.anti_aliasing's passes: the
pictures they read and write must be live color targets */
static void validate_upscale(gpu_texture source, gpu_texture destination, const struct gpu_rect *area, uint32_t anti_alias)
{
	validation.call = "upscale";
	check_picture(source, "the upscale's source", 1);
	check_picture(destination, "the upscale's destination", 1);
	validation.backend.upscale(source, destination, area, anti_alias);
}

static uint32_t validate_anti_alias_prepare(uint32_t method, const char *smaa_shader, const void *area_texels,
	const void *search_texels)
{
	validation.call = "anti_alias_prepare";
	return validation.backend.anti_alias_prepare(method, smaa_shader, area_texels, search_texels);
}

static void validate_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect)
{
	validation.call = "anti_alias";
	check_picture(target, "the anti-aliasing target", 1);
	validation.backend.anti_alias(method, target, rect);
}

static uint32_t validate_present(gpu_texture back_buffer)
{
	uint32_t due;
	unsigned long before = validation.failures;

	validation.call = "present";
	validation.current = NULL;
	/* his present: no active query, the source an initialized target */
	if (validation.visibility_active)
		REPORT(VISIBILITY_INVALID, 5, "a visibility test is still open at the frame's end");
	check_picture(back_buffer, "the back buffer", 1);
	due = rejected(before) ? 0 : validation.backend.present(back_buffer);
	frame_end();
	return due;
}

static uint32_t validate_present_stereo(const struct gpu_stereo_present *present)
{
	uint32_t due;
	int eye, group;
	unsigned long before = validation.failures;

	validation.call = "present_stereo";
	validation.current = NULL;
	if (validation.visibility_active)
		REPORT(VISIBILITY_INVALID, 5, "a visibility test is still open at the frame's end");
	if (present)
	{
		if (!present->zoom)
			for (eye = 0; eye < 2; eye++)
			{
				check_picture(present->eye_color[eye], eye ? "the right eye" : "the left eye", 1);
				if (present->eye_depth[eye])
					check_picture(present->eye_depth[eye], eye ? "the right eye's depth" : "the left eye's depth", 0);
			}
		else
			check_picture(present->zoom, "the zoomed picture", 1);
		if (present->hud)
			check_picture(present->hud, "the HUD layer", 1);
		if (present->ui)
			check_picture(present->ui, "the UI layer", 1);
		if (present->reticle_layer)
			check_picture(present->reticle_layer, "the reticle layer", 1);
		for (group = 0; group < HALO_HUD_GROUP_COUNT; group++)
			if (present->hud_group[group])
				check_picture(present->hud_group[group], "a HUD group's layer", 1);
		if (float_class(float_bits(&present->near_meters)) == FLOAT_NONFINITE ||
			float_class(float_bits(&present->far_meters)) == FLOAT_NONFINITE)
			REPORT(PRESENT_INVALID, 2, "the depth range is %#010x to %#010x", float_bits(&present->near_meters),
				float_bits(&present->far_meters));
	}
	else
	{
		REPORT(PRESENT_INVALID, 3, "no stereo frame");
	}
	due = rejected(before) ? 0 : validation.backend.present_stereo(present);
	frame_end();
	return due;
}

static uint32_t validate_call_count_take(void)
{
	return validation.backend.call_count_take();
}

static uint32_t validate_warm_list_read(const char *name, char *text, uint32_t size)
{
	return validation.backend.warm_list_read(name, text, size);
}

static void validate_warm_begin(void)
{
	validation.call = "warm_begin";
	validation.backend.warm_begin();
}

static void validate_pipeline_warm(const struct gpu_pipeline_description *description)
{
	validation.call = "pipeline_warm";
	if (!description || !shader_entry(description->vertex_shader) || !shader_entry(description->pixel_shader))
		REPORT(SHADER_UNKNOWN, 1, "a listed pipeline names shaders %u and %u", description ? description->vertex_shader : 0,
			description ? description->pixel_shader : 0);
	validation.backend.pipeline_warm(description);
}

static void validate_warm_end(void)
{
	validation.call = "warm_end";
	validation.backend.warm_end();
}

static uint32_t validate_pipeline_built_take(struct gpu_pipeline_description *description)
{
	return validation.backend.pipeline_built_take(description);
}

/* (what is being drawn, for the backend's frame log: nothing to check) */
static void validate_frame_context(uint32_t scene_class, uint32_t frame_rate, const char *level)
{
	validation.backend.frame_context(scene_class, frame_rate, level);
}

/* (the loading screen asks this from the game thread: nothing to check) */
static uint32_t validate_companion_size(uint32_t *width, uint32_t *height)
{
	return validation.backend.companion_size(width, height);
}

static void validate_companion_set(gpu_texture target)
{
	validation.backend.companion_set(target);
}

static void validate_compile_progress(uint32_t action, struct gpu_compile_progress *progress)
{
	validation.backend.compile_progress(action, progress);
}

static uint32_t validate_compiles_outstanding(void)
{
	return validation.backend.compiles_outstanding();
}

static void validate_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	validation.call = "initialize";
	validation.backend.initialize(flags, capabilities);
	if (capabilities)
		validation.capabilities = *capabilities;
}

/* ---------- the front end's side */

void gpu_validation_identity(const struct gpu_validation_identity *identity)
{
	if (!validation.installed || !identity)
		return;
	validation.identity = *identity;
	validation.identity_valid = 1;
}

void gpu_validation_note(uint32_t reason, uint32_t detail, uint32_t value)
{
	const char *saved = validation.call;

	if (!validation.installed)
		return;
	validation.call = "front end, before call";
	gpu_validation_report(reason, (uint64_t)detail << 32 | value, "detail %#x, value %#x (d3d8_device.c names them)", detail, value);
	validation.call = saved;
	validation.identity_valid = 0;
}

/* ---------- installing */

void gpu_validation_reset(void)
{
	uint32_t index;

	for (index = 0; index < validation.buffers.count; index++)
		free(((struct buffer_entry *)validation.buffers.entries)[index].pages);
	free(validation.textures.entries);
	free(validation.buffers.entries);
	free(validation.shaders.entries);
	memset(&validation, 0, sizeof(validation));
}

void gpu_validation_install(struct gpu_backend *table, const struct gpu_validation_host *host)
{
	static int registered;

	if (!table || !host || host->mode == GPU_VALIDATION_OFF)
		return;
	gpu_validation_reset();
	validation.backend = *table;
	validation.host = *host;
	validation.textures.size = sizeof(struct texture_entry);
	validation.buffers.size = sizeof(struct buffer_entry);
	validation.shaders.size = sizeof(struct shader_entry);
	validation.installed = 1;
	if (host->allow)
	{
		uint32_t reason;

		for (reason = 0; reason < GPU_VALIDATION_REASON_COUNT; reason++)
		{
			const char *found = strstr(host->allow, reason_names[reason]);
			size_t length = strlen(reason_names[reason]);

			while (found && !((found == host->allow || found[-1] == ',' || found[-1] == ' ') &&
				(found[length] == 0 || found[length] == ',' || found[length] == ' ')))
				found = strstr(found + 1, reason_names[reason]);
			validation.allowed[reason] = found != NULL;
		}
	}
#define INSTALL_FUNCTION(type, name, parameters, arguments) table->name = validate_##name;
#define INSTALL_PROCEDURE(name, parameters, arguments) table->name = validate_##name;
	GPU_FUNCTIONS(INSTALL_FUNCTION, INSTALL_PROCEDURE)
#undef INSTALL_FUNCTION
#undef INSTALL_PROCEDURE
	if (!registered)
	{
		registered = 1;
		atexit(summary_at_exit);
	}
	{
		char line[256];

		snprintf(line, sizeof(line), "render validation: on, in %s mode (debug.render_validation)%s%s",
			host->mode == GPU_VALIDATION_STRICT ? "strict" : host->mode == GPU_VALIDATION_REJECT ? "reject" : "log", host->allow && *host->allow ? ", allowing " : "",
			host->allow ? host->allow : "");
		emit(line);
	}
}

#endif
