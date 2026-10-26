/* Visibility answers by fixed latency (gpu_metal.m, visibility_latency): which
   of a slot's kept tests answers a read, in plain C so that
   port/ios/tests/visibility_history_probe.c runs the rules natively.

   A slot keeps its last test of each of the latest VISIBILITY_HISTORY frames
   (entry frame % VISIBILITY_HISTORY), and what the newest test displaced from
   those answered (settled). A read in frame N takes the newest kept test from
   frame N - latency or before, or settled when that is newer or nothing kept
   qualifies, or 0 when neither exists. Nothing depends on when the GPU
   finished, so every run gives the same answers on the same frames. */
#ifndef HALO_VISIBILITY_HISTORY_H
#define HALO_VISIBILITY_HISTORY_H

#include <stdatomic.h>
#include <stdint.h>

#define VISIBILITY_HISTORY 4

struct visibility_history_entry
{
	unsigned long frame;
	_Atomic uint64_t serial;   /* the test's command buffer; 0: none */
	_Atomic uint64_t answer;   /* (serial << the count's bits) | count, once written */
};

struct visibility_history
{
	struct visibility_history_entry entries[VISIBILITY_HISTORY];
	unsigned long settled_frame;
	uint64_t settled_answer;
	int settled;
};

/* the entry a test of frame takes; *displaced is set when the entry holds an older frame's test whose answer must
   become the settled one first */
static inline unsigned visibility_history_slot(const struct visibility_history *history, unsigned long frame,
	int *displaced)
{
	unsigned index = (unsigned)(frame % VISIBILITY_HISTORY);
	const struct visibility_history_entry *entry = &history->entries[index];

	*displaced = atomic_load(&entry->serial) && entry->frame != frame &&
		(!history->settled || entry->frame > history->settled_frame);
	return index;
}

/* the entry that answers a read in frame with latency frames back: its index, or -1 for the settled answer (or 0
   when there is none: *none) */
static inline int visibility_history_pick(const struct visibility_history *history, unsigned long frame, int latency,
	int *none)
{
	int index, best = -1;
	unsigned long target;

	*none = 0;
	if (frame < (unsigned long)latency)
	{
		*none = 1;
		return -1;
	}
	target = frame - (unsigned long)latency;
	for (index = 0; index < VISIBILITY_HISTORY; index++)
	{
		const struct visibility_history_entry *entry = &history->entries[index];

		if (atomic_load(&entry->serial) && entry->frame <= target &&
			(best < 0 || entry->frame > history->entries[best].frame))
			best = index;
	}
	if (best >= 0 && (!history->settled || history->entries[best].frame > history->settled_frame))
		return best;
	*none = !history->settled;
	return -1;
}

#endif
