/* Drawable timestamps are seconds on the presentation clock. This collector
   does not read a clock or infer display delivery from submission times. */
#ifndef HALO_BENCHMARK_PRESENTATION_H
#define HALO_BENCHMARK_PRESENTATION_H

#include "benchmark_metrics.h"

#define BENCHMARK_PRESENTATION_CAPACITY_MAX 65536

/* index is the immutable zero-based submission sequence within generation. */
struct benchmark_presentation_token { uint64_t generation; size_t index; };
enum benchmark_presentation_result {
    BENCHMARK_PRESENTATION_ACCEPTED,
    BENCHMARK_PRESENTATION_DUPLICATE,
    BENCHMARK_PRESENTATION_STALE,
    BENCHMARK_PRESENTATION_LATE,
    BENCHMARK_PRESENTATION_BAD_TOKEN
};
enum benchmark_presentation_span {
    BENCHMARK_PRESENTATION_NO_SPAN,
    BENCHMARK_PRESENTATION_INTERVAL,
    BENCHMARK_PRESENTATION_UNKNOWN_GAP
};
struct benchmark_presentation_record {
    uint64_t segment;
    double submitted, presented;
    int received, order_error;
    /* Derived only at seal, in submission order. Unknown gaps retain both
       endpoints, including spans >= 50 ms, separately from normal intervals. */
    enum benchmark_presentation_span span;
    size_t previous_index;
    double milliseconds;
};
/* expected = stored + overflow; stored = received + missing. Coverage uses
   expected as its denominator. positive includes finite positive timestamps
   with order errors; those errors never contribute normal intervals. */
struct benchmark_presentation_summary {
    size_t expected, stored, overflow, received, positive, unpresented;
    size_t missing, invalid, duplicates, bad_tokens, order_errors;
    size_t intervals, stalls, unknown_gaps, unknown_stalls;
    int timed_out;
    double callback_coverage, positive_coverage;
    struct benchmark_distribution interval;
};
struct benchmark_presentation {
    uint64_t generation, segment;
    struct benchmark_presentation_record *records;
    double *sort_buffer;
    size_t capacity, count, expected, overflow, received, duplicates, bad_tokens;
    int segment_open, admission_closed, sealed;
    struct benchmark_presentation_summary summary;
};

/* Zero-initialize before first use. All operations, including reads, require
   the owner's external lock while callbacks can access this object. The
   owner validates scalar tokens under that lock before dereferencing storage;
   never capture a collector pointer in a callback that can outlive its owner.
   Reset allocates only at run start and requires a strictly increasing nonzero
   generation. It fails without changing the old collector. No generation may
   be reused, even after free. The owner supplies that process-lifetime ID. */
/* Reserve only when a supported presentation source will supply the callback.
   Capability reporting belongs to the platform owner. */
int benchmark_presentation_reset(struct benchmark_presentation *p, uint64_t generation, size_t capacity);
void benchmark_presentation_free(struct benchmark_presentation *p);
/* IDs increase within a run. Closing a segment admits callbacks already
   reserved in it. Opening another segment never joins their intervals. */
int benchmark_presentation_open(struct benchmark_presentation *p, uint64_t segment);
void benchmark_presentation_close_segment(struct benchmark_presentation *p);
struct benchmark_presentation_token benchmark_presentation_reserve(struct benchmark_presentation *p, double submitted);
enum benchmark_presentation_result benchmark_presentation_complete(struct benchmark_presentation *p,
    struct benchmark_presentation_token token, double presented);
void benchmark_presentation_close(struct benchmark_presentation *p);
size_t benchmark_presentation_pending(const struct benchmark_presentation *p);
/* The owner drains outside the callback lock, then seals under it. Seal does
   not wait. The returned summary and records stay immutable until reset/free.
   Late and stale returns must be counted by the owner if needed after seal. */
const struct benchmark_presentation_summary *benchmark_presentation_seal(struct benchmark_presentation *p, int timed_out);

#endif
