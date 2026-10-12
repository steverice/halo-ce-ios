/* texture_replacement_cache.h: the replacement textures' byte budget. What makes room for a load, a dropped
replacement's reload, the purge after a map load, the two sets and the memory warning. No files and no GPU: a
"texture" here is a number, and release() counts. */
#include "texture_replacement_cache.h"
#include <assert.h>
#include <stdio.h>

#define MB ((uint64_t)1 << 20)

static unsigned released, live, next_texture = 1;
static uint32_t last_released;

static void release(uint32_t texture)
{
	assert(texture);
	released++;
	live--;
	last_released = texture;
}

/* what xbox_textures.c does at a draw: the replacement's texture, or 0 for the original */
static uint32_t draw(struct texture_replacement_cache *cache, int set, uint64_t hash, uint64_t bytes)
{
	long index = texture_replacement_record(cache, set, hash, 64, 64);
	struct texture_replacement *record;

	assert(index >= 0);
	record = &cache->records[index];
	if (record->state != _texture_replacement_resident && texture_replacement_wants_load(cache, record))
	{
		if (!bytes)
			texture_replacement_loaded(cache, record, 0, 0);   /* no file */
		else if (texture_replacement_admit(cache, record, bytes))
		{
			live++;
			texture_replacement_loaded(cache, record, next_texture++, 1);
		}
	}
	if (record->state != _texture_replacement_resident)
		return 0;
	texture_replacement_touch(cache, record);
	return record->texture;
}

static void frames(struct texture_replacement_cache *cache, unsigned count)
{
	unsigned long purged;

	while (count--)
		texture_replacement_begin_frame(cache, &purged);
}

static void check_totals(const struct texture_replacement_cache *cache)
{
	uint64_t bytes = 0;
	unsigned long index, resident = 0;

	for (index = 0; index < cache->count; index++)
		if (cache->records[index].state == _texture_replacement_resident)
		{
			bytes += cache->records[index].bytes;
			resident++;
		}
	assert(bytes == cache->bytes && resident == cache->resident && resident == live);
	assert(cache->bytes <= cache->budget);
}

int main(void)
{
	struct texture_replacement_cache cache;
	uint32_t a, b, c, d;
	unsigned long purged;
	unsigned before;

	/* the default budget: a quarter of what the system leaves the app, else an eighth of the machine, 64 MiB to 2 GiB */
	assert(texture_replacement_default_budget(3400 * MB, 8192 * MB) == 850 * MB);
	assert(texture_replacement_default_budget(7600 * MB, 16384 * MB) == 1900 * MB);
	assert(texture_replacement_default_budget(12000 * MB, 16384 * MB) == 2048 * MB);
	assert(texture_replacement_default_budget(100 * MB, 2048 * MB) == 64 * MB);
	assert(texture_replacement_default_budget(0, 8192 * MB) == 1024 * MB);
	assert(texture_replacement_default_budget(0, 65536 * MB) == 2048 * MB);
	assert(texture_replacement_default_budget(0, 0) == 512 * MB);

	/* ---------- the budget and the order of eviction */
	texture_replacement_cache_initialize(&cache, 100 * MB, release);
	a = draw(&cache, 0, 1, 40 * MB);
	frames(&cache, 1);
	b = draw(&cache, 0, 2, 40 * MB);
	assert(a && b && a != b && cache.bytes == 80 * MB && cache.statistics.loads == 2);
	/* no room, and both were drawn too lately to go: the newcomer is refused, and nothing is dropped for it */
	frames(&cache, 1);
	assert(!draw(&cache, 0, 3, 40 * MB) && released == 0 && cache.statistics.refusals == 1);
	/* a refused one is not asked for again at once (no file read every frame) */
	frames(&cache, 1);
	assert(!draw(&cache, 0, 3, 40 * MB) && cache.statistics.refusals == 1);
	/* one bigger than the whole budget never loads and drops nothing */
	assert(!draw(&cache, 0, 4, 101 * MB) && released == 0);
	/* b stays in use; once a is past the grace period it is the one that makes room */
	for (unsigned frame = 0; frame < TEXTURE_REPLACEMENT_RETRY_FRAMES; frame++)
	{
		frames(&cache, 1);
		assert(draw(&cache, 0, 2, 40 * MB) == b);
	}
	c = draw(&cache, 0, 3, 40 * MB);
	assert(c && released == 1 && last_released == a && cache.statistics.evictions == 1 && cache.bytes == 80 * MB);
	check_totals(&cache);
	/* least recently used first: of two old ones, the one drawn longer ago goes */
	frames(&cache, 1);
	d = draw(&cache, 0, 5, 20 * MB);                       /* fits: 100 of 100 */
	assert(d && released == 1);
	frames(&cache, 10);
	assert(draw(&cache, 0, 3, 40 * MB) == c);              /* c is now newer than b */
	frames(&cache, TEXTURE_REPLACEMENT_GRACE_FRAMES + 1);
	assert(draw(&cache, 0, 6, 30 * MB) && last_released == b && released == 2);
	check_totals(&cache);

	/* ---------- a dropped replacement is read again when drawn, and counted as read again */
	frames(&cache, TEXTURE_REPLACEMENT_GRACE_FRAMES + 1);
	before = cache.statistics.reloads;
	a = draw(&cache, 0, 1, 40 * MB);
	assert(a && cache.statistics.reloads == before + 1);
	check_totals(&cache);
	/* a texture with no file is remembered: no second look until a map load */
	assert(!draw(&cache, 0, 7, 0) && !draw(&cache, 0, 7, 40 * MB));

	/* ---------- a map load: what the new level does not draw goes after TEXTURE_REPLACEMENT_PURGE_FRAMES */
	texture_replacement_cache_initialize(&cache, 100 * MB, release);
	released = live = 0;
	a = draw(&cache, 0, 1, 30 * MB);
	b = draw(&cache, 0, 2, 30 * MB);
	assert(!draw(&cache, 0, 7, 0));
	texture_replacement_map_loaded(&cache, 1);
	assert(cache.resident == 2 && released == 0 && cache.statistics.loads == 0);
	assert(cache.statistics.peak_bytes == 60 * MB);
	/* the new level draws b only; a file that was missing is looked for again */
	for (unsigned frame = 0; frame < TEXTURE_REPLACEMENT_PURGE_FRAMES - 1; frame++)
	{
		assert(!texture_replacement_begin_frame(&cache, &purged) && !purged);
		assert(draw(&cache, 0, 2, 30 * MB) == b);
	}
	assert(draw(&cache, 0, 7, 10 * MB));
	assert(texture_replacement_begin_frame(&cache, &purged) == 30 * MB && purged == 1 && last_released == a);
	assert(cache.resident == 2 && cache.bytes == 40 * MB);
	check_totals(&cache);
	/* once only */
	frames(&cache, TEXTURE_REPLACEMENT_PURGE_FRAMES * 2);
	assert(cache.resident == 2);
	/* the level that plays with upscaled textures off keeps none */
	texture_replacement_map_loaded(&cache, 0);
	assert(!cache.resident && !cache.bytes && !cache.count && !live);

	/* ---------- two sets share the budget */
	texture_replacement_cache_initialize(&cache, 100 * MB, release);
	released = live = 0;
	a = draw(&cache, 0, 1, 30 * MB);
	b = draw(&cache, 0, 2, 30 * MB);
	/* room for both sets of hash 1, not for both of hash 2: the set left behind makes room at once, though it was
	drawn a frame ago, and the total never passes the budget */
	frames(&cache, 1);
	texture_replacement_select(&cache, 1);
	c = draw(&cache, 1, 1, 30 * MB);
	assert(c && c != a && released == 0 && cache.bytes == 90 * MB);
	d = draw(&cache, 1, 2, 60 * MB);
	assert(d && released == 2 && cache.bytes == 90 * MB && cache.resident == 2);
	check_totals(&cache);
	/* the selected set's own textures are not dropped for the other set's */
	frames(&cache, 1);
	assert(draw(&cache, 1, 1, 30 * MB) == c && draw(&cache, 1, 2, 60 * MB) == d);
	/* with room for both, a switch back and forth reads nothing */
	texture_replacement_cache_initialize(&cache, 200 * MB, release);
	released = live = 0;
	a = draw(&cache, 0, 1, 30 * MB);
	texture_replacement_select(&cache, 1);
	c = draw(&cache, 1, 1, 60 * MB);
	before = cache.statistics.loads + cache.statistics.reloads;
	for (unsigned toggle = 0; toggle < 20; toggle++)
	{
		unsigned long serial = cache.serial;

		frames(&cache, 3);
		texture_replacement_select(&cache, toggle % 2 ? 1 : 0);
		assert(cache.serial != serial);
		assert(draw(&cache, toggle % 2 ? 1 : 0, 1, toggle % 2 ? 60 * MB : 30 * MB) == (toggle % 2 ? c : a));
	}
	assert(cache.statistics.loads + cache.statistics.reloads == before && released == 0 && cache.resident == 2);
	/* a map load starts on the first set again */
	texture_replacement_map_loaded(&cache, 1);
	assert(cache.selected == 0);

	/* ---------- the memory warning: what the last two frames did not draw goes, and the budget halves */
	texture_replacement_cache_initialize(&cache, 100 * MB, release);
	released = live = 0;
	a = draw(&cache, 0, 1, 30 * MB);
	b = draw(&cache, 0, 2, 30 * MB);
	frames(&cache, 3);
	assert(draw(&cache, 0, 2, 30 * MB) == b);
	frames(&cache, 1);
	assert(texture_replacement_memory_warning(&cache) == 30 * MB && last_released == a && cache.budget == 50 * MB);
	assert(cache.resident == 1 && cache.warnings == 1);
	/* under the halved budget a's reload finds no room while b is in use */
	assert(!draw(&cache, 0, 1, 30 * MB) && draw(&cache, 0, 2, 30 * MB) == b);
	check_totals(&cache);
	/* the budget stops at the floor */
	for (unsigned warning = 0; warning < 8; warning++)
		texture_replacement_memory_warning(&cache);
	assert(cache.budget == TEXTURE_REPLACEMENT_MINIMUM_BUDGET);
	free(cache.records);
	puts("PASS: texture replacement budget, eviction order, reload, level purge, two sets and memory warning");
	return 0;
}
