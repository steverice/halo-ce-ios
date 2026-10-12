/*
TEXTURE_REPLACEMENT_CACHE.H

Which replacement textures (texture_override.h) are in GPU memory, under a
byte budget. xbox_textures.c loads a replacement the first time its texture
is drawn; this decides what makes room for it and when it goes:

- a load that would pass the budget first drops the replacements drawn
  longest ago. One drawn in the last TEXTURE_REPLACEMENT_GRACE_FRAMES frames
  is never dropped for another: when only those are left, the new one is
  refused and its texture is drawn as the map has it, until a later try;
- a map load marks every replacement unused; TEXTURE_REPLACEMENT_PURGE_FRAMES
  frames into the new level, the ones it has not drawn are dropped (a level
  with upscaled textures off drops them all at once);
- a memory warning from the system drops every replacement the last two
  frames did not draw and halves the budget until the game quits.

A dropped replacement keeps its record, so the next draw of its texture reads
the file again. Records are found by the hash of the original's first level,
its size and the set the replacement is from: debug.texture_compare_directory
makes a second set, and the two share the budget. Only the selected set is
drawn, so at a switch the other's replacements become the first to make
room: a switch back to them is immediate while there is room for both, and
reads them again otherwise. The cache never touches the GPU itself: the caller creates the
texture, and release() destroys one. Header-only, for a host probe
(port/ios/tests/texture_replacement_probe.c).
*/

#ifndef TEXTURE_REPLACEMENT_CACHE_H
#define TEXTURE_REPLACEMENT_CACHE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEXTURE_REPLACEMENT_GRACE_FRAMES 60
#define TEXTURE_REPLACEMENT_PURGE_FRAMES 600
#define TEXTURE_REPLACEMENT_RETRY_FRAMES 300
#define TEXTURE_REPLACEMENT_MINIMUM_BUDGET ((uint64_t)16 << 20)

enum
{
	/* nothing is known yet: the next draw looks for the file */
	_texture_replacement_unknown,
	/* no file, or one that can't stand for the texture */
	_texture_replacement_absent,
	/* texture holds it */
	_texture_replacement_resident,
	/* it was resident and was dropped, or found no room: bytes is its size */
	_texture_replacement_dropped,
};

struct texture_replacement
{
	uint64_t hash;
	uint32_t width, height;   /* the original's */
	uint32_t texture, levels; /* resident: gpu.h's handle and its level count */
	uint64_t bytes;           /* its levels' bytes, once the file has been checked */
	uint32_t last_used_frame;
	uint32_t retry_frame;     /* dropped: no load is tried before this frame */
	uint8_t state;
	uint8_t set;              /* 0, or 1 for debug.texture_compare_directory's */
	uint8_t used_this_level;
	uint8_t read_before;      /* it has been resident since its record was made */
};

struct texture_replacement_statistics
{
	/* since the last map load */
	unsigned long loads, reloads, evictions, refusals;
	uint64_t peak_bytes;
};

struct texture_replacement_cache
{
	struct texture_replacement *records;
	unsigned long count, capacity;
	/* changes whenever records move: an index is good for one serial */
	unsigned long serial;
	uint64_t budget, bytes;
	unsigned long resident;
	uint32_t frame;
	/* the frame the level's unused replacements go at; 0: none pending */
	uint32_t purge_frame;
	/* the set being drawn */
	uint8_t selected;
	unsigned long warnings;
	struct texture_replacement_statistics statistics;
	void (*release)(uint32_t texture);
};

static inline void texture_replacement_cache_initialize(struct texture_replacement_cache *cache, uint64_t budget,
	void (*release)(uint32_t texture))
{
	memset(cache, 0, sizeof(*cache));
	cache->serial = 1;
	/* (past the grace period, so that frame numbers never go below 0) */
	cache->frame = TEXTURE_REPLACEMENT_GRACE_FRAMES + 2;
	cache->budget = budget;
	cache->release = release;
}

/* the budget for a device with this much memory left to the process before
the system ends it (0: not known) and this much memory in all (0: not
known): a quarter of what is left, else an eighth of the machine's, between
64 MiB and 2 GiB */
static inline uint64_t texture_replacement_default_budget(uint64_t available, uint64_t physical)
{
	uint64_t budget = available ? available / 4 : physical ? physical / 8 : (uint64_t)512 << 20;

	if (budget < ((uint64_t)64 << 20))
		budget = (uint64_t)64 << 20;
	if (budget > ((uint64_t)2048 << 20))
		budget = (uint64_t)2048 << 20;
	return budget;
}

static inline void texture_replacement_drop(struct texture_replacement_cache *cache, struct texture_replacement *record)
{
	if (record->state != _texture_replacement_resident)
		return;
	cache->release(record->texture);
	cache->bytes -= record->bytes;
	cache->resident--;
	record->texture = 0;
	record->levels = 0;
	record->state = _texture_replacement_dropped;
	record->retry_frame = 0;
}

/* the record of this set for a texture with this hash and size, made if
there is none; -1 without memory for one. The index holds while
cache->serial does */
static inline long texture_replacement_record(struct texture_replacement_cache *cache, int set, uint64_t hash,
	uint32_t width, uint32_t height)
{
	struct texture_replacement *record;
	unsigned long index;

	for (index = 0; index < cache->count; index++)
	{
		record = &cache->records[index];
		if (record->hash == hash && record->width == width && record->height == height && record->set == set)
			return (long)index;
	}
	if (cache->count == cache->capacity)
	{
		unsigned long capacity = cache->capacity ? cache->capacity * 2 : 64;
		struct texture_replacement *records = realloc(cache->records, capacity * sizeof(*records));

		if (!records)
			return -1;
		cache->records = records;
		cache->capacity = capacity;
	}
	record = &cache->records[cache->count];
	memset(record, 0, sizeof(*record));
	record->hash = hash;
	record->width = width;
	record->height = height;
	record->set = (uint8_t)set;
	return (long)cache->count++;
}

/* a resident replacement was drawn */
static inline void texture_replacement_touch(struct texture_replacement_cache *cache, struct texture_replacement *record)
{
	record->last_used_frame = cache->frame;
	record->used_this_level = 1;
}

static inline int texture_replacement_old(const struct texture_replacement_cache *cache,
	const struct texture_replacement *record, uint32_t frames)
{
	return cache->frame - record->last_used_frame > frames;
}

/* whether a resident replacement may make room for another */
static inline int texture_replacement_droppable(const struct texture_replacement_cache *cache,
	const struct texture_replacement *record)
{
	return record->state == _texture_replacement_resident &&
		texture_replacement_old(cache, record, TEXTURE_REPLACEMENT_GRACE_FRAMES);
}

/* the other set is drawn from now on. Every index is void (the serial
changes). The replacements in memory all count as drawn long ago, so the set
left behind makes room for the one arriving, and what found no room is tried
again at once */
static inline void texture_replacement_select(struct texture_replacement_cache *cache, int set)
{
	unsigned long index;

	if (cache->selected == (uint8_t)set)
		return;
	cache->selected = (uint8_t)set;
	cache->serial++;
	for (index = 0; index < cache->count; index++)
	{
		struct texture_replacement *record = &cache->records[index];

		record->retry_frame = 0;
		if (!texture_replacement_old(cache, record, TEXTURE_REPLACEMENT_GRACE_FRAMES))
			record->last_used_frame = cache->frame - TEXTURE_REPLACEMENT_GRACE_FRAMES - 1;
	}
}

/* makes room for bytes more: drops the replacements drawn longest ago, never
one of the selected set drawn in the last TEXTURE_REPLACEMENT_GRACE_FRAMES
frames. 0, with nothing dropped, when that would not be enough */
static inline int texture_replacement_reserve(struct texture_replacement_cache *cache, uint64_t bytes)
{
	uint64_t droppable = 0;
	unsigned long index;

	if (cache->bytes + bytes <= cache->budget)
		return 1;
	if (bytes > cache->budget)
		return 0;
	for (index = 0; index < cache->count; index++)
	{
		const struct texture_replacement *record = &cache->records[index];

		if (texture_replacement_droppable(cache, record))
			droppable += record->bytes;
	}
	if (cache->bytes - droppable + bytes > cache->budget)
		return 0;
	while (cache->bytes + bytes > cache->budget)
	{
		struct texture_replacement *oldest = NULL;

		for (index = 0; index < cache->count; index++)
		{
			struct texture_replacement *record = &cache->records[index];

			if (texture_replacement_droppable(cache, record) &&
				(!oldest || record->last_used_frame < oldest->last_used_frame))
				oldest = record;
		}
		texture_replacement_drop(cache, oldest);
		cache->statistics.evictions++;
	}
	return 1;
}

/* whether the caller should read the record's file now. A record whose size
is known (it was resident, or refused) is asked for only if there is room,
which this makes; one refused waits TEXTURE_REPLACEMENT_RETRY_FRAMES */
static inline int texture_replacement_wants_load(struct texture_replacement_cache *cache,
	struct texture_replacement *record)
{
	if (record->state == _texture_replacement_unknown)
		return 1;
	if (record->state != _texture_replacement_dropped || cache->frame < record->retry_frame)
		return 0;
	if (texture_replacement_reserve(cache, record->bytes))
		return 1;
	record->retry_frame = cache->frame + TEXTURE_REPLACEMENT_RETRY_FRAMES;
	cache->statistics.refusals++;
	return 0;
}

/* the file was checked and takes bytes in GPU memory: whether there is room
(made as above). 0: the caller draws the original; the record waits */
static inline int texture_replacement_admit(struct texture_replacement_cache *cache,
	struct texture_replacement *record, uint64_t bytes)
{
	record->bytes = bytes;
	if (texture_replacement_reserve(cache, bytes))
		return 1;
	cache->statistics.refusals++;
	record->state = _texture_replacement_dropped;
	record->retry_frame = cache->frame + TEXTURE_REPLACEMENT_RETRY_FRAMES;
	return 0;
}

/* the load's result, after texture_replacement_admit: the texture and its
level count, or 0 for a file that is missing or can't be used (absent) */
static inline void texture_replacement_loaded(struct texture_replacement_cache *cache,
	struct texture_replacement *record, uint32_t texture, uint32_t levels)
{
	if (!texture)
	{
		record->state = _texture_replacement_absent;
		record->bytes = 0;
		return;
	}
	record->texture = texture;
	record->levels = levels;
	record->state = _texture_replacement_resident;
	cache->bytes += record->bytes;
	cache->resident++;
	if (cache->bytes > cache->statistics.peak_bytes)
		cache->statistics.peak_bytes = cache->bytes;
	if (record->read_before)
		cache->statistics.reloads++;
	else
		cache->statistics.loads++;
	record->read_before = 1;
	texture_replacement_touch(cache, record);
}

/* drops every record that is not resident, so that a file written since is
looked for again, and closes the gaps */
static inline void texture_replacement_compact(struct texture_replacement_cache *cache)
{
	unsigned long index, kept = 0;

	for (index = 0; index < cache->count; index++)
		if (cache->records[index].state == _texture_replacement_resident)
			cache->records[kept++] = cache->records[index];
	cache->count = kept;
	cache->serial++;
}

/* drops the resident replacements the level has not drawn (all of them with
all set); returns the bytes freed, and their number in count */
static inline uint64_t texture_replacement_purge(struct texture_replacement_cache *cache, int all, unsigned long *count)
{
	uint64_t before = cache->bytes;
	unsigned long index, resident = cache->resident;

	for (index = 0; index < cache->count; index++)
		if (all || !cache->records[index].used_this_level)
			texture_replacement_drop(cache, &cache->records[index]);
	texture_replacement_compact(cache);
	cache->purge_frame = 0;
	*count = resident - cache->resident;
	return before - cache->bytes;
}

/* a map has loaded. With upscaled textures on for the level, its replacements
stay until TEXTURE_REPLACEMENT_PURGE_FRAMES frames show which of them the new
level draws; otherwise all go now. The level starts on the first set, and
the statistics start again */
static inline void texture_replacement_map_loaded(struct texture_replacement_cache *cache, int enabled)
{
	unsigned long index, count;

	if (enabled)
	{
		texture_replacement_compact(cache);
		for (index = 0; index < cache->count; index++)
			cache->records[index].used_this_level = 0;
		cache->purge_frame = cache->frame + TEXTURE_REPLACEMENT_PURGE_FRAMES;
	}
	else
		texture_replacement_purge(cache, 1, &count);
	texture_replacement_select(cache, 0);
	memset(&cache->statistics, 0, sizeof(cache->statistics));
	cache->statistics.peak_bytes = cache->bytes;
}

/* a frame begins. Returns the bytes freed if this is the frame the level's
unused replacements go at, with their number in count; else 0 */
static inline uint64_t texture_replacement_begin_frame(struct texture_replacement_cache *cache, unsigned long *count)
{
	*count = 0;
	cache->frame++;
	if (!cache->purge_frame || cache->frame < cache->purge_frame)
		return 0;
	return texture_replacement_purge(cache, 0, count);
}

/* the system warned of low memory (called after a frame's
texture_replacement_begin_frame): every replacement the last two frames did not draw goes, and the budget is
halved until the game quits (a budget above TEXTURE_REPLACEMENT_MINIMUM_BUDGET
never goes below it). Returns the bytes freed */
static inline uint64_t texture_replacement_memory_warning(struct texture_replacement_cache *cache)
{
	uint64_t before = cache->bytes;
	unsigned long index;

	for (index = 0; index < cache->count; index++)
	{
		struct texture_replacement *record = &cache->records[index];

		if (record->state == _texture_replacement_resident && texture_replacement_old(cache, record, 2))
		{
			texture_replacement_drop(cache, record);
			cache->statistics.evictions++;
		}
	}
	if (cache->budget / 2 >= TEXTURE_REPLACEMENT_MINIMUM_BUDGET)
		cache->budget /= 2;
	else if (cache->budget > TEXTURE_REPLACEMENT_MINIMUM_BUDGET)
		cache->budget = TEXTURE_REPLACEMENT_MINIMUM_BUDGET;
	cache->warnings++;
	return before - cache->bytes;
}

#endif
