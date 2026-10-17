#include "benchmark_presentation.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void benchmark_presentation_free(struct benchmark_presentation *p) {
    uint64_t generation = p->generation;
    free(p->records);
    free(p->sort_buffer);
    memset(p, 0, sizeof(*p));
    /* Retain the generation fence even when the storage is released. */
    p->generation = generation;
    p->sealed = p->admission_closed = 1;
}

int benchmark_presentation_reset(struct benchmark_presentation *p, uint64_t generation, size_t capacity) {
    if (!generation || generation <= p->generation || !capacity || capacity > BENCHMARK_PRESENTATION_CAPACITY_MAX)
        return 0;
    struct benchmark_presentation_record *records = calloc(capacity, sizeof(*records));
    double *sort_buffer = malloc(capacity * sizeof(*sort_buffer));
    if (!records || !sort_buffer) {
        free(records);
        free(sort_buffer);
        return 0;
    }
    benchmark_presentation_free(p);
    memset(p, 0, sizeof(*p));
    p->generation = generation;
    p->records = records;
    p->sort_buffer = sort_buffer;
    p->capacity = capacity;
    return 1;
}

int benchmark_presentation_open(struct benchmark_presentation *p, uint64_t segment) {
    if (!p->records || p->admission_closed || p->sealed || !segment || segment <= p->segment)
        return 0;
    p->segment = segment;
    p->segment_open = 1;
    return 1;
}

void benchmark_presentation_close_segment(struct benchmark_presentation *p) {
    if (!p->sealed) p->segment_open = 0;
}

struct benchmark_presentation_token benchmark_presentation_reserve(struct benchmark_presentation *p, double submitted) {
    struct benchmark_presentation_token token = {0};
    if (p->sealed || p->admission_closed || !p->segment_open) return token;
    p->expected++;
    if (p->count == p->capacity) {
        p->overflow++;
        return token;
    }
    token.generation = p->generation;
    token.index = p->count++;
    struct benchmark_presentation_record *record = &p->records[token.index];
    record->segment = p->segment;
    record->submitted = submitted;
    return token;
}

enum benchmark_presentation_result benchmark_presentation_complete(struct benchmark_presentation *p,
    struct benchmark_presentation_token token, double presented) {
    if (!token.generation || token.generation != p->generation) return BENCHMARK_PRESENTATION_STALE;
    if (p->sealed) return BENCHMARK_PRESENTATION_LATE;
    if (token.index >= p->count) {
        p->bad_tokens++;
        return BENCHMARK_PRESENTATION_BAD_TOKEN;
    }
    struct benchmark_presentation_record *record = &p->records[token.index];
    if (record->received) {
        p->duplicates++;
        return BENCHMARK_PRESENTATION_DUPLICATE;
    }
    record->presented = presented;
    record->received = 1;
    p->received++;
    return BENCHMARK_PRESENTATION_ACCEPTED;
}

void benchmark_presentation_close(struct benchmark_presentation *p) {
    if (!p->sealed) {
        p->admission_closed = 1;
        p->segment_open = 0;
    }
}

size_t benchmark_presentation_pending(const struct benchmark_presentation *p) {
    return p->count - p->received;
}

static int compare_double(const void *a, const void *b) {
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}

const struct benchmark_presentation_summary *benchmark_presentation_seal(struct benchmark_presentation *p, int timed_out) {
    if (p->sealed) return &p->summary;
    benchmark_presentation_close(p);
    struct benchmark_presentation_summary *s = &p->summary;
    s->expected = p->expected;
    s->stored = p->count;
    s->overflow = p->overflow;
    s->received = p->received;
    s->missing = benchmark_presentation_pending(p);
    s->duplicates = p->duplicates;
    s->bad_tokens = p->bad_tokens;
    s->timed_out = !!timed_out;
    size_t previous = 0;
    int have_previous = 0;
    uint64_t segment = 0;
    double mean = 0;
    for (size_t index = 0; index < p->count; index++) {
        struct benchmark_presentation_record *record = &p->records[index];
        if (segment != record->segment) {
            segment = record->segment;
            have_previous = 0;
        }
        if (!record->received) continue;
        if (record->presented == 0) { s->unpresented++; continue; }
        if (!isfinite(record->presented) || record->presented < 0) { s->invalid++; continue; }
        s->positive++;
        if (have_previous) {
            const struct benchmark_presentation_record *before = &p->records[previous];
            /* Do not sort raw timestamps: that would erase contradictions.
               An out-of-order timestamp cannot advance the high watermark. */
            if (record->presented <= before->presented) {
                record->order_error = 1;
                s->order_errors++;
                continue;
            }
            record->previous_index = previous;
            double milliseconds = (record->presented - before->presented) * 1000;
            if (!isfinite(milliseconds)) {
                /* Preserve the positive endpoints and known long gap, while
                   marking its millisecond duration unavailable for reports. */
                record->span = BENCHMARK_PRESENTATION_INVALID_GAP;
                s->invalid_spans++;
                s->unknown_gaps++;
                s->unknown_stalls++;
            } else {
                record->milliseconds = milliseconds;
                if (index == previous + 1) {
                    record->span = BENCHMARK_PRESENTATION_INTERVAL;
                    p->sort_buffer[s->intervals++] = milliseconds;
                    /* A sum can overflow even when every interval is finite. */
                    mean += (milliseconds - mean) / s->intervals;
                    if (milliseconds >= BENCHMARK_STALL_MILLISECONDS) s->stalls++;
                } else {
                    record->span = BENCHMARK_PRESENTATION_UNKNOWN_GAP;
                    s->unknown_gaps++;
                    if (milliseconds >= BENCHMARK_STALL_MILLISECONDS) s->unknown_stalls++;
                }
            }
        }
        previous = index;
        have_previous = 1;
    }
    if (s->expected) {
        s->callback_coverage = (double)s->received / s->expected;
        s->positive_coverage = (double)s->positive / s->expected;
    }
    if (s->intervals) {
        size_t count = s->intervals;
        qsort(p->sort_buffer, count, sizeof(*p->sort_buffer), compare_double);
        s->interval.mean = mean;
        s->interval.median = p->sort_buffer[(size_t)ceil(count * 0.5) - 1];
        s->interval.high = p->sort_buffer[(size_t)ceil(count * 0.95) - 1];
        s->interval.higher = p->sort_buffer[(size_t)ceil(count * 0.99) - 1];
        s->interval.worst = p->sort_buffer[count - 1];
    }
    p->sealed = 1;
    return s;
}
