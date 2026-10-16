/* Per-load preparation jobs. Workers keep a generation token so a canceled
or superseded load cannot receive an earlier load's completions. Scheduling
stops at seal; later first-draw work still gates readiness through the
backend's outstanding count, without changing this batch's denominator. */
#ifndef METAL_COMPILE_PROGRESS_H
#define METAL_COMPILE_PROGRESS_H

#include "gpu.h"
#include <pthread.h>
#include <string.h>

struct metal_compile_progress
{
	pthread_mutex_t lock;
	uint64_t generation;
	int active;
	struct gpu_compile_progress progress;
};

static inline void metal_compile_progress_update(struct metal_compile_progress *batch, uint32_t action,
	struct gpu_compile_progress *progress)
{
	pthread_mutex_lock(&batch->lock);
	if (action == GPU_COMPILE_PROGRESS_BEGIN)
	{
		batch->generation++;
		batch->active = 1;
		memset(&batch->progress, 0, sizeof(batch->progress));
	}
	else if (action == GPU_COMPILE_PROGRESS_SEAL)
		batch->progress.sealed = 1;
	else if (action == GPU_COMPILE_PROGRESS_END)
		batch->active = 0;
	*progress = batch->progress;
	pthread_mutex_unlock(&batch->lock);
}

/* Called only for a newly claimed job, before its worker is dispatched. */
static inline uint64_t metal_compile_progress_schedule(struct metal_compile_progress *batch)
{
	uint64_t token = 0;
	pthread_mutex_lock(&batch->lock);
	if (batch->active && !batch->progress.sealed)
	{
		batch->progress.total++;
		token = batch->generation;
	}
	pthread_mutex_unlock(&batch->lock);
	return token;
}

static inline void metal_compile_progress_finish(struct metal_compile_progress *batch, uint64_t token, int succeeded)
{
	pthread_mutex_lock(&batch->lock);
	if (token && batch->active && token == batch->generation)
	{
		batch->progress.completed++;
		batch->progress.failed += !succeeded;
	}
	pthread_mutex_unlock(&batch->lock);
}

#endif
