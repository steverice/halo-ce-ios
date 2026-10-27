/*
METAL_COMPILE_ASYNC.H

What the Metal backend (gpu_metal.m) keeps to compile pipelines off the game
thread (display.async_shaders), so that no frame waits on Metal's compiler:

- a table of compiled objects by key, which worker threads fill while the
  game thread reads it. A key is claimed once, and the claimer compiles it;
  until its object is published it reads as pending, and a draw that needs it
  is skipped. A published object is kept for good, so a reader may hold it
  unretained, and a key published twice (which a claim prevents) keeps the
  first object and hands the second back to be released;
- the frame's skipped draws, in runs of draws to one target: a run whose
  draws were all skipped is a pass that drew nothing, and the window shows
  its last whole frame again rather than such a frame (metal_hold_frame);
- the launch's totals, for its log line.

Plain C, for port/ios/tests/metal_compile_probe.c.
*/
#ifndef METAL_COMPILE_ASYNC_H
#define METAL_COMPILE_ASYNC_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the largest key: gpu_metal.m's pipeline_key */
#define METAL_COMPILE_KEY_BYTES 48

enum
{
	METAL_CLAIM_NEW,     /* the caller claimed the key, and compiles and publishes it */
	METAL_CLAIM_PENDING, /* another claimed it, and it isn't published yet */
	METAL_CLAIM_READY,   /* published: the object is set */
};

struct metal_compile_entry
{
	unsigned char key[METAL_COMPILE_KEY_BYTES];
	uint32_t size; /* 0: an empty slot */
	int ready;
	void *object;
};

struct metal_compile_table
{
	pthread_mutex_t lock;
	struct metal_compile_entry *entries;
	size_t capacity, count;
	/* claimed and not yet published */
	size_t pending;
};

static inline void metal_compile_table_initialize(struct metal_compile_table *table)
{
	memset(table, 0, sizeof(*table));
	pthread_mutex_init(&table->lock, NULL);
}

static inline uint32_t metal_compile_hash(const void *key, size_t size)
{
	const unsigned char *bytes = key;
	uint32_t hash = 2166136261u;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	return hash ^ (hash >> 15);
}

/* the key's slot, or the empty one it would go in (the lock held, room left) */
static inline struct metal_compile_entry *metal_compile_slot(struct metal_compile_table *table, const void *key,
	size_t size)
{
	size_t index = metal_compile_hash(key, size) & (table->capacity - 1);

	for (;; index = (index + 1) & (table->capacity - 1))
	{
		struct metal_compile_entry *entry = &table->entries[index];

		if (!entry->size || (entry->size == size && !memcmp(entry->key, key, size)))
			return entry;
	}
}

/* room for one more key, at most half full (the lock held); 0 if out of memory */
static inline int metal_compile_room(struct metal_compile_table *table)
{
	struct metal_compile_entry *old = table->entries;
	size_t old_capacity = table->capacity, index;

	if (table->capacity && (table->count + 1) * 2 <= table->capacity)
		return 1;
	table->capacity = table->capacity ? table->capacity * 2 : 256;
	table->entries = calloc(table->capacity, sizeof(*table->entries));
	if (!table->entries)
	{
		table->entries = old;
		table->capacity = old_capacity;
		return 0;
	}
	for (index = 0; index < old_capacity; index++)
	{
		if (old[index].size)
			*metal_compile_slot(table, old[index].key, old[index].size) = old[index];
	}
	free(old);
	return 1;
}

/* the key's state (METAL_CLAIM_*), claiming it if nobody has: object is set
when it's ready. An oversized key, or no memory, reads as pending, so the
draw is skipped rather than compiled twice */
static inline int metal_compile_claim(struct metal_compile_table *table, const void *key, size_t size, void **object)
{
	struct metal_compile_entry *entry;
	int state;

	*object = NULL;
	if (size == 0 || size > METAL_COMPILE_KEY_BYTES)
		return METAL_CLAIM_PENDING;
	pthread_mutex_lock(&table->lock);
	if (!metal_compile_room(table))
	{
		pthread_mutex_unlock(&table->lock);
		return METAL_CLAIM_PENDING;
	}
	entry = metal_compile_slot(table, key, size);
	if (!entry->size)
	{
		memcpy(entry->key, key, size);
		entry->size = (uint32_t)size;
		entry->ready = 0;
		entry->object = NULL;
		table->count++;
		table->pending++;
		state = METAL_CLAIM_NEW;
	}
	else if (entry->ready)
	{
		*object = entry->object;
		state = METAL_CLAIM_READY;
	}
	else
		state = METAL_CLAIM_PENDING;
	pthread_mutex_unlock(&table->lock);
	return state;
}

/* the key's object, or NULL if it isn't published (nothing claimed) */
static inline void *metal_compile_lookup(struct metal_compile_table *table, const void *key, size_t size)
{
	void *object = NULL;

	if (size == 0 || size > METAL_COMPILE_KEY_BYTES)
		return NULL;
	pthread_mutex_lock(&table->lock);
	if (table->capacity)
	{
		struct metal_compile_entry *entry = metal_compile_slot(table, key, size);

		if (entry->size && entry->ready)
			object = entry->object;
	}
	pthread_mutex_unlock(&table->lock);
	return object;
}

/* publishes the key's object (claimed or not), which the table then owns;
returns NULL, or object itself when the key already had one (the first is
kept), for the caller to release */
static inline void *metal_compile_publish(struct metal_compile_table *table, const void *key, size_t size, void *object)
{
	struct metal_compile_entry *entry;
	void *extra = NULL;

	if (size == 0 || size > METAL_COMPILE_KEY_BYTES)
		return object;
	pthread_mutex_lock(&table->lock);
	if (!metal_compile_room(table))
	{
		pthread_mutex_unlock(&table->lock);
		return object;
	}
	entry = metal_compile_slot(table, key, size);
	if (!entry->size)
	{
		memcpy(entry->key, key, size);
		entry->size = (uint32_t)size;
		table->count++;
		entry->ready = 1;
		entry->object = object;
	}
	else if (entry->ready)
		extra = object;
	else
	{
		entry->ready = 1;
		entry->object = object;
		table->pending--;
	}
	pthread_mutex_unlock(&table->lock);
	return extra;
}

static inline size_t metal_compile_pending(struct metal_compile_table *table)
{
	size_t pending;

	pthread_mutex_lock(&table->lock);
	pending = table->pending;
	pthread_mutex_unlock(&table->lock);
	return pending;
}

/* ---------- the frame's skipped draws (game thread only) */

struct metal_skip_frame
{
	uint64_t target;                     /* the current run's target */
	unsigned long run_drawn, run_skipped;
	int broken;                          /* a run this frame drew nothing */
	unsigned long skipped;               /* draws skipped this frame */
};

static inline void metal_skip_run_end(struct metal_skip_frame *frame)
{
	if (frame->run_skipped && !frame->run_drawn)
		frame->broken = 1;
	frame->run_drawn = frame->run_skipped = 0;
}

/* a draw to target (its color target, or its depth target without one),
drawn or skipped for a pipeline still compiling */
static inline void metal_skip_draw(struct metal_skip_frame *frame, uint64_t target, int skipped)
{
	if (target != frame->target)
	{
		metal_skip_run_end(frame);
		frame->target = target;
	}
	if (skipped)
	{
		frame->run_skipped++;
		frame->skipped++;
	}
	else
		frame->run_drawn++;
}

/* whether a pass drew nothing so far this frame (the last run included) */
static inline int metal_skip_frame_broken(const struct metal_skip_frame *frame)
{
	return frame->broken || (frame->run_skipped && !frame->run_drawn);
}

/* the frame's skipped draws, and whether a pass drew nothing; starts the next frame */
static inline unsigned long metal_skip_frame_end(struct metal_skip_frame *frame, int *broken)
{
	unsigned long skipped;

	metal_skip_run_end(frame);
	*broken = frame->broken;
	skipped = frame->skipped;
	memset(frame, 0, sizeof(*frame));
	return skipped;
}

/* ---------- holding the window's picture */

struct metal_hold
{
	int whole_shown;    /* a frame with no broken pass has shown */
	unsigned held;      /* frames held in a row */
};

/* whether the window keeps showing its last picture instead of this frame:
a broken frame is held once a whole one has shown, for at most limit frames
in a row (then shown, so a pipeline that takes long doesn't freeze the
picture) */
static inline int metal_hold_frame(struct metal_hold *hold, int broken, unsigned limit)
{
	if (broken && hold->whole_shown && hold->held < limit)
	{
		hold->held++;
		return 1;
	}
	hold->held = 0;
	if (!broken)
		hold->whole_shown = 1;
	return 0;
}

/* ---------- holding the level's first frames */

/* the loading screen's end arms it (GPU_COMPILE_PROGRESS_FIRST_FRAME): for
the level's first frames (window), a frame that has draws skipped for
pipelines still compiling (ones the map's shader list did not name) is not
shown, so the window keeps the loading screen's last picture, or the last
frame that drew everything. The window does not end at the first frame with
nothing skipped: the frames after it draw what the first did not (the HUD,
the weapon raised, an effect, the structure from a vertex shader the list
pairs with other pixel shaders: Blood Gulch's ground on an iPhone, the sky
alone for three frames), and those make more pipelines. At most limit frames
in a row are held, so a long compile doesn't freeze the picture */
struct metal_start_hold
{
	int armed;
	unsigned frames;    /* the window's frames so far */
	unsigned held;      /* frames held */
	unsigned shown;     /* frames shown with draws skipped */
	unsigned in_a_row;  /* frames held in a row */
};

static inline void metal_start_hold_arm(struct metal_start_hold *hold)
{
	memset(hold, 0, sizeof(*hold));
	hold->armed = 1;
}

/* whether this frame, which skipped this many draws, is not shown; window
frames from the arming, at most limit held in a row */
static inline int metal_start_hold_frame(struct metal_start_hold *hold, unsigned long skipped, unsigned window,
	unsigned limit)
{
	if (!hold->armed)
		return 0;
	if (++hold->frames >= window)
		hold->armed = 0;
	if (!skipped)
	{
		hold->in_a_row = 0;
		return 0;
	}
	if (hold->in_a_row >= limit)
	{
		hold->in_a_row = 0;
		hold->shown++;
		return 0;
	}
	hold->in_a_row++;
	hold->held++;
	return 1;
}

/* ---------- the launch's totals */

struct metal_compile_totals
{
	pthread_mutex_t lock;
	unsigned long compiles;           /* finished off the game thread */
	double slowest;                   /* seconds, the slowest of them */
	int slowest_kind;
	unsigned long draws_skipped, frames_skipping, frames_held;
	int summarized;                   /* the once-a-launch line was logged */
	unsigned long reported_skips;     /* draws_skipped at the last line */
};

static inline void metal_compile_totals_initialize(struct metal_compile_totals *totals)
{
	memset(totals, 0, sizeof(*totals));
	pthread_mutex_init(&totals->lock, NULL);
}

/* a compile finished, on any thread */
static inline void metal_compile_finished(struct metal_compile_totals *totals, int kind, double seconds)
{
	pthread_mutex_lock(&totals->lock);
	totals->compiles++;
	if (seconds > totals->slowest)
	{
		totals->slowest = seconds;
		totals->slowest_kind = kind;
	}
	pthread_mutex_unlock(&totals->lock);
}

/* a frame ended (game thread) with outstanding compiles still running;
1 once a launch: the first frame that skipped nothing with nothing left to
compile, after compiles ran */
static inline int metal_compile_frame_end(struct metal_compile_totals *totals, unsigned long skipped, int held,
	unsigned long outstanding)
{
	int due;

	pthread_mutex_lock(&totals->lock);
	totals->draws_skipped += skipped;
	totals->frames_skipping += skipped != 0;
	totals->frames_held += held != 0;
	due = !totals->summarized && totals->compiles && !outstanding && !skipped;
	if (due)
	{
		totals->summarized = 1;
		totals->reported_skips = totals->draws_skipped;
	}
	pthread_mutex_unlock(&totals->lock);
	return due;
}

#endif
