/* metal_compile_async.h: the Metal backend's table of pipelines compiled off
the game thread, the frame's skipped draws and the window's hold. Threads
claim, compile and publish keys while others read them, as gpu_metal.m's
workers and game thread do; tools/ios_test.py runs it under the thread
sanitizer and under the address and undefined-behavior sanitizers. */
#include "metal_compile_async.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

/* a pipeline_key's size, and a stand-in for its compiled object */
#define KEY_SIZE 48
#define KEYS 2000
#define THREADS 8

struct object
{
	unsigned char key[KEY_SIZE];
	int built;
};

static struct metal_compile_table table;
static _Atomic unsigned long compiled, released;

static void make_key(unsigned char key[KEY_SIZE], unsigned index)
{
	memset(key, 0, KEY_SIZE);
	memcpy(key, &index, sizeof(index));
	/* (a difference in the last byte alone, as pipeline_key's last attribute kind) */
	key[KEY_SIZE - 1] = (unsigned char)(index * 7);
}

/* each thread walks every key from its own start: a key claimed elsewhere
reads as pending until published, then as the object for its key, built
whole */
static void *worker(void *argument)
{
	unsigned start = (unsigned)(uintptr_t)argument * (KEYS / THREADS), step;

	for (step = 0; step < KEYS * 2; step++)
	{
		unsigned char key[KEY_SIZE];
		void *found;
		int state;

		make_key(key, (start + step) % KEYS);
		state = metal_compile_claim(&table, key, KEY_SIZE, &found);
		if (state == METAL_CLAIM_NEW)
		{
			struct object *object = malloc(sizeof(*object));

			memcpy(object->key, key, KEY_SIZE);
			object->built = 1;
			atomic_fetch_add(&compiled, 1);
			assert(!metal_compile_publish(&table, key, KEY_SIZE, object));
		}
		else if (state == METAL_CLAIM_READY)
		{
			const struct object *object = found;

			assert(object && object->built && !memcmp(object->key, key, KEY_SIZE));
		}
		else
			assert(state == METAL_CLAIM_PENDING && !found);
		found = metal_compile_lookup(&table, key, KEY_SIZE);
		assert(!found || !memcmp(((const struct object *)found)->key, key, KEY_SIZE));
	}
	return NULL;
}

/* the same key compiled on two threads at once, unclaimed (the double
compile a claim prevents): one object stays, the other comes back */
static void *racer(void *argument)
{
	unsigned char key[KEY_SIZE];
	struct object *object = malloc(sizeof(*object));
	void *extra;

	make_key(key, KEYS + 1);
	memcpy(object->key, key, KEY_SIZE);
	object->built = (int)(uintptr_t)argument;
	extra = metal_compile_publish(&table, key, KEY_SIZE, object);
	if (extra)
	{
		assert(extra == object);
		free(extra);
		atomic_fetch_add(&released, 1);
	}
	return NULL;
}

int main(void)
{
	pthread_t threads[THREADS];
	unsigned char key[KEY_SIZE];
	void *found;
	unsigned index;

	metal_compile_table_initialize(&table);

	/* a claim: new once, then pending, then the published object */
	make_key(key, 0);
	assert(metal_compile_claim(&table, key, KEY_SIZE, &found) == METAL_CLAIM_NEW && !found);
	assert(metal_compile_claim(&table, key, KEY_SIZE, &found) == METAL_CLAIM_PENDING && !found);
	assert(!metal_compile_lookup(&table, key, KEY_SIZE));
	assert(metal_compile_pending(&table) == 1);
	{
		struct object *object = malloc(sizeof(*object));

		memcpy(object->key, key, KEY_SIZE);
		object->built = 1;
		assert(!metal_compile_publish(&table, key, KEY_SIZE, object));
		assert(metal_compile_claim(&table, key, KEY_SIZE, &found) == METAL_CLAIM_READY && found == object);
		assert(metal_compile_lookup(&table, key, KEY_SIZE) == object);
		/* published again: the first stays, the second comes back */
		{
			struct object *second = malloc(sizeof(*second));

			assert(metal_compile_publish(&table, key, KEY_SIZE, second) == second);
			free(second);
		}
		assert(metal_compile_lookup(&table, key, KEY_SIZE) == object);
	}
	assert(metal_compile_pending(&table) == 0);
	compiled = 1;
	/* an oversized key reads as pending, never claimed */
	{
		unsigned char big[METAL_COMPILE_KEY_BYTES + 1] = { 0 };

		assert(metal_compile_claim(&table, big, sizeof(big), &found) == METAL_CLAIM_PENDING);
		assert(metal_compile_pending(&table) == 0);
	}

	/* threads claiming, compiling, publishing and reading at once, through
	the table's growth: every key compiled exactly once */
	for (index = 0; index < THREADS; index++)
		pthread_create(&threads[index], NULL, worker, (void *)(uintptr_t)index);
	for (index = 0; index < THREADS; index++)
		pthread_join(threads[index], NULL);
	assert(compiled == KEYS);
	assert(metal_compile_pending(&table) == 0);
	for (index = 0; index < KEYS; index++)
	{
		const struct object *object;

		make_key(key, index);
		object = metal_compile_lookup(&table, key, KEY_SIZE);
		assert(object && object->built && !memcmp(object->key, key, KEY_SIZE));
	}
	for (index = 0; index < THREADS; index++)
		pthread_create(&threads[index], NULL, racer, (void *)(uintptr_t)(index + 1));
	for (index = 0; index < THREADS; index++)
		pthread_join(threads[index], NULL);
	assert(released == THREADS - 1);
	make_key(key, KEYS + 1);
	assert(metal_compile_lookup(&table, key, KEY_SIZE));

	/* the frame's skips: a pass whose draws were all skipped breaks the
	frame; one that drew some of them doesn't */
	{
		struct metal_skip_frame frame = { 0 };
		int broken;

		metal_skip_draw(&frame, 1, 0);
		metal_skip_draw(&frame, 1, 1);
		metal_skip_draw(&frame, 2, 0);
		assert(metal_skip_frame_end(&frame, &broken) == 1 && !broken);
		metal_skip_draw(&frame, 1, 0);
		metal_skip_draw(&frame, 2, 1);
		metal_skip_draw(&frame, 2, 1);
		metal_skip_draw(&frame, 3, 0);
		assert(metal_skip_frame_end(&frame, &broken) == 2 && broken);
		/* the last run counts at the frame's end */
		metal_skip_draw(&frame, 1, 0);
		assert(!metal_skip_frame_broken(&frame));
		metal_skip_draw(&frame, 4, 1);
		assert(metal_skip_frame_broken(&frame));
		assert(metal_skip_frame_end(&frame, &broken) == 1 && broken);
		/* and each frame starts clean */
		assert(metal_skip_frame_end(&frame, &broken) == 0 && !broken);
	}

	/* the window's hold: nothing to hold before a whole frame has shown, then
	broken frames held up to the limit, then one shown */
	{
		struct metal_hold hold = { 0 };

		assert(!metal_hold_frame(&hold, 1, 3));
		assert(!metal_hold_frame(&hold, 0, 3));
		assert(metal_hold_frame(&hold, 1, 3));
		assert(metal_hold_frame(&hold, 1, 3));
		assert(metal_hold_frame(&hold, 1, 3));
		assert(!metal_hold_frame(&hold, 1, 3));
		assert(metal_hold_frame(&hold, 1, 3));
		assert(!metal_hold_frame(&hold, 0, 3));
	}

	/* the level's first frames: nothing held until armed; held while a draw is skipped, up to the limit; the
	first frame with nothing skipped ends it, and a later skipping frame is not held */
	{
		struct metal_start_hold start = { 0 };

		assert(!metal_start_hold_frame(&start, 5, 3));
		metal_start_hold_arm(&start);
		assert(metal_start_hold_frame(&start, 5, 3));
		assert(metal_start_hold_frame(&start, 2, 3));
		assert(!metal_start_hold_frame(&start, 0, 3));
		assert(!start.armed && !metal_start_hold_frame(&start, 5, 3));
		metal_start_hold_arm(&start);
		assert(metal_start_hold_frame(&start, 1, 2));
		assert(metal_start_hold_frame(&start, 1, 2));
		assert(!metal_start_hold_frame(&start, 1, 2));
		assert(!start.armed);
	}

	/* the launch's line: once, at the first frame with nothing skipped and
	nothing left to compile, after compiles ran */
	{
		struct metal_compile_totals totals;

		metal_compile_totals_initialize(&totals);
		assert(!metal_compile_frame_end(&totals, 0, 0, 0));
		metal_compile_finished(&totals, 2, 0.25);
		metal_compile_finished(&totals, 0, 0.5);
		metal_compile_finished(&totals, 2, 0.125);
		assert(totals.slowest == 0.5 && totals.slowest_kind == 0 && totals.compiles == 3);
		assert(!metal_compile_frame_end(&totals, 40, 1, 3));
		assert(!metal_compile_frame_end(&totals, 0, 0, 1));
		assert(!metal_compile_frame_end(&totals, 5, 0, 0));
		assert(metal_compile_frame_end(&totals, 0, 0, 0));
		assert(!metal_compile_frame_end(&totals, 0, 0, 0));
		assert(totals.draws_skipped == 45 && totals.frames_skipping == 2 && totals.frames_held == 1);
		assert(totals.reported_skips == 45);
	}

	printf("PASS: Metal compile table: %d keys compiled once each on %d threads; skips, hold and totals\n", KEYS,
		THREADS);
	return 0;
}
