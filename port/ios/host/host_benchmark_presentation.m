#import "host_benchmark_presentation.h"
#include "benchmark_presentation.h"
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <zlib.h>

/* This owner outlives every Metal handler. No handler retains storage: its
   generation is checked under the mutex before the collector is touched. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct benchmark_presentation collector;
static uint64_t generation;
static int active, admitted, draining, allocation_failed;
static double drain_deadline;
static enum benchmark_scene_kind kind;
static enum host_presentation_capability configured, capability;
static int configured_sync = -1, configured_tearing = -1, synchronized_display, tearing, policy_changed;
/* Process diagnostics, never retroactively attributed to an immutable run. */
static uint64_t rejected_generation, rejected_after_finalize;
static NSDictionary *completed_report;
static int async_pending;

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}
void host_benchmark_presentation_configure(enum host_presentation_capability value, int sync, int tear) {
    pthread_mutex_lock(&mutex);
    if (active && (capability != value || synchronized_display != sync || tearing != tear)) policy_changed = 1;
    configured = value; configured_sync = sync; configured_tearing = tear;
    pthread_mutex_unlock(&mutex);
}
void host_benchmark_presentation_begin(enum benchmark_scene_kind value, size_t capacity) {
    pthread_mutex_lock(&mutex);
    /* host_next never begins while an asynchronous drain is pending. Free the
       old storage first so two maximum-size collectors never coexist. */
    if (draining || async_pending) { pthread_mutex_unlock(&mutex); return; }
    benchmark_presentation_free(&collector);
    active = 1; admitted = 0; kind = value; policy_changed = 0;
    capability = configured; synchronized_display = configured_sync; tearing = configured_tearing;
    allocation_failed = generation == UINT64_MAX;
    if (!allocation_failed) {
        generation++;
        if (capability == HOST_PRESENTATION_METAL)
            allocation_failed = !benchmark_presentation_reset(&collector, generation, capacity);
    }
    pthread_mutex_unlock(&mutex);
}
void host_benchmark_presentation_window(void) {
    pthread_mutex_lock(&mutex);
    /* The first call can be the legacy closed-cutscene notification. Later
       toggles must not erase the measured-run admission. */
    if (active && !draining) admitted = 1;
    pthread_mutex_unlock(&mutex);
}
struct host_benchmark_presentation_token host_benchmark_presentation_reserve(double submitted, int cutscene) {
    struct benchmark_presentation_token result = { 0 };
    pthread_mutex_lock(&mutex);
    if (active && admitted && !draining && !allocation_failed && capability == HOST_PRESENTATION_METAL) {
        if (kind == BENCHMARK_SCENE_GAMEPLAY && cutscene) benchmark_presentation_close_segment(&collector);
        else {
            if (!collector.segment_open) benchmark_presentation_open(&collector, collector.segment + 1);
            result = benchmark_presentation_reserve(&collector, submitted);
        }
    }
    pthread_mutex_unlock(&mutex);
    return (struct host_benchmark_presentation_token){ result.generation, result.index };
}
void host_benchmark_presentation_complete(struct host_benchmark_presentation_token token, double presented) {
    pthread_mutex_lock(&mutex);
    if (!token.generation || token.generation != generation) rejected_generation++;
    else if (!active || !collector.records) rejected_after_finalize++;
    else {
        benchmark_presentation_complete(&collector,
            (struct benchmark_presentation_token){ token.generation, token.index }, presented);
        pthread_cond_signal(&changed);
    }
    pthread_mutex_unlock(&mutex);
}
static void close_locked(double timeout) {
    if (!draining) drain_deadline = monotonic_seconds() +
        (isfinite(timeout) && timeout > 0 ? fmin(timeout, 1.0) : 0);
    admitted = 0;
    draining = 1;
    benchmark_presentation_close(&collector);
}
static id number(double value) { return isfinite(value) ? @(value) : NSNull.null; }
static NSString *capability_name(enum host_presentation_capability value) {
    switch (value) {
        case HOST_PRESENTATION_METAL: return @"metal_drawable_presented_time";
        case HOST_PRESENTATION_UNSUPPORTED: return @"unsupported_renderer";
        case HOST_PRESENTATION_SIMULATOR: return @"simulator";
        case HOST_PRESENTATION_VISIONOS: return @"visionos_compositor_unavailable";
        default: return @"not_configured";
    }
}
/* Stream small JSON rows through zlib so full-capacity reports do not create
   hundreds of thousands of retained Foundation objects or an uncompressed copy. */
static BOOL compress_bytes(z_stream *stream, NSMutableData *output, NSData *input, int flush) {
    unsigned char buffer[16384];
    stream->next_in = (Bytef *)input.bytes; stream->avail_in = (uInt)input.length;
    int result;
    do {
        stream->next_out = buffer; stream->avail_out = sizeof(buffer);
        result = deflate(stream, flush);
        if (result != Z_OK && result != Z_STREAM_END) return NO;
        [output appendBytes:buffer length:sizeof(buffer) - stream->avail_out];
    } while (stream->avail_in || !stream->avail_out || (flush == Z_FINISH && result != Z_STREAM_END));
    return YES;
}
static NSDictionary *trace_report(const struct benchmark_presentation *snapshot) {
    z_stream stream = { 0 };
    if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) return @{ @"status": @"compression_failed" };
    NSMutableData *output = [NSMutableData data];
    BOOL ok = compress_bytes(&stream, output, [@"[" dataUsingEncoding:NSUTF8StringEncoding], Z_NO_FLUSH);
    for (size_t i = 0; ok && i < snapshot->count; i++) { @autoreleasepool {
        const struct benchmark_presentation_record *r = &snapshot->records[i];
        NSString *status = !r->received ? @"missing" : !isfinite(r->presented) || r->presented < 0 ? @"invalid" :
            r->presented == 0 ? @"unpresented" : r->order_error ? @"order_error" : @"positive";
        NSString *span = r->span == BENCHMARK_PRESENTATION_INTERVAL ? @"interval" :
            r->span == BENCHMARK_PRESENTATION_UNKNOWN_GAP ? @"unknown_gap" :
            r->span == BENCHMARK_PRESENTATION_INVALID_GAP ? @"invalid_gap" : @"none";
        NSArray *row = @[ @(i), @(r->segment), number(r->submitted), r->received ? number(r->presented) : NSNull.null,
            @(r->received != 0), status, span, r->span ? @(r->previous_index) : NSNull.null,
            r->span && r->span != BENCHMARK_PRESENTATION_INVALID_GAP ? number(r->milliseconds) : NSNull.null ];
        if (i) ok = compress_bytes(&stream, output, [@"," dataUsingEncoding:NSUTF8StringEncoding], Z_NO_FLUSH);
        NSData *json = [NSJSONSerialization dataWithJSONObject:row options:0 error:NULL];
        ok = ok && json && compress_bytes(&stream, output, json, Z_NO_FLUSH);
    } }
    ok = ok && compress_bytes(&stream, output, [@"]" dataUsingEncoding:NSUTF8StringEncoding], Z_FINISH);
    deflateEnd(&stream);
    if (!ok) return @{ @"status": @"compression_failed" };
    return @{ @"status": @"ok", @"format": @"json_rows_v1", @"encoding": @"zlib+base64",
        @"columns": @[ @"frame", @"segment", @"submitted_seconds", @"presented_seconds", @"received", @"status",
            @"span", @"previous_frame", @"milliseconds" ], @"data": [output base64EncodedStringWithOptions:0] };
}
NSDictionary *host_benchmark_presentation_finish(double timeout) {
    struct benchmark_presentation snapshot = { 0 };
    pthread_mutex_lock(&mutex);
    close_locked(timeout);
    BOOL main_thread = NSThread.isMainThread;
    double deadline = drain_deadline;
    while (!main_thread && benchmark_presentation_pending(&collector)) {
        double left = deadline - monotonic_seconds();
        if (left <= 0) break;
        struct timespec duration = { (time_t)left, (long)((left - floor(left)) * 1e9) };
        /* Releases the callback mutex; relative wait uses elapsed time, not
           wall-clock adjustments. No UIKit or renderer lock is held here. */
        pthread_cond_timedwait_relative_np(&changed, &mutex, &duration);
    }
    BOOL missing = benchmark_presentation_pending(&collector) != 0;
    benchmark_presentation_seal(&collector, missing && !main_thread);
    snapshot = collector;
    memset(&collector, 0, sizeof(collector));
    active = 0; draining = 0;
    enum host_presentation_capability cap = capability;
    int failed = allocation_failed, sync = synchronized_display, tear = tearing, policy = policy_changed;
    pthread_mutex_unlock(&mutex);

    const struct benchmark_presentation_summary *s = &snapshot.summary;
    NSString *status = cap != HOST_PRESENTATION_METAL ? @"unsupported" : failed ? @"allocation_failed" :
        s->missing || s->overflow || s->invalid || s->order_errors || s->invalid_spans || policy ? @"incomplete" :
        !s->expected ? @"no_samples" : @"complete";
    NSDictionary *trace = trace_report(&snapshot);
    if (![trace[@"status"] isEqual:@"ok"] && cap == HOST_PRESENTATION_METAL) status = @"incomplete";
    NSDictionary *report = @{ @"supported": @(cap == HOST_PRESENTATION_METAL), @"capability": capability_name(cap),
        @"status": status, @"drain_status": missing ? (main_thread ? @"skipped_main_thread" : @"timeout") : @"complete",
        @"expected": @(s->expected), @"stored": @(s->stored), @"received": @(s->received), @"positive": @(s->positive),
        @"unpresented": @(s->unpresented), @"missing_at_finalize": @(s->missing), @"overflow": @(s->overflow),
        @"invalid_timestamps": @(s->invalid), @"invalid_spans": @(s->invalid_spans), @"duplicates": @(s->duplicates),
        @"bad_tokens": @(s->bad_tokens), @"order_errors": @(s->order_errors), @"interval_count": @(s->intervals),
        @"stalls_50ms": @(s->stalls), @"unknown_gaps": @(s->unknown_gaps), @"unknown_stalls_50ms": @(s->unknown_stalls),
        @"callback_coverage": @(s->callback_coverage), @"positive_coverage": @(s->positive_coverage),
        @"interval_ms": s->intervals ? @{ @"mean": number(s->interval.mean), @"median": number(s->interval.median),
            @"p95": number(s->interval.high), @"p99": number(s->interval.higher), @"max": number(s->interval.worst) } : NSNull.null,
        @"display_synchronized": sync < 0 ? NSNull.null : @(sync != 0),
        @"allows_tearing": tear < 0 ? NSNull.null : @(tear != 0), @"policy_changed": @(policy != 0),
        @"interpretation": @"drawable_timestamps_not_whole_refresh_images", @"trace": trace };
    benchmark_presentation_free(&snapshot);
    return report;
}
void host_benchmark_presentation_end_async(void) {
    pthread_mutex_lock(&mutex);
    if (async_pending) { pthread_mutex_unlock(&mutex); return; }
    close_locked(0.25); async_pending = 1; completed_report = nil;
    pthread_mutex_unlock(&mutex);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSDictionary *report = host_benchmark_presentation_finish(0.25);
        pthread_mutex_lock(&mutex);
        completed_report = report;
        pthread_mutex_unlock(&mutex);
    });
}
NSDictionary *host_benchmark_presentation_take_report(void) {
    pthread_mutex_lock(&mutex);
    NSDictionary *report = completed_report;
    if (report) { completed_report = nil; async_pending = 0; }
    pthread_mutex_unlock(&mutex);
    return report;
}
NSDictionary *host_benchmark_presentation_diagnostics(void) {
    pthread_mutex_lock(&mutex);
    NSDictionary *result = @{ @"rejected_generation": @(rejected_generation), @"rejected_after_finalize": @(rejected_after_finalize) };
    pthread_mutex_unlock(&mutex);
    return result;
}
NSDictionary *host_benchmark_measurement_record(NSDictionary *record, NSDictionary *presentation) {
    NSMutableDictionary *result = [record mutableCopy];
    result[@"measurement_semantics_version"] = presentation ? @2 : @1;
    result[@"measurement_semantics"] = @{ @"interval_ms": @"submission_timing", @"frame_interval": @"submission_timing", @"frames_a_second": @"submission_throughput",
        @"shown_refreshes": @"inferred_refresh_bins_from_submission_intervals", @"on_time_full": @"inferred_from_submission_intervals", @"on_time_half": @"inferred_from_submission_intervals",
        @"cpu_ms": @"cpu_duration", @"gpu_ms": @"gpu_duration", @"decision_basis": @"submission_timing" };
    result[@"presentation"] = presentation ?: @{ @"supported": NSNull.null, @"capability": @"unknown",
        @"status": @"unavailable_recovered_run" };
    return result;
}
