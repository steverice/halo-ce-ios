/* The device benchmark's measurements for one run (benchmark_metrics.h). */
#include "benchmark_metrics.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void benchmark_metrics_free(struct benchmark_metrics *metrics) {
    free(metrics->interval); free(metrics->cpu); free(metrics->gpu);
    free(metrics->simulation); free(metrics->render);
    memset(metrics, 0, sizeof(*metrics));
}

void benchmark_metrics_reset(struct benchmark_metrics *metrics, double refresh_hertz) {
    uint16_t *interval = metrics->interval, *cpu = metrics->cpu, *gpu = metrics->gpu;
    uint16_t *simulation = metrics->simulation, *render = metrics->render;
    size_t capacity = metrics->capacity, split_capacity = metrics->split_capacity;

    memset(metrics, 0, sizeof(*metrics));
    metrics->interval = interval; metrics->cpu = cpu; metrics->gpu = gpu; metrics->capacity = capacity;
    metrics->simulation = simulation; metrics->render = render; metrics->split_capacity = split_capacity;
    metrics->refresh_hertz = refresh_hertz;
    metrics->footprint_first = metrics->footprint_last = metrics->footprint_peak = metrics->left_least = -1;
    metrics->thermal_first = metrics->thermal_worst = metrics->thermal_last = BENCHMARK_THERMAL_UNKNOWN;
}

static uint16_t tenths(double milliseconds) {
    double value = floor(milliseconds * 10.0 + 0.5);

    return value < 0.0 || value != value ? 0 : value > 65535.0 ? 65535 : (uint16_t)value;
}

void benchmark_metrics_frame(struct benchmark_metrics *metrics, double interval, double cpu, double gpu) {
    benchmark_metrics_frame_split(metrics, interval, cpu, gpu, -1.0, -1.0, 0);
}

void benchmark_metrics_frame_split(struct benchmark_metrics *metrics, double interval, double cpu, double gpu,
    double simulation, double render, int ticks) {
    if (simulation >= 0.0 && metrics->split_frames == metrics->split_capacity) {
        size_t capacity = metrics->split_capacity ? metrics->split_capacity * 2 : 8192;
        uint16_t *a = realloc(metrics->simulation, capacity * sizeof(*a));
        uint16_t *b = a ? realloc(metrics->render, capacity * sizeof(*b)) : NULL;

        if (a) metrics->simulation = a;
        if (b) { metrics->render = b; metrics->split_capacity = capacity; }
    }
    if (simulation >= 0.0 && metrics->split_frames < metrics->split_capacity) {
        metrics->simulation[metrics->split_frames] = tenths(simulation);
        metrics->render[metrics->split_frames] = tenths(render);
        metrics->split_frames++;
        if (ticks > 0) metrics->ticks += (unsigned long)ticks;
    }
    if (metrics->frames == metrics->capacity) {
        size_t capacity = metrics->capacity ? metrics->capacity * 2 : 8192;
        uint16_t *a = realloc(metrics->interval, capacity * sizeof(*a));
        uint16_t *b = a ? realloc(metrics->cpu, capacity * sizeof(*b)) : NULL;
        uint16_t *c = b ? realloc(metrics->gpu, capacity * sizeof(*c)) : NULL;

        if (a) metrics->interval = a;
        if (b) metrics->cpu = b;
        if (!c) return;   /* out of memory: the run keeps the frames it has */
        metrics->gpu = c;
        metrics->capacity = capacity;
    }
    if (interval >= BENCHMARK_STALL_MILLISECONDS) {
        if (metrics->stall_count < BENCHMARK_STALLS_KEPT) {
            metrics->stalls[metrics->stall_count].at = metrics->seconds;
            metrics->stalls[metrics->stall_count].milliseconds = interval;
        }
        metrics->stall_count++;
    }
    metrics->interval[metrics->frames] = tenths(interval);
    metrics->cpu[metrics->frames] = tenths(cpu);
    metrics->gpu[metrics->frames] = tenths(gpu);
    metrics->frames++;
    if (interval > 0.0) metrics->seconds += interval / 1000.0;
}

void benchmark_metrics_sample(struct benchmark_metrics *metrics, long footprint, long left, int thermal) {
    if (footprint >= 0) {
        if (metrics->footprint_first < 0) metrics->footprint_first = footprint;
        metrics->footprint_last = footprint;
        if (footprint > metrics->footprint_peak) metrics->footprint_peak = footprint;
    }
    if (left >= 0 && (metrics->left_least < 0 || left < metrics->left_least)) metrics->left_least = left;
    if (thermal != BENCHMARK_THERMAL_UNKNOWN) {
        if (metrics->thermal_first == BENCHMARK_THERMAL_UNKNOWN) metrics->thermal_first = thermal;
        if (thermal > metrics->thermal_worst) metrics->thermal_worst = thermal;
        if (thermal != metrics->thermal_last && metrics->thermal_last != BENCHMARK_THERMAL_UNKNOWN) {
            if (metrics->thermal_change_count < BENCHMARK_THERMAL_CHANGES_KEPT) {
                metrics->thermal_changes[metrics->thermal_change_count].at = metrics->seconds;
                metrics->thermal_changes[metrics->thermal_change_count].state = thermal;
            }
            metrics->thermal_change_count++;
        }
        metrics->thermal_last = thermal;
    }
    metrics->samples++;
}

void benchmark_metrics_compile(struct benchmark_metrics *metrics, double milliseconds) {
    metrics->compiles++;
    metrics->compile_milliseconds += milliseconds;
    if (milliseconds > metrics->compile_longest) metrics->compile_longest = milliseconds;
}

long benchmark_refreshes(double interval, double refresh_hertz) {
    long refreshes = refresh_hertz > 0.0 ? lround(interval * refresh_hertz / 1000.0) : 1;

    return refreshes < 1 ? 1 : refreshes;
}

static int compare_tenths(const void *a, const void *b) {
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* nearest-rank percentiles of count values, which sorted may reorder */
static void distribution(uint16_t *sorted, size_t count, struct benchmark_distribution *out) {
    double total = 0.0;
    size_t index;

    memset(out, 0, sizeof(*out));
    if (!count) return;
    qsort(sorted, count, sizeof(*sorted), compare_tenths);
    for (index = 0; index < count; index++) total += sorted[index];
    out->mean = total / (double)count / 10.0;
    out->median = sorted[(count - 1) / 2] / 10.0;
    out->high = sorted[(size_t)ceil(0.95 * (double)count) - 1] / 10.0;
    out->higher = sorted[(size_t)ceil(0.99 * (double)count) - 1] / 10.0;
    out->worst = sorted[count - 1] / 10.0;
}

void benchmark_metrics_summarize(const struct benchmark_metrics *metrics, struct benchmark_summary *summary) {
    uint16_t *copy;
    size_t index;

    memset(summary, 0, sizeof(*summary));
    summary->frames = metrics->frames;
    summary->seconds = metrics->seconds;
    summary->refresh_hertz = metrics->refresh_hertz;
    summary->stalls = metrics->stall_count;
    if (!metrics->frames) return;
    summary->frames_a_second = metrics->seconds > 0.0 ? (double)metrics->frames / metrics->seconds : 0.0;
    for (index = 0; index < metrics->frames; index++) {
        long refreshes = benchmark_refreshes(metrics->interval[index] / 10.0, metrics->refresh_hertz);

        summary->shown[refreshes > 5 ? 5 : refreshes]++;
    }
    summary->on_time_full = (double)summary->shown[1] / (double)metrics->frames;
    summary->on_time_half = (double)(summary->shown[1] + summary->shown[2]) / (double)metrics->frames;
    copy = malloc(metrics->frames * sizeof(*copy));
    if (!copy) return;
    memcpy(copy, metrics->interval, metrics->frames * sizeof(*copy));
    distribution(copy, metrics->frames, &summary->interval);
    memcpy(copy, metrics->cpu, metrics->frames * sizeof(*copy));
    distribution(copy, metrics->frames, &summary->cpu);
    memcpy(copy, metrics->gpu, metrics->frames * sizeof(*copy));
    distribution(copy, metrics->frames, &summary->gpu);
    free(copy);
    summary->split_frames = metrics->split_frames;
    if (!metrics->split_frames) return;
    summary->ticks_per_frame = (double)metrics->ticks / (double)metrics->split_frames;
    copy = malloc(metrics->split_frames * sizeof(*copy));
    if (!copy) return;
    memcpy(copy, metrics->simulation, metrics->split_frames * sizeof(*copy));
    distribution(copy, metrics->split_frames, &summary->simulation_cpu);
    memcpy(copy, metrics->render, metrics->split_frames * sizeof(*copy));
    distribution(copy, metrics->split_frames, &summary->render_cpu);
    free(copy);
}

char *benchmark_base64(const void *bytes, size_t size) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *in = bytes;
    char *text = malloc((size + 2) / 3 * 4 + 1), *out = text;
    size_t index;

    if (!text) return NULL;
    for (index = 0; index < size; index += 3) {
        unsigned long word = (unsigned long)in[index] << 16;

        if (index + 1 < size) word |= (unsigned long)in[index + 1] << 8;
        if (index + 2 < size) word |= in[index + 2];
        *out++ = digits[word >> 18 & 63];
        *out++ = digits[word >> 12 & 63];
        *out++ = index + 1 < size ? digits[word >> 6 & 63] : '=';
        *out++ = index + 2 < size ? digits[word & 63] : '=';
    }
    *out = 0;
    return text;
}

/* text that grows; a failed allocation leaves it NULL and later appends do nothing */
struct text { char *bytes; size_t length, capacity; int failed; };

static void append(struct text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void append(struct text *text, const char *format, ...) {
    va_list arguments;
    int needed;

    if (text->failed) return;
    va_start(arguments, format);
    needed = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    if (needed < 0) { text->failed = 1; return; }
    if (text->length + (size_t)needed + 1 > text->capacity) {
        size_t capacity = (text->length + (size_t)needed + 1) * 2;
        char *bytes = realloc(text->bytes, capacity);

        if (!bytes) { text->failed = 1; return; }
        text->bytes = bytes; text->capacity = capacity;
    }
    va_start(arguments, format);
    vsnprintf(text->bytes + text->length, text->capacity - text->length, format, arguments);
    va_end(arguments);
    text->length += (size_t)needed;
}

static void append_distribution(struct text *text, const char *name, const struct benchmark_distribution *d) {
    append(text, "\"%s\":{\"mean\":%.2f,\"median\":%.1f,\"p95\":%.1f,\"p99\":%.1f,\"worst\":%.1f},", name, d->mean, d->median,
        d->high, d->higher, d->worst);
}

static void append_frames(struct text *text, const char *name, const uint16_t *values, size_t count) {
    unsigned char *bytes = malloc(count * 2 + 1);
    char *encoded;
    size_t index;

    if (!bytes) { text->failed = 1; return; }
    for (index = 0; index < count; index++) {
        bytes[index * 2] = (unsigned char)(values[index] & 255);
        bytes[index * 2 + 1] = (unsigned char)(values[index] >> 8);
    }
    encoded = benchmark_base64(bytes, count * 2);
    free(bytes);
    if (!encoded) { text->failed = 1; return; }
    append(text, "\"%s\":\"%s\"", name, encoded);
    free(encoded);
}

char *benchmark_metrics_json(const struct benchmark_metrics *metrics) {
    struct benchmark_summary summary;
    struct text text = { 0 };
    size_t index, kept;

    benchmark_metrics_summarize(metrics, &summary);
    append(&text, "\"frames\":%zu,\"seconds\":%.2f,\"frames_a_second\":%.2f,\"refresh_hertz\":%.2f,", summary.frames,
        summary.seconds, summary.frames_a_second, summary.refresh_hertz);
    append_distribution(&text, "interval_ms", &summary.interval);
    append_distribution(&text, "cpu_ms", &summary.cpu);
    append_distribution(&text, "gpu_ms", &summary.gpu);
    if (summary.split_frames) {
        append_distribution(&text, "simulation_cpu_ms", &summary.simulation_cpu);
        append_distribution(&text, "render_cpu_ms", &summary.render_cpu);
        append(&text, "\"ticks_per_frame\":%.2f,", summary.ticks_per_frame);
    } else append(&text, "\"simulation_cpu_ms\":null,\"render_cpu_ms\":null,\"ticks_per_frame\":null,");
    append(&text, "\"shown_refreshes\":[%lu,%lu,%lu,%lu,%lu],\"on_time_full\":%.4f,\"on_time_half\":%.4f,", summary.shown[1],
        summary.shown[2], summary.shown[3], summary.shown[4], summary.shown[5], summary.on_time_full, summary.on_time_half);
    append(&text, "\"memory_mb\":{\"footprint_first\":%ld,\"footprint_last\":%ld,\"footprint_peak\":%ld,\"left_least\":%ld},",
        metrics->footprint_first, metrics->footprint_last, metrics->footprint_peak, metrics->left_least);
    append(&text, "\"thermal\":{\"first\":%d,\"worst\":%d,\"last\":%d,\"changes\":[", metrics->thermal_first, metrics->thermal_worst,
        metrics->thermal_last);
    kept = metrics->thermal_change_count < BENCHMARK_THERMAL_CHANGES_KEPT ? metrics->thermal_change_count :
        BENCHMARK_THERMAL_CHANGES_KEPT;
    for (index = 0; index < kept; index++)
        append(&text, "%s[%.1f,%d]", index ? "," : "", metrics->thermal_changes[index].at, metrics->thermal_changes[index].state);
    append(&text, "],\"change_count\":%zu},", metrics->thermal_change_count);
    append(&text, "\"shader_compiles\":{\"count\":%lu,\"total_ms\":%.1f,\"longest_ms\":%.1f},", metrics->compiles,
        metrics->compile_milliseconds, metrics->compile_longest);
    append(&text, "\"stalls\":{\"count\":%zu,\"over_ms\":%.0f,\"first\":[", metrics->stall_count, BENCHMARK_STALL_MILLISECONDS);
    kept = metrics->stall_count < BENCHMARK_STALLS_KEPT ? metrics->stall_count : BENCHMARK_STALLS_KEPT;
    for (index = 0; index < kept; index++)
        append(&text, "%s[%.2f,%.1f]", index ? "," : "", metrics->stalls[index].at, metrics->stalls[index].milliseconds);
    append(&text, "]},\"frame_encoding\":\"base64 of little-endian uint16, tenths of a millisecond\",");
    append_frames(&text, "frame_interval", metrics->interval, metrics->frames);
    append(&text, ",");
    append_frames(&text, "frame_cpu", metrics->cpu, metrics->frames);
    append(&text, ",");
    append_frames(&text, "frame_gpu", metrics->gpu, metrics->frames);
    if (text.failed) { free(text.bytes); return NULL; }
    return text.bytes;
}
