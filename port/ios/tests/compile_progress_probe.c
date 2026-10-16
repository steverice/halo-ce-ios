/* Per-load progress retains early completions and ignores late workers from
an earlier load. The compiler's existing claim table owns deduplication. */
#include "metal_compile_progress.h"
#include "halo_loading_progress.h"
#include "metal_compile_async.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>

static struct metal_compile_progress batch = { .lock = PTHREAD_MUTEX_INITIALIZER };

static struct gpu_compile_progress snapshot(uint32_t action)
{
	struct gpu_compile_progress result;
	metal_compile_progress_update(&batch, action, &result);
	return result;
}

static void early_completions(void)
{
	struct gpu_compile_progress p;
	uint64_t first, second;
	snapshot(GPU_COMPILE_PROGRESS_BEGIN);
	first = metal_compile_progress_schedule(&batch);
	metal_compile_progress_finish(&batch, first, 1);
	second = metal_compile_progress_schedule(&batch);
	p = snapshot(GPU_COMPILE_PROGRESS_SNAPSHOT);
	assert(p.total == 2 && p.completed == 1 && !p.sealed);
	assert(halo_loading_compile_fraction(&p) == 0.0f);
	p = snapshot(GPU_COMPILE_PROGRESS_SEAL);
	assert(p.total == 2 && p.completed == 1 && p.sealed);
	assert(halo_loading_compile_fraction(&p) == 0.5f);
	/* First-draw work discovered after sealing cannot change the denominator. */
	assert(!metal_compile_progress_schedule(&batch));
	metal_compile_progress_finish(&batch, second, 1);
	p = snapshot(GPU_COMPILE_PROGRESS_SNAPSHOT);
	assert(p.total == 2 && p.completed == 2 && !p.failed);
	assert(halo_loading_compile_fraction(&p) == 1.0f);
}

static void failures_and_late_workers(void)
{
	struct gpu_compile_progress p;
	uint64_t old, current;
	snapshot(GPU_COMPILE_PROGRESS_BEGIN);
	old = metal_compile_progress_schedule(&batch);
	snapshot(GPU_COMPILE_PROGRESS_END);
	snapshot(GPU_COMPILE_PROGRESS_BEGIN);
	current = metal_compile_progress_schedule(&batch);
	snapshot(GPU_COMPILE_PROGRESS_SEAL);
	metal_compile_progress_finish(&batch, old, 1);
	p = snapshot(GPU_COMPILE_PROGRESS_SNAPSHOT);
	assert(p.total == 1 && !p.completed && !p.failed);
	metal_compile_progress_finish(&batch, current, 0);
	p = snapshot(GPU_COMPILE_PROGRESS_SNAPSHOT);
	assert(p.total == 1 && p.completed == 1 && p.failed == 1);
	assert(halo_loading_compile_fraction(&p) == 0.0f);
	snapshot(GPU_COMPILE_PROGRESS_END);
	assert(!metal_compile_progress_schedule(&batch));
}

static void cached_and_shared(void)
{
	struct metal_compile_table table;
	struct gpu_compile_progress p;
	unsigned key = 7, visits;
	void *found;
	metal_compile_table_initialize(&table);
	snapshot(GPU_COMPILE_PROGRESS_BEGIN);
	for (visits = 0; visits < 3; visits++)
	{
		/* The same library/pipeline can occur in both bundled and local lists. */
		if (metal_compile_claim(&table, &key, sizeof(key), &found) == METAL_CLAIM_NEW)
		{
			uint64_t token = metal_compile_progress_schedule(&batch);
			metal_compile_publish(&table, &key, sizeof(key), &key);
			metal_compile_progress_finish(&batch, token, 1);
		}
	}
	p = snapshot(GPU_COMPILE_PROGRESS_SEAL);
	assert(p.total == 1 && p.completed == 1);
	/* A subsequent load needs no new jobs for this cached resource. */
	snapshot(GPU_COMPILE_PROGRESS_BEGIN);
	assert(metal_compile_claim(&table, &key, sizeof(key), &found) == METAL_CLAIM_READY);
	p = snapshot(GPU_COMPILE_PROGRESS_SEAL);
	assert(!p.total && !p.completed && halo_loading_compile_fraction(&p) == 1.0f);
	free(table.entries);
	pthread_mutex_destroy(&table.lock);
}

static void *complete_on_worker(void *argument)
{
	uint64_t token = *(uint64_t *)argument;
	unsigned index;
	for (index = 0; index < 1000; index++)
		metal_compile_progress_finish(&batch, token, 1);
	return NULL;
}

static void parallel_completions(void)
{
	pthread_t threads[4];
	uint64_t token;
	unsigned index;
	struct gpu_compile_progress p;
	snapshot(GPU_COMPILE_PROGRESS_BEGIN);
	for (index = 0; index < 4000; index++)
		token = metal_compile_progress_schedule(&batch);
	snapshot(GPU_COMPILE_PROGRESS_SEAL);
	for (index = 0; index < 4; index++)
		assert(!pthread_create(&threads[index], NULL, complete_on_worker, &token));
	do
	{
		p = snapshot(GPU_COMPILE_PROGRESS_SNAPSHOT);
		assert(p.total == 4000 && p.completed <= p.total && !p.failed);
	} while (p.completed != 4000);
	for (index = 0; index < 4; index++)
		assert(!pthread_join(threads[index], NULL));
}

static void measured_animation(void)
{
	float shown = 0.0f, previous;
	unsigned index;
	/* Time alone never advances the band's measured target. */
	for (index = 0; index < 10000; index++)
		shown = halo_loading_progress_advance(shown, 0.0f, 16, 0.15f);
	assert(shown == 0.0f);
	for (index = 0; index < 200; index++)
	{
		previous = shown;
		shown = halo_loading_progress_advance(shown, 0.525f, 16, 0.15f);
		assert(shown >= previous && shown <= 0.525f);
	}
	assert(shown > 0.524f);
	/* The compile phase stops short of overall completion, even when done. */
	for (index = 0; index < 200; index++)
		shown = halo_loading_progress_advance(shown, 0.9f, 16, 0.15f);
	assert(shown > 0.899f && shown <= 0.9f);
	/* Only the existing ready-level tail supplies the final target. */
	for (index = 0; index < 200; index++)
		shown = halo_loading_progress_advance(shown, 1.0f, 16, 0.1f);
	assert(shown > 0.999f && shown <= 1.0f);
}

int main(void)
{
	early_completions();
	failures_and_late_workers();
	cached_and_shared();
	parallel_completions();
	measured_animation();
	puts("PASS: per-load compile progress, cache hits, failures, generations, threads and measured animation");
	return 0;
}
