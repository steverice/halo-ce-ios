/* Apache-2.0. Disk-only material classification; never follows guest pointers. */
#ifndef ASTC_MATERIAL_H
#define ASTC_MATERIAL_H
#include "astc_encode.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static uint32_t astc_material_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool astc_material_class(const unsigned char *p, const char *forward, const char *reverse)
{
	return !memcmp(p, forward, 4) || !memcmp(p, reverse, 4);
}
static bool astc_material_read(FILE *map, uint64_t size, uint64_t at, void *bytes, size_t count)
{
	return at <= size && count <= size - at && at <= INT64_MAX &&
		!fseeko(map, (off_t)at, SEEK_SET) && fread(bytes, 1, count, map) == count;
}
static int astc_material_offset_compare(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
	return (a > b) - (a < b);
}
struct astc_material_span
{
	uint64_t start, end;
	unsigned type;
};
static int astc_material_span_compare(const void *left, const void *right)
{
	const struct astc_material_span *a = left, *b = right;
	return (a->start > b->start) - (a->start < b->start);
}
/* Return a caller-owned array indexed by the bitmap datum's low 16 bits.
 * NULL, an unreferenced bitmap, or any unknown use retains the existing gate.
 * The caller keeps shared resource records conservative and merges policies of
 * distinct bitmap tags whose texture bytes have the same store identity. */
static struct astc_alpha_policy *astc_material_collect(FILE *map, uint64_t map_size,
	uint64_t instances, uint32_t tag_count, int64_t translation)
{
	if (!map || !tag_count || tag_count > 65536 || instances > map_size ||
		(uint64_t)tag_count * 32 > map_size - instances) return NULL;
	struct astc_alpha_policy *policies = calloc(tag_count, sizeof(*policies));
	uint32_t *datums = calloc(tag_count, sizeof(*datums));
	unsigned char *state = calloc(tag_count, 1);
	uint64_t *recognized = malloc((size_t)tag_count * sizeof(*recognized));
	struct astc_material_span *spans = malloc((size_t)tag_count * sizeof(*spans));
	if (!policies || !datums || !state || !recognized || !spans) goto failed;
	size_t recognized_count = 0, span_count = 0;
	for (uint32_t i = 0; i < tag_count; i++)
	{
		unsigned char tag[32];
		if (!astc_material_read(map, map_size, instances + (uint64_t)i * 32, tag, sizeof(tag))) goto failed;
		if (astc_material_class(tag, "bitm", "mtib"))
		{
			datums[i] = astc_material_u32(tag + 12);
			state[i] = 1;
		}
		bool model = astc_material_class(tag, "soso", "osos");
		bool environment = astc_material_class(tag, "senv", "vnes");
		uint32_t pointer = astc_material_u32(tag + 20);
		if (!pointer || translation > INT64_MAX - pointer) continue;
		int64_t group = translation + pointer;
		if (group < 0 || (uint64_t)group > map_size) continue;
		unsigned extent = model ? 0x174u : environment ? 0x334u : 0;
		spans[span_count++] = (struct astc_material_span){(uint64_t)group, (uint64_t)group + extent,
			model ? 1u : environment ? 2u : 0u};
	}
	qsort(spans, span_count, sizeof(*spans), astc_material_span_compare);
	uint64_t previous_end = 0;
	for (size_t i = 0; i < span_count; i++)
	{
		const struct astc_material_span *span = spans + i;
		/* An aliased material may interpret the same bytes as a blended or
		 * unknown consumer. Keep all such candidates conservative. The sorted
		 * starts and prefix maximum detect overlaps without a quadratic walk. */
		bool aliased = previous_end > span->start ||
			(i && spans[i - 1].start == span->start) ||
			(i + 1 < span_count && spans[i + 1].start < span->end);
		if (span->end > previous_end) previous_end = span->end;
		if (span->type != 1 || aliased || span->end > map_size) continue;
		unsigned char metadata[6];
		if (!astc_material_read(map, map_size, span->start + 0x24, metadata, sizeof(metadata))) continue;
		/* The renderer dispatches by shader_base.type, not just the tag class. */
		if (metadata[0] != 4 || metadata[1]) continue;
		/* Model alpha-blended decals use base alpha for blending. All other
		 * shader classes and nonshader consumers remain unknown below. */
		if (metadata[4] & 8) continue;
		/* Only model base alpha is a direct >127 test input without a blended
		 * use. Model multipurpose alpha feeds RGB weights; environment alpha
		 * feeds later DESTALPHA blend masks. Preserve their existing gate. See
		 * rasterizer_xbox_models.c and rasterizer_xbox_active_camouflage.c. */
		recognized[recognized_count++] = span->start + 0xA4;
	}
	qsort(recognized, recognized_count, sizeof(*recognized), astc_material_offset_compare);
	/* Scan the entire file, including separately stored BSP metadata, so an
	 * unclassified consumer overrides recognized uses. Pixel coincidences only
	 * make the policy more conservative. Overlap covers references split by I/O. */
	for (uint64_t start = 0; start < map_size;)
	{
		unsigned char chunk[4096 + 12];
		size_t bytes = map_size - start < sizeof(chunk) ? (size_t)(map_size - start) : sizeof(chunk);
		if (!astc_material_read(map, map_size, start, chunk, bytes)) goto failed;
		for (unsigned local = 0; local < 4096 && local + 16 <= bytes; local += 4)
		{
			uint64_t at = start + local;
			/* A bitmap tag instance has the same FourCC/datum arrangement as
			 * a reference. The table itself does not represent a material use. */
			if (at >= instances && at < instances + (uint64_t)tag_count * 32) continue;
			const unsigned char *reference = chunk + local;
			if (!astc_material_class(reference, "bitm", "mtib")) continue;
			uint32_t datum = astc_material_u32(reference + 12), index = datum & 0xFFFF;
			if (index >= tag_count || !state[index] || datums[index] != datum) continue;
			if (bsearch(&at, recognized, recognized_count, sizeof(*recognized), astc_material_offset_compare))
				state[index] |= 2;
			else state[index] |= 4;
		}
		if (map_size - start <= 4096) break;
		start += 4096;
	}
	for (uint32_t i = 0; i < tag_count; i++) if (state[i] == 3)
	{
		policies[i].known = true;
		policies[i].boundaries[16] = 1;
	}
	free(datums); free(state); free(recognized); free(spans);
	return policies;
failed:
	free(policies); free(datums); free(state); free(recognized); free(spans);
	return NULL;
}
#endif
