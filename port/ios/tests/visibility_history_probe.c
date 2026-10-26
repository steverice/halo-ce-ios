/* visibility_history.h's rules, natively (tools/ios_test.py): a made-up game that tests and reads slots on chosen
   frames, with the GPU finishing tests at random times. The answers must come from the expected frames and must not
   depend on when the GPU finished. */
#include "visibility_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(stderr, "visibility_history_probe: line %d: %s\n", __LINE__, #condition); } } while (0)

#define BITS 25

/* one slot played through: a test on frames where tested(frame) and two on frames where twice(frame), each with the
   answer frame * 10 + its order in the frame; a read every frame. The GPU writes an answer when (frame + seed) says,
   or when it is waited for. Returns the reads' answers in out */
static void play(unsigned seed, int latency, int (*tested)(unsigned long), int (*twice)(unsigned long), unsigned long count,
    uint32_t *out) {
    struct visibility_history history;
    uint64_t serial = 1;
    uint64_t pending_serial[64]; struct visibility_history_entry *pending_entry[64]; uint64_t pending_answer[64];
    int pending = 0;
    unsigned long frame;

    memset(&history, 0, sizeof(history));
    srand(seed);
    for (frame = 0; frame < count; frame++) {
        int tests = tested(frame) ? (twice(frame) ? 2 : 1) : 0, test;

        for (test = 0; test < tests; test++) {
            int displaced;
            unsigned index = visibility_history_slot(&history, frame, &displaced);
            struct visibility_history_entry *entry = &history.entries[index];

            if (displaced) {
                /* (waited for: written now if the GPU hadn't) */
                uint64_t want = atomic_load(&entry->serial);
                int at;
                for (at = 0; at < pending; at++)
                    if (pending_entry[at] == entry && pending_serial[at] == want) {
                        atomic_store(&entry->answer, (want << BITS) | pending_answer[at]);
                        pending_serial[at] = 0;
                    }
                CHECK((atomic_load(&entry->answer) >> BITS) == want);
                history.settled_answer = atomic_load(&entry->answer) & ((1ull << BITS) - 1);
                history.settled_frame = entry->frame;
                history.settled = 1;
            }
            entry->frame = frame;
            atomic_store(&entry->answer, 0);
            atomic_store(&entry->serial, ++serial);
            if (pending < 64) {
                pending_serial[pending] = serial; pending_entry[pending] = entry;
                pending_answer[pending] = frame * 10 + (unsigned long)test; pending++;
            }
        }
        /* the GPU finishes some of what is pending, by the seed */
        {
            int at;
            for (at = 0; at < pending; at++)
                if (pending_serial[at] && rand() % 3 == 0) {
                    if (atomic_load(&pending_entry[at]->serial) == pending_serial[at])
                        atomic_store(&pending_entry[at]->answer, (pending_serial[at] << BITS) | pending_answer[at]);
                    pending_serial[at] = 0;
                }
            int kept = 0;
            for (at = 0; at < pending; at++)
                if (pending_serial[at]) {
                    pending_serial[kept] = pending_serial[at]; pending_entry[kept] = pending_entry[at];
                    pending_answer[kept] = pending_answer[at]; kept++;
                }
            pending = kept;
        }
        {
            int none, index = visibility_history_pick(&history, frame, latency, &none);

            if (index >= 0) {
                struct visibility_history_entry *entry = &history.entries[index];
                uint64_t want = atomic_load(&entry->serial);
                int at;
                for (at = 0; at < pending; at++)
                    if (pending_entry[at] == entry && pending_serial[at] == want) {
                        atomic_store(&entry->answer, (want << BITS) | pending_answer[at]);
                        pending_serial[at] = 0;
                    }
                CHECK((atomic_load(&entry->answer) >> BITS) == want);
                out[frame] = (uint32_t)(atomic_load(&entry->answer) & ((1ull << BITS) - 1));
            } else
                out[frame] = none ? 0xffffffffu : (uint32_t)history.settled_answer;
        }
    }
}

static int every(unsigned long frame) { (void)frame; return 1; }
static int never(unsigned long frame) { (void)frame; return 0; }
static int sparse(unsigned long frame) { return frame % 7 == 0 || frame % 11 == 3; }
static int some(unsigned long frame) { return frame % 5 == 2; }

/* the answer expected in frame: the last test of the newest tested frame at or before frame - latency */
static uint32_t expected(int latency, int (*tested)(unsigned long), int (*twice)(unsigned long), unsigned long frame) {
    long at;

    if (frame < (unsigned long)latency) return 0xffffffffu;
    for (at = (long)(frame - (unsigned long)latency); at >= 0; at--)
        if (tested((unsigned long)at)) return (uint32_t)at * 10 + (twice((unsigned long)at) ? 1 : 0);
    return 0xffffffffu;
}

int main(void) {
    enum { FRAMES = 400 };
    static uint32_t first[FRAMES], again[FRAMES];
    int (*patterns[][2])(unsigned long) = { { every, never }, { every, some }, { sparse, never }, { sparse, some } };
    int latency, pattern;
    unsigned seed;

    for (latency = 1; latency < VISIBILITY_HISTORY; latency++)
        for (pattern = 0; pattern < 4; pattern++) {
            play(1, latency, patterns[pattern][0], patterns[pattern][1], FRAMES, first);
            for (unsigned long frame = 0; frame < FRAMES; frame++)
                CHECK(first[frame] == expected(latency, patterns[pattern][0], patterns[pattern][1], frame));
            /* the GPU's timing never changes an answer */
            for (seed = 2; seed < 12; seed++) {
                play(seed, latency, patterns[pattern][0], patterns[pattern][1], FRAMES, again);
                CHECK(!memcmp(first, again, sizeof(first)));
            }
        }
    printf("visibility_history_probe: %d checks %s\n", checks, failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
